// macOS (Apple Silicon / Intel) build. Nothing in here is portable to Windows
// on purpose: the diagnostics use CoreGraphics event-source timing.
#if !defined(__APPLE__)
    #error "This mod is tailored for macOS only."
#endif

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace geode::prelude;

namespace jw {
    // Diagnostics only. No gameplay decision reads the wall clock any more.
    using Clock = std::chrono::steady_clock;

    constexpr int    kButtonJump = 1;
    constexpr double kJumpVel    = 5.0;  // old absolute test, kept for log comparison only
    constexpr double kRegisterDeltaV = 2.0; // per-step upward velocity change that proves a press took (one step of gravity is ~0.25)

    // Sub-tick hold cushion: a cushioned press stays down at least this many physics steps.
    constexpr int    kMinHoldSteps = 3;

    // Ceiling for the "stay on the orb" extension (see runBuffer): 0.5 s of
    // game time. Only reachable if the player stays inside an orb's overlap,
    // i.e. a frozen/stuck state; normal overlaps end long before this.
    constexpr int    kHardCapSteps = 120;

    // Nominal physics rate. The real rate is measured at runtime (g_tps) and
    // used to turn the window-ms setting into a tick budget when a click arrives.
    constexpr double kNominalTps = 240.0;

    // Wave: every input edge (press or release) must stay live for at least
    // this many physics ticks. Two edges that arrive in the SAME tick (a fast
    // tap: press+release, or release+press) would otherwise cancel out and the
    // wave would never change direction. The second edge is pushed to the
    // next tick instead, so the tap is preserved.
    constexpr uint64_t kWaveMinGapSteps = 1;

    // One cushioned press per player side.
    struct Side {
        bool     active         = false; // a press we are streaming/cushioning is in flight
        bool     releaseQueued  = false; // player let go; the release sits in the tick queue
        bool     releaseOk      = false; // gate: the queued release may now be delivered
        bool     registered     = false; // the engine acted on the press
        bool     pressGround    = false; // diagnostics
        int      pressRings     = 0;     // diagnostics
        // Snapshot from the previous step, used to detect that the press took.
        bool     prevGround     = false;
        bool     prevUpsideDown = false;
        int      prevRings      = 0;
        double   prevUp         = 0.0;
        uint64_t pressSeq       = 0;     // step counter when the press went in
        int      heldSteps      = 0;     // physics steps the press has been live
        int      ackSteps       = 0;     // ticks between the press and the engine acting on it
        int      maxSteps       = 0;     // step budget (stutter-proof, game time)
        int      streamed       = 0;     // extra presses streamed while waiting
        int      extSteps       = 0;     // steps spent past the budget because an orb overlap was live
        Clock::time_point clickTime{};   // diagnostics only
    };

    struct State {
        bool injecting = false;
        bool anyActive = false;
        Side sides[2];
    };

    // One ordered input timeline, clocked in raw physics ticks.
    struct Event {
        uint64_t dueStep;
        uint64_t seq;
        bool     down;
        bool     p1;
        bool     cushion; // release gated by the side's releaseOk
    };

    inline State& state() {
        static State s;
        return s;
    }

    inline bool g_enabled      = true;
    inline bool g_practiceOnly = false;
    inline int  g_windowMs     = 80;   // converted to a step budget per click
    inline int  g_waveDelay    = 0;    // setting is in frames; converted to physics ticks
    inline bool g_waveGuard    = false;
    inline bool g_stepHookSeen = false;
    inline bool g_inTick       = false; // true only while the physics-step hook is running the game's input queue
    inline bool g_waveDown[2]  = {false, false};
    inline int  g_guardLogs    = 0;
    inline double g_stepsPerFrame = 4.0; // measured, read-only use (wave delay conversion)
    inline double g_tps = kNominalTps;   // measured ticks per second (hook calls per wall second)
    inline int    g_waveEvents = 0, g_waveSpaced = 0; // wave inputs seen / saved from a same-tick collision

    inline double g_modUs = 0.0; // microseconds this mod spent since the last frame (diagnostic)
    inline std::vector<Event> g_queue;
    inline uint64_t g_seq      = 0;    // event ordering
    inline uint64_t g_stepSeq  = 0;    // physics-step counter (the only clock gameplay logic uses)
    inline uint64_t g_lastDue[2] = {0, 0};
    inline uint64_t g_lastDelivered[2] = {0, 0}; // tick of the last input actually delivered, per side

