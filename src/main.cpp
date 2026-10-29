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
#include <limits>
#include <vector>

using namespace geode::prelude;

// ===========================================================================
// PURE WAVE PREDICTION MATH (no Geode types; unit-tested separately)
// ===========================================================================
// BEGIN PURE
namespace jwmath {
    struct Box { double minX, maxX, minY, maxY; bool solid; };

    // The wave as seen by the predictor. vx = horizontal distance per physics
    // step, vy = vertical distance per step while the wave is angled (>= 0).
    struct WaveState { double x, y, vx, vy, half; };

    struct World {
        std::vector<Box> boxes;   // sorted by minX
        double maxW = 0.0;        // widest box, so a minX window query cannot miss a long one

        void clear() { boxes.clear(); maxW = 0.0; }

        void finalize() {
            std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.minX < b.minX; });
            maxW = 0.0;
            for (auto& b : boxes) maxW = std::max(maxW, b.maxX - b.minX);
        }

        void gather(double xlo, double xhi, double ylo, double yhi, std::vector<Box>& out) const {
            out.clear();
            const double from = xlo - maxW;
            auto it = std::lower_bound(boxes.begin(), boxes.end(), from,
                [](const Box& b, double v) { return b.minX < v; });
            for (; it != boxes.end() && it->minX <= xhi; ++it) {
                if (it->maxX < xlo) continue;
                if (it->maxY < ylo || it->minY > yhi) continue;
                out.push_back(*it);
            }
        }
    };

    struct Hit { int kind = 0; double t = 0.0; }; // kind: 0 none, 1 entered through a side (x), 2 through a top/bottom (y)

    // Segment (x0,y0)->(x1,y1) of the wave's CENTER against a box grown by the
    // wave's half-size. Objects the wave already overlaps (sliding along them)
    // are ignored: they cannot be judged from a point sweep.
    inline Hit sweep(double x0, double y0, double x1, double y1, const Box& b, double half, double eps) {
        const double minX = b.minX - half + eps, maxX = b.maxX + half - eps;
        const double minY = b.minY - half + eps, maxY = b.maxY + half - eps;
        if (minX >= maxX || minY >= maxY) return {};
        if (x0 > minX && x0 < maxX && y0 > minY && y0 < maxY) return {};

        const double dx = x1 - x0, dy = y1 - y0;
        const double inf = std::numeric_limits<double>::infinity();
        auto slab = [&](double p0, double d, double lo, double hi, double& tn, double& tf) {
            if (std::fabs(d) < 1e-9) {
                if (p0 <= lo || p0 >= hi) return false;
                tn = -inf; tf = inf; return true;
            }
            double t1 = (lo - p0) / d, t2 = (hi - p0) / d;
            if (t1 > t2) std::swap(t1, t2);
            tn = t1; tf = t2; return true;
        };
        double txn, txf, tyn, tyf;
        if (!slab(x0, dx, minX, maxX, txn, txf)) return {};
        if (!slab(y0, dy, minY, maxY, tyn, tyf)) return {};
        const double tn = std::max(txn, tyn), tf = std::min(txf, tyf);
        if (tn > tf || tf < 0.0 || tn > 1.0 || tn < 0.0) return {};
        return Hit{txn > tyn ? 1 : 2, tn};
    }

    // Does flying `horizon` steps kill the wave? First `switchAfter` steps use
    // dir0, the rest dir1 (dir: +1 up, -1 down). Rules, matching the game:
    //   * hazards (spikes, saws): any contact is fatal
    //   * solid blocks: entering through a SIDE is fatal; touching a top or
    //     bottom surface makes the wave slide, so the path is treated as safe
    //     from there on.
    inline bool pathFatal(const std::vector<Box>& cands, WaveState s, int dir0, int dir1,
                          int switchAfter, int horizon, double eps = 0.25) {
        double x = s.x, y = s.y;
        for (int i = 0; i < horizon; ++i) {
            const int dir = i < switchAfter ? dir0 : dir1;
            const double nx = x + s.vx, ny = y + dir * s.vy;
            Hit best; const Box* bestBox = nullptr;
            for (const auto& b : cands) {
                Hit h = sweep(x, y, nx, ny, b, s.half, eps);
                if (h.kind != 0 && (bestBox == nullptr || h.t < best.t)) { best = h; bestBox = &b; }
            }
            if (bestBox) {
                if (!bestBox->solid) return true;   // hazard
                if (best.kind == 1)  return true;   // block side
                return false;                       // slide: stop judging
            }
            x = nx; y = ny;
        }
        return false;
    }

    // Should a direction change be held back this tick?
    // Yes only if: turning now is fatal, but continuing for k ticks and then
    // turning (some k <= maxHold) survives. Otherwise waiting cannot help.
    inline bool shouldHoldFlip(const World& w, const WaveState& s, int curDir, int newDir,
                               int maxHold, int horizon, std::vector<Box>& scratch) {
        if (curDir == newDir) return false;
        const double reachX = (horizon + maxHold) * s.vx + s.half;
        const double reachY = (horizon + maxHold) * s.vy + s.half;
        w.gather(s.x - s.half, s.x + reachX, s.y - reachY, s.y + reachY, scratch);
        if (scratch.empty()) return false;
        if (!pathFatal(scratch, s, newDir, newDir, 0, horizon)) return false;
        for (int k = 1; k <= maxHold; ++k) {
            if (!pathFatal(scratch, s, curDir, newDir, k, horizon)) return true;
        }
        return false;
    }
}
// END PURE

namespace jw {
    // Diagnostics only. No gameplay decision reads the wall clock.
    using Clock = std::chrono::steady_clock;

    constexpr int    kButtonJump = 1;
    constexpr double kJumpVel    = 5.0;      // old absolute test, kept for log comparison only
    constexpr double kRegisterDeltaV = 2.0;  // per-step velocity change that counts as an orb effect (with orb contact)

    // Sub-tick hold cushion: a press stays down at least this many physics steps.
    constexpr int    kMinHoldSteps = 3;

    // Ceiling for the "stay on the orb" extension: 0.5 s of game time.
    constexpr int    kHardCapSteps = 120;

    // Physics runs at 240 steps per second; used once per click to turn the
    // window-ms setting into a STEP budget.
    constexpr double kStepMs = 1000.0 / 240.0;

    // ---- Wave predictor tuning -------------------------------------------
    constexpr bool   kWavePredict       = true;  // master switch for the look-ahead
    constexpr int    kWaveMaxHoldSteps  = 6;     // a flip is never held longer than this (6 steps = 25 ms)
    constexpr int    kWaveHorizon       = 24;    // steps simulated ahead (~100 ms: assumes you hold the new direction that long)
    constexpr double kWaveHalf          = 5.0;   // half the wave hitbox, in level units (scaled by the player's scale)
    constexpr double kHazardShrink      = 0.6;   // hazard bounding boxes are bigger than their real hitbox
    constexpr int    kTypeSolid         = 0;     // GameObjectType values
    constexpr int    kTypeHazard        = 2;

    struct Side {
        bool     active         = false; // a click is being handled (waiting for contact, or press in flight)
        bool     pressed        = false; // the press has been delivered to the engine
        bool     early          = false; // arrived with nothing to land on: buffered until contact
        bool     releaseQueued  = false; // player let go; the release sits in the tick queue
        bool     releaseOk      = false; // gate: the queued release may now be delivered
        bool     registered     = false; // the engine acted on the press
        bool     pressGround    = false; // diagnostics
        int      pressRings     = 0;     // diagnostics
        bool     prevGround     = false;
        bool     prevUpsideDown = false;
        int      prevRings      = 0;
        double   prevUp         = 0.0;
        uint64_t clickSeq       = 0;     // step counter when the click arrived
        uint64_t pressSeq       = 0;     // step counter when the press went in
        int      waitSteps      = 0;     // steps spent waiting for contact
        int      heldSteps      = 0;     // steps the press has been live
        int      maxSteps       = 0;     // step budget (game time, stutter-proof)
        int      streamed       = 0;
        int      extSteps       = 0;
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
        uint64_t bornStep;
        uint64_t seq;
        bool     down;
        bool     p1;
        bool     cushion;   // release gated by the side's releaseOk (cube)
        bool     waveGate;  // direction change gated by the wave predictor
    };

    struct WaveTrack {
        bool   seen  = false;
        bool   valid = false;   // vx measured
        double lastX = 0.0, lastY = 0.0;
        double vx    = 0.0;
        double slope = 1.0;     // |dy| / dx while angled
        int    dir   = -1;      // +1 up, -1 down (measured; events as fallback)
    };