    inline double msBetween(Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    inline int sideIdx(bool p1) { return p1 ? 0 : 1; }

    inline void refreshLastDue() {
        g_lastDue[0] = 0;
        g_lastDue[1] = 0;
        for (auto& ev : g_queue) {
            auto& d = g_lastDue[sideIdx(ev.p1)];
            if (ev.dueStep > d) d = ev.dueStep;
        }
    }

    inline void resetSides() {
        auto& s = state();
        s.sides[0] = Side{};
        s.sides[1] = Side{};
        s.injecting = false;
        s.anyActive = false;
    }

    inline void reset() {
        resetSides();
        g_queue.clear();
        refreshLastDue();
        g_lastDelivered[0] = g_lastDelivered[1] = 0;
        g_waveDown[0] = g_waveDown[1] = false;
    }

    inline bool assistActive() {
        auto* pl = PlayLayer::get();
        if (!g_enabled || !pl) return false;
        if (g_practiceOnly && !pl->m_isPracticeMode) return false;
        return true;
    }

    // One-off-action modes: cube, ball, robot, spider.
    // Ship, UFO, wave and swing never get a cushion.
    inline bool isCubeLike(PlayerObject* p) {
        return p && !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
    }

    inline int ringCount(PlayerObject* p) {
        return (p && p->m_touchingRings) ? static_cast<int>(p->m_touchingRings->count()) : 0;
    }

    inline double upVel(PlayerObject* p) {
        const double vel = static_cast<double>(p->m_yVelocity);
        return p->m_isUpsideDown ? -vel : vel;
    }

    // Used for logging only: does the player's state look like a jump just fired?
    inline bool jumpFired(PlayerObject* p, bool wasGround) {
        return (wasGround && !p->m_isOnGround) || upVel(p) > kJumpVel;
    }

    // Diagnostic (macOS): how old is the physical key press when the game receives it?
    inline double clickAgeMs(bool mouse) {
        const auto src = kCGEventSourceStateHIDSystemState;
        const double a = CGEventSourceSecondsSinceLastEventType(
            src, mouse ? kCGEventLeftMouseDown : kCGEventKeyDown);
        return a < 0.0 ? -1.0 : a * 1000.0;
    }

    inline void logReplaced(const Side& w) {
        static int n = 0;
        if (w.active && n < 100) {
            ++n;
            log::info("JW: previous cushion replaced by a new click (release queued={})", w.releaseQueued);
        }
    }

    // Diagnostic: which path did a real click take?
    inline int g_cntTotal = 0, g_cntCushioned = 0, g_cntUnsupported = 0, g_cntInactive = 0;
    inline void logDecision(const char* why, PlayerObject* p) {
        const bool cushioned = std::strncmp(why, "CUSHIONED", 9) == 0;
        if (cushioned)                              ++g_cntCushioned;
        else if (std::strstr(why, "not supported")) ++g_cntUnsupported;
        else                                        ++g_cntInactive;
        ++g_cntTotal;
        if (g_cntTotal % 25 == 0) {
            log::info("JW: summary after {} clicks: cushioned={} unsupported-mode={} inactive={}",
                g_cntTotal, g_cntCushioned, g_cntUnsupported, g_cntInactive);
        }

        static int detail = 0;
        if (cushioned) { if (g_cntCushioned > 300) return; }
        else           { if (detail >= 40) return; ++detail; }

        log::info("JW: click -> {} | stepBudgetFromMs={} ground={} rings={} ship={} ufo={} ball={} wave={} robot={} spider={} swing={}",
            why, g_windowMs,
            p ? p->m_isOnGround : false, ringCount(p),
            p ? p->m_isShip : false, p ? p->m_isBird : false, p ? p->m_isBall : false,
            p ? p->m_isDart : false, p ? p->m_isRobot : false, p ? p->m_isSpider : false,
            p ? p->m_isSwing : false);
    }

    inline void updateLabel(PlayLayer* pl) {
        if (auto* label = pl->getChildByID("jw-label")) {
            label->setVisible(assistActive());
        }
    }

    // ---------------------------------------------------------------------
    // The physics-tick input queue
    // ---------------------------------------------------------------------

    // Send one virtual jump input to the game, bypassing our own intercept.
    inline void inject(GJBaseGameLayer* layer, bool down, bool p1) {
        auto& st = state();
        const bool was = st.injecting;
        st.injecting = true;
        layer->handleButton(down, kButtonJump, p1);
        st.injecting = was;
    }

    // Queue an input for a physics tick. Per side, due ticks never go
    // backwards, so presses and releases can never swap places.
    // minGap > 0: this input must be at least minGap ticks after the previous
    // input of the same side (queued or already delivered). Returns true when
    // that pushed it later than it would otherwise have been.
    inline bool enqueue(bool down, bool p1, uint64_t dueStep, bool cushion, uint64_t minGap = 0) {
        const int i = sideIdx(p1);
        if (dueStep < g_lastDue[i]) dueStep = g_lastDue[i];
        bool spaced = false;
        if (minGap > 0) {
            const uint64_t earliest = std::max(g_lastDue[i], g_lastDelivered[i]) + minGap;
            if (dueStep < earliest) { dueStep = earliest; spaced = true; }
        }
        g_lastDue[i] = dueStep;
        g_queue.push_back(Event{dueStep, ++g_seq, down, p1, cushion});
        return spaced;
    }

    // Wave delay is configured in frames; the queue counts physics ticks.
    inline uint64_t waveDelaySteps() {
        if (g_waveDelay <= 0) return 0;
        const long steps = std::lround(static_cast<double>(g_waveDelay) * g_stepsPerFrame);
        return static_cast<uint64_t>(std::max<long>(steps, g_waveDelay));
    }

    inline void recordOutcome(const Side& w);

    inline void deliver(GJBaseGameLayer* layer, const Event& ev) {
        auto& st = state();
        const int i = sideIdx(ev.p1);
        if (ev.cushion && !ev.down) {
            auto& w = st.sides[i];
            if (w.active) {
                recordOutcome(w);
                PlayerObject* p = ev.p1 ? layer->m_player1 : layer->m_player2;
                static int relLogs = 0;
                if (relLogs < 300) {
                    ++relLogs;
                    log::info("JW: cushion release via {} (held {}/{} ticks, acked after {} ticks, streamed {} extra presses, {} ticks past budget on an orb, registered: {}, pressed on ground={} rings={}, now ground={} rings={})",
                        w.registered ? "engine ack" : "budget end (jump never became legal)", w.heldSteps, w.maxSteps, w.ackSteps,
                        w.streamed, w.extSteps, w.registered, w.pressGround, w.pressRings,
                        p ? p->m_isOnGround : false, ringCount(p));
                }
            }
            st.sides[i] = Side{};
            st.anyActive = st.sides[0].active || st.sides[1].active;
        }
        g_lastDelivered[sideIdx(ev.p1)] = g_stepSeq;
        inject(layer, ev.down, ev.p1);
    }

    // Deliver what is due on this physics tick, in order. force: deliver
    // everything now. onlySide >= 0: touch only that side's events.
    inline void drain(GJBaseGameLayer* layer, bool force, int onlySide = -1) {
        if (g_queue.empty() || !layer) return;
        auto& st = state();

        std::vector<Event> pending = std::move(g_queue);
        g_queue.clear();
        std::stable_sort(pending.begin(), pending.end(), [](const Event& a, const Event& b) {
            return a.dueStep != b.dueStep ? a.dueStep < b.dueStep : a.seq < b.seq;
        });

        std::vector<Event> keep;
        bool blocked[2] = {false, false};
        for (const auto& ev : pending) {
            const int i = sideIdx(ev.p1);
            if (onlySide >= 0 && i != onlySide) { keep.push_back(ev); continue; }

            bool ready = force || (!blocked[i] && ev.dueStep <= g_stepSeq);
            if (ready && !force && ev.cushion) {
                const auto& w = st.sides[i];
                if (w.active && !w.releaseOk) ready = false; // engine has not acknowledged yet
            }
            if (!ready) { blocked[i] = true; keep.push_back(ev); continue; }
            deliver(layer, ev);
        }
        keep.insert(keep.end(), g_queue.begin(), g_queue.end());
        g_queue = std::move(keep);
        refreshLastDue();
    }

    inline void pump(GJBaseGameLayer* layer)               { drain(layer, false); }
    inline void flushSide(GJBaseGameLayer* layer, bool p1) { drain(layer, true, sideIdx(p1)); }
    inline void flushAll(GJBaseGameLayer* layer)           { drain(layer, true); }

    inline void beginCushion(Side& w, PlayerObject* p) {
        w = Side{};
        w.active         = true;
        w.clickTime      = Clock::now();   // diagnostics only
        w.pressSeq       = g_stepSeq;
        w.maxSteps       = std::max(kMinHoldSteps,
                               static_cast<int>(std::lround(static_cast<double>(g_windowMs) * g_tps / 1000.0)));
        w.pressGround    = p->m_isOnGround;
        w.pressRings     = ringCount(p);
        w.prevGround     = p->m_isOnGround;
        w.prevUpsideDown = p->m_isUpsideDown;
        w.prevRings      = ringCount(p);
        w.prevUp         = upVel(p);
    }

    // ---------------------------------------------------------------------
    // Outcome statistics (the per-click logs are capped; these are not)
    // ---------------------------------------------------------------------
    inline int g_outGroundAck = 0, g_outGroundMiss = 0;   // pressed on ground / on an orb
    inline int g_outAirAck = 0,    g_outAirMiss = 0;      // pressed in the air with nothing in reach
    inline long g_airAckStepsSum = 0;
    inline int  g_airAckStepsMax = 0;
    inline int  g_savedClicks = 0;  // early taps the buffer carried to a legal tick
    inline long g_savedTicks  = 0;  // total ticks those taps were carried for

    inline void recordOutcome(const Side& w) {
        const bool air = !w.pressGround && w.pressRings == 0;
        if (air) {
            if (w.registered) { ++g_outAirAck; g_airAckStepsSum += w.ackSteps; g_airAckStepsMax = std::max(g_airAckStepsMax, w.ackSteps); }
            else              { ++g_outAirMiss; }
        } else {
            if (w.registered) ++g_outGroundAck; else ++g_outGroundMiss;
        }
        const int total = g_outGroundAck + g_outGroundMiss + g_outAirAck + g_outAirMiss;
        if (total % 10 == 0) {
            log::info("JW: outcomes after {}: ground/orb ack={} miss={} | early-air ack={} miss={} (ack after avg {:.1f} / max {} ticks, budget {} ticks, {} ticks saved in total)",
                total, g_outGroundAck, g_outGroundMiss, g_outAirAck, g_outAirMiss,
                g_outAirAck > 0 ? static_cast<double>(g_airAckStepsSum) / g_outAirAck : 0.0,
                g_airAckStepsMax, w.maxSteps, g_savedTicks);
        }
    }

    // ---------------------------------------------------------------------
    // Per-step supervision: stream the press until the engine acknowledges it
    // ---------------------------------------------------------------------
    //
    // Every physics step the press is sent again (handleButton(true)) until
    // the engine shows the press took: left the ground, gravity flipped, or an
    // upward velocity jump far larger than one step of gravity.
    //
    // IMPORTANT: a jump or orb fires INSIDE handleButton, so the comparison
    // snapshot must stay at its pre-press value until the check has run, and
    // the check runs again right after every streamed press. (Build 11
    // refreshed the snapshot too early and never saw a single ground jump.)
    //
    // Budget: normally the step budget derived from window-ms. If the player
    // is overlapping an orb (m_touchingRings) and the press is still
    // unacknowledged, the budget is ignored and the stream keeps going until
    // the engine acknowledges or the overlap ends (kHardCapSteps as a rail).
    inline void runBuffer(GJBaseGameLayer* layer) {
        auto& st = state();
        if (!st.anyActive) return;

        for (int i = 0; i < 2; ++i) {
            auto& w = st.sides[i];
            if (!w.active) continue;

            const bool p1 = (i == 0);
            PlayerObject* p = p1 ? layer->m_player1 : layer->m_player2;

            // Player gone or no longer in a supported mode: stop; a queued
            // release is no longer gated and goes out this step.
            if (!p || !isCubeLike(p)) {
                w = Side{};
                continue;
            }

            w.heldSteps = static_cast<int>(g_stepSeq - w.pressSeq);

            // Did the press take, judged against the snapshot? Landing is
            // excluded (ground contact zeroes velocity, which is not a jump).
            auto detect = [&]() {
                if (w.registered) return;
                const bool   onGround = p->m_isOnGround;
                const bool   left     = w.prevGround && !onGround;
                const bool   flipped  = p->m_isUpsideDown != w.prevUpsideDown;
                // A velocity change alone is NOT proof (pads, portals, slopes also
                // change it). It only counts together with real orb contact,
                // now or at the previous step; gravity orbs show up as `flipped`.
                const bool   orbContact = ringCount(p) > 0 || w.prevRings > 0;
                const bool   kick     = !onGround && orbContact
                                        && std::fabs(upVel(p) - w.prevUp) > kRegisterDeltaV;
                if (left || flipped || kick) {
                    w.registered = true;
                    w.ackSteps = static_cast<int>(g_stepSeq - w.pressSeq);
                    if (!w.pressGround && w.pressRings == 0) {   // an early tap: this is a saved click
                        ++g_savedClicks;
                        g_savedTicks += w.ackSteps;
                        static int savedLogs = 0;
                        if (savedLogs < 300) {
                            ++savedLogs;
                            log::info("JW: SAVED early click #{}: jump fired {} ticks ({:.1f} ms at {:.0f} TPS) after the press",
                                g_savedClicks, w.ackSteps, w.ackSteps * 1000.0 / g_tps, g_tps);
                        }
                    }
                }
            };

            detect();   // catches a press that took inside handleButton / the previous streamed press

            const bool overBudget = w.heldSteps >= w.maxSteps;
            const bool nearOrb    = ringCount(p) > 0;
            const bool extend     = !w.registered && overBudget && nearOrb && w.heldSteps < kHardCapSteps;

            // Continuous streaming: one more press on every sub-tick cycle
            // until acknowledged. The first press already went in with the click.
            if (!w.registered && w.heldSteps >= 1 && (!overBudget || extend)) {
                inject(layer, true, p1);
                ++w.streamed;
                if (overBudget) ++w.extSteps;
                detect();   // the streamed press may itself have fired the jump
            }

            // Only now roll the snapshot forward, for the next step's comparison.
            w.prevGround     = p->m_isOnGround;
            w.prevUpsideDown = p->m_isUpsideDown;
            w.prevRings      = ringCount(p);
            w.prevUp         = upVel(p);

            w.releaseOk = (w.registered && w.heldSteps >= kMinHoldSteps)
                       || (overBudget && !extend);

            // Player is still holding and we are done: nothing to release, stop tracking.
            if (!w.releaseQueued && w.releaseOk) {
                recordOutcome(w);
                static int doneLogs = 0;
                if (doneLogs < 400) {
                    ++doneLogs;
                    log::info("JW: press tracking ended while held (registered: {}, held {}/{} steps, streamed {}, extended {})",
                        w.registered, w.heldSteps, w.maxSteps, w.streamed, w.extSteps);
                }
                w = Side{};
            }
        }

        st.anyActive = st.sides[0].active || st.sides[1].active;
    }

    // Runs once per rendered frame. Diagnostics and UI only: it delivers no
    // input and drives no gameplay logic.
    inline void tick() {
        const auto tickStart = Clock::now();
        static auto lastFrame = Clock::now();
        static uint64_t lastSeq = g_stepSeq;
        const auto nowFrame = Clock::now();
        const double frameMs = msBetween(lastFrame, nowFrame);
        lastFrame = nowFrame;

        // Measure physics ticks per rendered frame (read-only; converts wave-delay frames to ticks).
        const uint64_t steps = g_stepSeq - lastSeq;
        lastSeq = g_stepSeq;
        if (steps > 0 && steps <= 64) {
            g_stepsPerFrame = std::clamp(g_stepsPerFrame * 0.9 + static_cast<double>(steps) * 0.1, 1.0, 16.0);
        }

        // Measure the real tick rate over ~1 s windows. Steps inside a frame run
        // back to back, so only an average over many frames is meaningful.
        {
            static double accWall = 0.0, accSteps = 0.0, idleMs = 0.0;
            static bool firstLogged = false;
            if (steps > 0 && steps <= 64) {
                accWall  += frameMs + idleMs;
                accSteps += static_cast<double>(steps);
                idleMs = 0.0;
                if (accWall >= 1000.0) {
                    const double tps = std::clamp(accSteps / accWall * 1000.0, 30.0, 2000.0);
                    const bool changed = std::fabs(tps - g_tps) / g_tps > 0.08;
                    if (changed) g_tps = std::round(tps);
                    if (changed || !firstLogged) {
                        firstLogged = true;
                        log::info("JW: measured tick rate {:.0f} TPS, using {:.0f} TPS for budgets (window {} ms = {} ticks)",
                            tps, g_tps, g_windowMs, static_cast<int>(std::lround(g_windowMs * g_tps / 1000.0)));
                    }
                    accWall = accSteps = 0.0;
                }
            } else if (steps == 0) {
                idleMs += frameMs;
                if (idleMs > 200.0) { idleMs = 0.0; accWall = accSteps = 0.0; } // paused or in a menu
            }
        }

        auto* pl = PlayLayer::get();
        if (!pl) {
            reset();
            return;
        }

        updateLabel(pl);

        static int slowLogs = 0;
        if (frameMs > 25.0 && slowLogs < 300) {
            ++slowLogs;
            log::info("JW: diag slow frame {:.1f} ms ({} physics steps ran in it, assist tracking active: {}, this mod used {:.0f} us since last frame)",
                frameMs, steps, state().anyActive, g_modUs + msBetween(tickStart, Clock::now()) * 1000.0);
        }
        g_modUs = 0.0;
    }
}

$on_mod(Loaded) {
    log::info("JW: build 14 loaded (measured tick rate, saved-tick logging, same-tick wave tap preservation, macOS)");
    jw::g_enabled      = Mod::get()->getSettingValue<bool>("assist-enabled");
    jw::g_practiceOnly = Mod::get()->getSettingValue<bool>("practice-only");
    jw::g_windowMs     = static_cast<int>(Mod::get()->getSettingValue<int64_t>("window-ms"));
    jw::g_waveDelay    = static_cast<int>(Mod::get()->getSettingValue<int64_t>("wave-delay"));
    jw::g_waveGuard    = Mod::get()->getSettingValue<bool>("wave-guard");
    log::info("JW: loaded settings enabled={} practiceOnly={} windowMs={} waveDelay={}",
        jw::g_enabled, jw::g_practiceOnly, jw::g_windowMs, jw::g_waveDelay);

    listenForSettingChanges<bool>("assist-enabled",   [](bool v)    { jw::g_enabled = v; });
    listenForSettingChanges<bool>("practice-only",    [](bool v)    { jw::g_practiceOnly = v; });
    listenForSettingChanges<int64_t>("window-ms",     [](int64_t v) {
        jw::g_windowMs = static_cast<int>(v);
        log::info("JW: window changed to {} ms (applies to the next click as a step budget)", jw::g_windowMs);
    });
    listenForSettingChanges<int64_t>("wave-delay",    [](int64_t v) { jw::g_waveDelay = static_cast<int>(v); });
    listenForSettingChanges<bool>("wave-guard",       [](bool v)    { jw::g_waveGuard = v; });
}

class $modify(JWScheduler, CCScheduler) {
    void update(float dt) {
        CCScheduler::update(dt);
        jw::tick();
    }
};

class $modify(JWGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& st = jw::state();

        // Diagnostic: how old is this physical click when the game receives it?
        static int diagLogs = 0;
        if (down && !st.injecting && button == jw::kButtonJump && diagLogs < 40) {
            ++diagLogs;
            log::info("JW: diag click age at game: mouse {:.1f} ms, key {:.1f} ms",
                jw::clickAgeMs(true), jw::clickAgeMs(false));
        }

        GJBaseGameLayer* self = this;

        if (st.injecting || button != jw::kButtonJump || !jw::assistActive()) {
            if (!st.injecting && button == jw::kButtonJump) {
                if (down) {
                    jw::logDecision(
                        jw::g_enabled ? "PASS: assist inactive (practice-only outside practice, or no level)"
                                      : "PASS: assist inactive (Enabled is off)",
                        isPlayer1 ? m_player1 : m_player2);
                }
                // Anything still queued for this side goes out first, in order.
                jw::flushSide(self, isPlayer1);
            }
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        static bool logged = false;
        if (!logged) { logged = true; log::info("JW: handleButton intercept running"); }

        PlayerObject* p = isPlayer1 ? m_player1 : m_player2;
        auto& w = st.sides[isPlayer1 ? 0 : 1];
        const int side = isPlayer1 ? 0 : 1;

        // Wave: presses and releases ride the physics-tick queue. With delay 0
        // they are delivered inside this very tick; otherwise on the tick
        // they fall due. The frame handler never touches them.
        if (p && p->m_isDart) {
            if (jw::g_waveGuard) {
                // Drop a repeated press with no release in between. Releases always pass.
                if (down && jw::g_waveDown[side]) {
                    if (jw::g_guardLogs < 20) {
                        ++jw::g_guardLogs;
                        log::info("JW: dropped duplicate wave press");
                    }
                    return;
                }
                jw::g_waveDown[side] = down;
            }
            if (w.active) w = jw::Side{};     // left cube-like mode with a cushion pending
            const bool spaced = jw::enqueue(down, isPlayer1, jw::g_stepSeq + jw::waveDelaySteps(), false, jw::kWaveMinGapSteps);
            ++jw::g_waveEvents;
            if (spaced) {
                ++jw::g_waveSpaced;
                if (jw::g_waveSpaced <= 60 || jw::g_waveSpaced % 25 == 0) {
                    log::info("JW: wave {} arrived in the same tick as the previous wave input -> pushed one tick later so the tap is not swallowed (saved {} of {} wave inputs)",
                        down ? "press" : "release", jw::g_waveSpaced, jw::g_waveEvents);
                }
            }
            if (jw::g_inTick) jw::pump(self);
            return;
        }
        jw::g_waveDown[side] = false; // not in wave: nothing to track

        if (down) {
            // Window of 0 or unsupported mode: vanilla behavior.
            if (jw::g_windowMs <= 0 || !jw::isCubeLike(p)) {
                jw::logDecision("PASS: window is 0 or mode not supported", p);
                jw::flushSide(self, isPlayer1);
                w = jw::Side{};
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
                return;
            }

            // The press goes in right now; runBuffer keeps streaming it every
            // physics step until the engine acknowledges it.
            jw::logDecision("CUSHIONED: press forwarded at once, streamed until the engine acknowledges", p);
            jw::logReplaced(w);
            jw::flushSide(self, isPlayer1);       // a stale queued release goes first
            jw::beginCushion(w, p);
            st.anyActive = true;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Release while a press is being streamed: hold it in the tick queue,
        // gated until the engine acknowledges (or the step budget ends).
        if (w.active && !w.releaseQueued) {
            w.releaseQueued = true;
            jw::enqueue(false, isPlayer1, jw::g_stepSeq, true);
            return;
        }

        // Any other release is queued too (due this tick), so ordering is
        // always decided in one place.
        jw::enqueue(false, isPlayer1, jw::g_stepSeq, false);
        if (jw::g_inTick) jw::pump(self);
    }

    // Runs once per physics step. Original first (it feeds queued clicks through
    // handleButton above), then supervise the stream and deliver due inputs.
    void processQueuedButtons(float dt, bool clearInputQueue) {
        GJBaseGameLayer* self = this;
        const bool inPlay = typeinfo_cast<PlayLayer*>(self) != nullptr;  // PlayLayer only, not the editor
        if (!inPlay) {
            GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
            return;
        }

        ++jw::g_stepSeq;
        jw::g_inTick = true;
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
        jw::g_inTick = false;

        if (!jw::g_stepHookSeen) {
            jw::g_stepHookSeen = true;
            log::info("JW: physics-step hook is running");
        }

        // Assist switched off (or level gone): never strand a queued input.
        if (!jw::assistActive()) {
            jw::flushAll(self);
            jw::resetSides();
            return;
        }

        const auto t0 = jw::Clock::now();
        jw::runBuffer(self);
        jw::pump(self);
        jw::g_modUs += jw::msBetween(t0, jw::Clock::now()) * 1000.0;
    }
};

class $modify(JWPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        auto* label = CCLabelBMFont::create("Assist ON", "bigFont.fnt");
        label->setID("jw-label");
        label->setScale(0.4f);
        label->setOpacity(160);
        label->setAnchorPoint({0.f, 1.f});
        auto size = CCDirector::get()->getWinSize();
        label->setPosition({8.f, size.height - 8.f});
        label->setVisible(false);
        this->addChild(label, 100);
    }

    void resetLevel() {
        jw::flushAll(static_cast<GJBaseGameLayer*>(this)); // never leave our own held press behind
        jw::reset();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        jw::reset();
        PlayLayer::onQuit();
    }
};