    inline State& state() {
        static State s;
        return s;
    }

    inline bool g_enabled      = true;
    inline bool g_practiceOnly = false;
    inline int  g_windowMs     = 80;
    inline int  g_waveDelay    = 0;
    inline bool g_waveGuard    = false;
    inline bool g_stepHookSeen = false;
    inline bool g_inTick       = false;
    inline bool g_waveDown[2]  = {false, false};
    inline int  g_guardLogs    = 0;
    inline double g_stepsPerFrame = 4.0;
    inline double g_modUs      = 0.0;

    inline std::vector<Event> g_queue;
    inline uint64_t g_seq      = 0;
    inline uint64_t g_stepSeq  = 0;
    inline uint64_t g_lastDue[2] = {0, 0};

    inline WaveTrack g_wave[2];
    inline jwmath::World g_world;
    inline bool g_worldReady = false;
    inline std::vector<jwmath::Box> g_scratch;

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
        g_waveDown[0] = g_waveDown[1] = false;
        g_wave[0] = WaveTrack{};
        g_wave[1] = WaveTrack{};
    }

    inline bool assistActive() {
        auto* pl = PlayLayer::get();
        if (!g_enabled || !pl) return false;
        if (g_practiceOnly && !pl->m_isPracticeMode) return false;
        return true;
    }

    // One-off-action modes: cube, ball, robot, spider.
    inline bool isCubeLike(PlayerObject* p) {
        return p && !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
    }

    inline int ringCount(PlayerObject* p) {
        return (p && p->m_touchingRings) ? static_cast<int>(p->m_touchingRings->count()) : 0;
    }

    // First tick of ground or orb contact.
    inline bool hasContact(PlayerObject* p) {
        return p && (p->m_isOnGround || ringCount(p) > 0);
    }

    inline double upVel(PlayerObject* p) {
        const double vel = static_cast<double>(p->m_yVelocity);
        return p->m_isUpsideDown ? -vel : vel;
    }

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
            log::info("JW: previous click replaced by a new click (pressed={}, release queued={})", w.pressed, w.releaseQueued);
        }
    }

    inline int g_cntTotal = 0, g_cntCushioned = 0, g_cntUnsupported = 0, g_cntInactive = 0;
    inline void logDecision(const char* why, PlayerObject* p) {
        const bool cushioned = std::strncmp(why, "CUSHIONED", 9) == 0 || std::strncmp(why, "HELD", 4) == 0;
        if (cushioned)                              ++g_cntCushioned;
        else if (std::strstr(why, "not supported")) ++g_cntUnsupported;
        else                                        ++g_cntInactive;
        ++g_cntTotal;
        if (g_cntTotal % 25 == 0) {
            log::info("JW: summary after {} clicks: buffered-or-cushioned={} unsupported-mode={} inactive={}",
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
    // Level geometry for the wave predictor (built ONCE at level load)
    // ---------------------------------------------------------------------

    // Reads one object. The `requires` check makes this degrade gracefully:
    // if the bindings in use lack these members, it returns false (predictor
    // stays off) instead of breaking the build.
    template <class Obj>
    inline bool readBox(Obj* o, jwmath::Box& b) {
        if constexpr (requires { o->m_objectType; o->getObjectRect(); }) {
            if (!o) return false;
            const int t = static_cast<int>(o->m_objectType);
            if (t != kTypeSolid && t != kTypeHazard) return false;
            const auto r = o->getObjectRect();
            b.minX = r.origin.x;               b.maxX = r.origin.x + r.size.width;
            b.minY = r.origin.y;               b.maxY = r.origin.y + r.size.height;
            b.solid = (t == kTypeSolid);
            if (!b.solid) { // real spike hitboxes are smaller than their bounding box
                const double cx = (b.minX + b.maxX) * 0.5, cy = (b.minY + b.maxY) * 0.5;
                const double hw = (b.maxX - b.minX) * 0.5 * kHazardShrink;
                const double hh = (b.maxY - b.minY) * 0.5 * kHazardShrink;
                b.minX = cx - hw; b.maxX = cx + hw; b.minY = cy - hh; b.maxY = cy + hh;
            }
            return b.maxX > b.minX && b.maxY > b.minY;
        } else {
            return false;
        }
    }

    template <class Layer>
    inline void buildWorldFrom(Layer* layer) {
        g_world.clear();
        g_worldReady = false;
        if constexpr (requires { layer->m_objects; }) {
            auto* arr = layer->m_objects;
            if (!arr) { log::info("JW: wave predictor off (no object array)"); return; }
            const auto t0 = Clock::now();
            const unsigned n = static_cast<unsigned>(arr->count());
            g_world.boxes.reserve(n / 2);
            for (unsigned i = 0; i < n; ++i) {
                jwmath::Box b{};
                if (readBox(static_cast<GameObject*>(arr->objectAtIndex(i)), b)) g_world.boxes.push_back(b);
            }
            g_world.finalize();
            g_worldReady = !g_world.boxes.empty();
            log::info("JW: wave predictor world built: {} blocks/hazards from {} objects in {:.1f} ms (ready: {})",
                g_world.boxes.size(), n, msBetween(t0, Clock::now()), g_worldReady);
        } else {
            log::info("JW: wave predictor off (bindings have no m_objects)");
        }
    }

    // Measure the wave once per physics tick: speed, slope, direction.
    inline void trackWave(GJBaseGameLayer* layer) {
        for (int i = 0; i < 2; ++i) {
            PlayerObject* p = (i == 0) ? layer->m_player1 : layer->m_player2;
            auto& t = g_wave[i];
            if (!p || !p->m_isDart) { t = WaveTrack{}; continue; }
            const double x = static_cast<double>(p->getPositionX());
            const double y = static_cast<double>(p->getPositionY());
            if (t.seen) {
                const double dx = x - t.lastX, dy = y - t.lastY;
                if (dx > 0.05 && dx < 50.0) {
                    t.vx = dx;
                    t.valid = true;
                    if (std::fabs(dy) > 0.05) {
                        t.slope = std::clamp(0.8 * t.slope + 0.2 * (std::fabs(dy) / dx), 0.25, 4.0);
                        t.dir = dy > 0.0 ? 1 : -1;
                    }
                }
            }
            t.seen = true;
            t.lastX = x;
            t.lastY = y;
        }
    }

    inline int waveDirFor(bool down, bool upsideDown) {
        return (down ? 1 : -1) * (upsideDown ? -1 : 1);
    }

    // May this direction change go out on this tick?
    inline bool waveMayDeliver(GJBaseGameLayer* layer, const struct Event& ev);

    // ---------------------------------------------------------------------
    // The physics-tick input queue
    // ---------------------------------------------------------------------

    inline void inject(GJBaseGameLayer* layer, bool down, bool p1) {
        auto& st = state();
        const bool was = st.injecting;
        st.injecting = true;
        layer->handleButton(down, kButtonJump, p1);
        st.injecting = was;
    }

    inline void enqueue(bool down, bool p1, uint64_t dueStep, bool cushion, bool waveGate = false) {
        const int i = sideIdx(p1);
        if (dueStep < g_lastDue[i]) dueStep = g_lastDue[i];
        g_lastDue[i] = dueStep;
        g_queue.push_back(Event{dueStep, g_stepSeq, ++g_seq, down, p1, cushion, waveGate});
    }

    inline uint64_t waveDelaySteps() {
        if (g_waveDelay <= 0) return 0;
        const long steps = std::lround(static_cast<double>(g_waveDelay) * g_stepsPerFrame);
        return static_cast<uint64_t>(std::max<long>(steps, g_waveDelay));
    }

    // ---------------------------------------------------------------------
    // Outcome statistics (the per-click logs are capped; these are not)
    // ---------------------------------------------------------------------
    inline int g_outGroundAck = 0, g_outGroundMiss = 0;   // contact existed at click time
    inline int g_outEarlyAck = 0,  g_outEarlyMiss = 0;    // buffered until contact
    inline long g_earlyWaitSum = 0;
    inline int  g_earlyWaitMax = 0;

    inline void recordOutcome(const Side& w) {
        if (w.early) {
            if (w.registered) { ++g_outEarlyAck; g_earlyWaitSum += w.waitSteps; g_earlyWaitMax = std::max(g_earlyWaitMax, w.waitSteps); }
            else              { ++g_outEarlyMiss; }
        } else {
            if (w.registered) ++g_outGroundAck; else ++g_outGroundMiss;
        }
        const int total = g_outGroundAck + g_outGroundMiss + g_outEarlyAck + g_outEarlyMiss;
        if (total % 10 == 0) {
            log::info("JW: outcomes after {}: contact-at-click ack={} miss={} | buffered ack={} miss={} (waited avg {:.1f} / max {} steps, budget {} steps)",
                total, g_outGroundAck, g_outGroundMiss, g_outEarlyAck, g_outEarlyMiss,
                g_outEarlyAck > 0 ? static_cast<double>(g_earlyWaitSum) / g_outEarlyAck : 0.0,
                g_earlyWaitMax, w.maxSteps);
        }
    }

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
                    log::info("JW: cushion release via {} (waited {} steps for contact, held {}/{} steps, streamed {} extra presses, {} steps past budget on an orb, registered: {}, diag wall {:.1f} ms, pressed on ground={} rings={}, now ground={} rings={}, old test: {})",
                        w.registered ? "engine ack" : "step budget end", w.waitSteps, w.heldSteps, w.maxSteps,
                        w.streamed, w.extSteps, w.registered, msBetween(w.clickTime, Clock::now()),
                        w.pressGround, w.pressRings,
                        p ? p->m_isOnGround : false, ringCount(p),
                        p ? jumpFired(p, w.pressGround) : false);
                }
            }
            st.sides[i] = Side{};
            st.anyActive = st.sides[0].active || st.sides[1].active;
        }
        if (ev.waveGate) {
            PlayerObject* p = ev.p1 ? layer->m_player1 : layer->m_player2;
            g_wave[i].dir = waveDirFor(ev.down, p ? p->m_isUpsideDown : false);
            const uint64_t held = g_stepSeq - ev.bornStep;
            static int heldLogs = 0;
            if (held > 0 && heldLogs < 300) {
                ++heldLogs;
                log::info("JW: wave {} delivered after being held {} ticks for a safe turn",
                    ev.down ? "press" : "release", held);
            }
        }
        inject(layer, ev.down, ev.p1);
    }

    inline bool waveMayDeliver(GJBaseGameLayer* layer, const Event& ev) {
        if (!kWavePredict || !g_worldReady) return true;
        if (g_stepSeq - ev.bornStep >= static_cast<uint64_t>(kWaveMaxHoldSteps)) return true; // never eat an input

        const int i = sideIdx(ev.p1);
        PlayerObject* p = ev.p1 ? layer->m_player1 : layer->m_player2;
        const auto& t = g_wave[i];
        if (!p || !p->m_isDart || !t.valid) return true;
        if (p->m_isUpsideDown) return true; // direction mapping unverified when flipped: stay out of the way

        const double scale = std::clamp(static_cast<double>(p->getScale()), 0.3, 1.5);
        jwmath::WaveState s;
        s.x = t.lastX; s.y = t.lastY;
        s.vx = t.vx;   s.vy = t.vx * t.slope;
        s.half = kWaveHalf * scale;

        const int curDir = t.dir;
        const int newDir = waveDirFor(ev.down, p->m_isUpsideDown);
        const bool hold = jwmath::shouldHoldFlip(g_world, s, curDir, newDir,
                                                 kWaveMaxHoldSteps, kWaveHorizon, g_scratch);
        return !hold;
    }

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
                if (w.active && !w.releaseOk) ready = false;     // engine has not acknowledged yet
            }
            if (ready && !force && ev.waveGate) {
                if (!waveMayDeliver(layer, ev)) ready = false;   // unsafe to turn on this tick
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

    inline void snapshotForPress(Side& w, PlayerObject* p) {
        w.pressSeq       = g_stepSeq;
        w.pressGround    = p->m_isOnGround;
        w.pressRings     = ringCount(p);
        w.prevGround     = p->m_isOnGround;
        w.prevUpsideDown = p->m_isUpsideDown;
        w.prevRings      = ringCount(p);
        w.prevUp         = upVel(p);
    }

    // ---------------------------------------------------------------------
    // Per-step supervision (cube-like modes)
    // ---------------------------------------------------------------------
    //
    // A click with nothing to land on is HELD BACK (the engine never sees it).
    // On the first tick of ground or orb contact the press is injected, then
    // re-sent every step until the engine acknowledges it (left the ground,
    // gravity flip, or an orb-contact velocity change). A jump fires INSIDE
    // handleButton, so the comparison snapshot is taken before the press and
    // only rolled forward after the check has run.
    //
    // Budget: step counts only (stutter-proof). If the player overlaps an
    // orb and the press is still unacknowledged, the budget is ignored until
    // the engine acknowledges or the overlap ends (kHardCapSteps as a rail).
    inline void runBuffer(GJBaseGameLayer* layer) {
        auto& st = state();
        if (!st.anyActive) return;

        for (int i = 0; i < 2; ++i) {
            auto& w = st.sides[i];
            if (!w.active) continue;

            const bool p1 = (i == 0);
            PlayerObject* p = p1 ? layer->m_player1 : layer->m_player2;

            // Player gone or no longer in a supported mode: stop; a queued release goes out this step.
            if (!p || !isCubeLike(p)) {
                w = Side{};
                continue;
            }

            // ---- waiting for the first tick of contact -------------------
            if (!w.pressed) {
                w.waitSteps = static_cast<int>(g_stepSeq - w.clickSeq);
                if (hasContact(p)) {
                    snapshotForPress(w, p);          // pre-press state
                    w.pressed = true;
                    inject(layer, true, p1);         // the buffered 'down' goes in on the contact tick
                    static int contactLogs = 0;
                    if (contactLogs < 200) {
                        ++contactLogs;
                        log::info("JW: buffered click injected on contact tick after {} steps (ground={} rings={})",
                            w.waitSteps, w.pressGround, w.pressRings);
                    }
                } else if (w.waitSteps >= w.maxSteps) {
                    // No contact within the budget: this click never had a landing.
                    recordOutcome(w);
                    if (!w.releaseQueued) {
                        inject(layer, true, p1);     // player is still holding: hand the hold back
                        log::info("JW: buffered click expired after {} steps without contact; hold handed back", w.waitSteps);
                    } else {
                        log::info("JW: buffered tap expired after {} steps without contact; dropped", w.waitSteps);
                    }
                    w = Side{};
                    continue;
                } else {
                    continue;
                }
            }

            // ---- press is live: stream until acknowledged ----------------
            w.heldSteps = static_cast<int>(g_stepSeq - w.pressSeq);

            auto detect = [&]() {
                if (w.registered) return;
                const bool   onGround   = p->m_isOnGround;
                const bool   left       = w.prevGround && !onGround;
                const bool   flipped    = p->m_isUpsideDown != w.prevUpsideDown;
                // A velocity change alone is NOT proof (pads, portals, slopes also
                // change it); it only counts together with real orb contact.
                const bool   orbContact = ringCount(p) > 0 || w.prevRings > 0;
                const bool   kick       = !onGround && orbContact
                                          && std::fabs(upVel(p) - w.prevUp) > kRegisterDeltaV;
                if (left || flipped || kick) w.registered = true;
            };

            detect();

            const bool overBudget = w.heldSteps >= w.maxSteps;
            const bool nearOrb    = ringCount(p) > 0;
            const bool extend     = !w.registered && overBudget && nearOrb && w.heldSteps < kHardCapSteps;

            if (!w.registered && w.heldSteps >= 1 && (!overBudget || extend)) {
                inject(layer, true, p1);
                ++w.streamed;
                if (overBudget) ++w.extSteps;
                detect();
            }

            w.prevGround     = p->m_isOnGround;
            w.prevUpsideDown = p->m_isUpsideDown;
            w.prevRings      = ringCount(p);
            w.prevUp         = upVel(p);

            w.releaseOk = (w.registered && w.heldSteps >= kMinHoldSteps)
                       || (overBudget && !extend);

            if (!w.releaseQueued && w.releaseOk) {
                recordOutcome(w);
                static int doneLogs = 0;
                if (doneLogs < 400) {
                    ++doneLogs;
                    log::info("JW: press tracking ended while held (registered: {}, waited {} steps, held {}/{} steps, streamed {}, extended {})",
                        w.registered, w.waitSteps, w.heldSteps, w.maxSteps, w.streamed, w.extSteps);
                }
                w = Side{};
            }
        }

        st.anyActive = st.sides[0].active || st.sides[1].active;
    }

    // Runs once per rendered frame. Diagnostics and UI only.
    inline void tick() {
        const auto tickStart = Clock::now();
        static auto lastFrame = Clock::now();
        static uint64_t lastSeq = g_stepSeq;
        const auto nowFrame = Clock::now();
        const double frameMs = msBetween(lastFrame, nowFrame);
        lastFrame = nowFrame;

        const uint64_t steps = g_stepSeq - lastSeq;
        lastSeq = g_stepSeq;
        if (steps > 0 && steps <= 64) {
            g_stepsPerFrame = std::clamp(g_stepsPerFrame * 0.9 + static_cast<double>(steps) * 0.1, 1.0, 16.0);
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
    log::info("JW: build 14 loaded (contact-tick cube buffer, predictive wave turn buffer, tick-locked queue, macOS)");
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

        // ---- Wave: every direction change rides the tick queue and is gated
        // by the predictor (delivered at once when safe, held up to
        // kWaveMaxHoldSteps ticks when turning right now would crash).
        if (p && p->m_isDart) {
            if (jw::g_waveGuard) {
                if (down && jw::g_waveDown[side]) {
                    if (jw::g_guardLogs < 20) {
                        ++jw::g_guardLogs;
                        log::info("JW: dropped duplicate wave press");
                    }
                    return;
                }
                jw::g_waveDown[side] = down;
            }
            if (w.active) w = jw::Side{};

            static int waveLogs = 0;
            if (waveLogs < 300) {
                ++waveLogs;
                log::info("JW: wave {} at step {} x={:.1f} y={:.1f} yVel={:.2f} (earliest delivery step {})",
                    down ? "press" : "release", jw::g_stepSeq,
                    static_cast<double>(p->getPositionX()), static_cast<double>(p->getPositionY()),
                    static_cast<double>(p->m_yVelocity), jw::g_stepSeq + jw::waveDelaySteps());
            }

            jw::enqueue(down, isPlayer1, jw::g_stepSeq + jw::waveDelaySteps(), false, true);
            if (jw::g_inTick) jw::pump(self);
            return;
        }
        jw::g_waveDown[side] = false;

        if (down) {
            if (jw::g_windowMs <= 0 || !jw::isCubeLike(p)) {
                jw::logDecision("PASS: window is 0 or mode not supported", p);
                jw::flushSide(self, isPlayer1);
                w = jw::Side{};
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
                return;
            }

            jw::logReplaced(w);
            jw::flushSide(self, isPlayer1);       // a stale queued release goes first

            w = jw::Side{};
            w.active    = true;
            w.clickSeq  = jw::g_stepSeq;
            w.clickTime = jw::Clock::now();
            w.maxSteps  = std::max(jw::kMinHoldSteps,
                              static_cast<int>(std::lround(static_cast<double>(jw::g_windowMs) / jw::kStepMs)));
            st.anyActive = true;

            if (jw::hasContact(p)) {
                // Ground or orb contact right now: nothing to wait for.
                jw::snapshotForPress(w, p);
                w.pressed = true;
                jw::logDecision("CUSHIONED: contact now, press forwarded and streamed until acknowledged", p);
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
                return;
            }

            // Too early: hold the click back; runBuffer injects it on the first contact tick.
            w.early = true;
            jw::logDecision("HELD: nothing to land on yet, click buffered until the first contact tick", p);
            return;
        }

        // Release while a click is active (waiting or streaming): keep it in the queue, gated.
        if (w.active && !w.releaseQueued) {
            w.releaseQueued = true;
            jw::enqueue(false, isPlayer1, jw::g_stepSeq, true);
            return;
        }

        jw::enqueue(false, isPlayer1, jw::g_stepSeq, false);
        if (jw::g_inTick) jw::pump(self);
    }

    // Runs once per physics step. Original first (it feeds queued clicks through
    // handleButton above), then track the wave, supervise buffered clicks and
    // deliver due inputs.
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

        if (!jw::assistActive()) {
            jw::flushAll(self);
            jw::resetSides();
            return;
        }

        const auto t0 = jw::Clock::now();
        jw::trackWave(self);
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

        // One-time geometry cache for the wave predictor (done here so any
        // cost lands in the loading screen, never in gameplay).
        jw::buildWorldFrom(static_cast<GJBaseGameLayer*>(this));
    }

    void resetLevel() {
        jw::flushAll(static_cast<GJBaseGameLayer*>(this)); // never leave our own held press behind
        jw::reset();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        jw::reset();
        jw::g_world.clear();
        jw::g_worldReady = false;
        PlayLayer::onQuit();
    }
};
