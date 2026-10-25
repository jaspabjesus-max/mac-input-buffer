#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include <CoreGraphics/CoreGraphics.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

using namespace geode::prelude;

namespace jw {
    using Clock = std::chrono::steady_clock;

    constexpr int    kButtonJump = 1;
    constexpr double kJumpVel    = 5.0; // old absolute test, kept for log comparison only
    constexpr double kRegisterDeltaV = 2.0; // velocity change that proves a press took (one step of gravity is ~0.25)

    // One held early click per player side.
    struct Side {
        bool              active          = false; // a swallowed click is waiting
        bool              releaseDeferred = false; // player let go while we waited
        bool              pressed         = false; // our single press is in flight
        bool              wasGround       = false;
        bool              clickedInAir    = false;
        double            waitedMs        = 0.0;   // wait before our press
        const char*       driver          = "";    // "step" or "tick"
        double            velAtPress      = 0.0;   // vertical velocity when we pressed
        CCObject*         ringAtPress     = nullptr; // the orb we pressed on (identity only, never dereferenced)
        int               heldSteps       = 0;     // sub-steps the press was kept held waiting for confirmation
        Clock::time_point clickTime{};
    };

    struct State {
        bool injecting = false;
        bool anyActive = false;
        Side sides[2];
    };

    // Uniform delay line for wave inputs (optional, off by default).
    struct Delayed { bool down; bool p1; int framesLeft; };

    inline State& state() {
        static State s;
        return s;
    }

    inline bool g_enabled      = true;
    inline bool g_practiceOnly = false;
    inline int  g_windowMs     = 80;   // how long an early click may wait, in real milliseconds
    inline int  g_waveDelay    = 0;
    inline bool g_waveGuard    = false;
    inline bool g_stepHookSeen = false;
    inline std::vector<Delayed> g_delayQ;
    inline bool g_waveDown[2]  = {false, false};
    inline int  g_guardLogs    = 0;

    inline double msBetween(Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
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
        g_delayQ.clear();
        g_waveDown[0] = g_waveDown[1] = false;
    }

    inline bool assistActive() {
        auto* pl = PlayLayer::get();
        if (!g_enabled || !pl) return false;
        if (g_practiceOnly && !pl->m_isPracticeMode) return false;
        return true;
    }

    // One-off-action modes: cube, ball, robot, spider.
    // Ship, UFO, wave and swing pass through untouched.
    inline bool isCubeLike(PlayerObject* p) {
        return p && !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
    }

    inline int ringCount(PlayerObject* p) {
        return (p && p->m_touchingRings) ? static_cast<int>(p->m_touchingRings->count()) : 0;
    }

    inline bool touchingOrb(PlayerObject* p) {
        return ringCount(p) > 0;
    }

    // Used for logging only: does the player's state look like a jump just fired?
    inline bool jumpFired(PlayerObject* p, bool wasGround) {
        const double vel = static_cast<double>(p->m_yVelocity);
        const double up  = p->m_isUpsideDown ? -vel : vel;
        return (wasGround && !p->m_isOnGround) || up > kJumpVel;
    }

    // Diagnostic: how old is the physical key press when the game receives it?
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
            log::info("JW: previous buffer replaced by a new click (pressed={})", w.pressed);
        }
    }

    // Diagnostic: which path did a real click take, and why?
    inline int g_cntTotal = 0, g_cntReady = 0, g_cntSwallowed = 0, g_cntUnsupported = 0, g_cntInactive = 0;
    inline void logDecision(const char* why, PlayerObject* p) {
        const bool swallowed = std::strncmp(why, "SWALLOWED", 9) == 0;
        if (swallowed)                              ++g_cntSwallowed;
        else if (std::strstr(why, "on ground"))     ++g_cntReady;
        else if (std::strstr(why, "not supported")) ++g_cntUnsupported;
        else                                        ++g_cntInactive;
        ++g_cntTotal;
        if (g_cntTotal % 25 == 0) {
            log::info("JW: summary after {} clicks: passed-through(ground/orb)={} swallowed={} unsupported-mode={} inactive={}",
                g_cntTotal, g_cntReady, g_cntSwallowed, g_cntUnsupported, g_cntInactive);
        }

        static int detail = 0;
        if (swallowed) { if (g_cntSwallowed > 300) return; }
        else           { if (detail >= 40) return; ++detail; }

        log::info("JW: click -> {} | windowMs={} ground={} rings={} ship={} ufo={} ball={} wave={} robot={} spider={} swing={}",
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

    // The core of the assist. Called from the physics-step hook (every step,
    // fine resolution) and from the scheduler tick (once per frame, fallback).
    //
    // ONE press per click, never a retry: a retry would press the NEXT orb the
    // player reaches, an input they never made.
    inline void runBuffer(GJBaseGameLayer* layer, const char* driver) {
        auto& st = state();
        if (!st.anyActive) return;
        if (!assistActive()) { resetSides(); return; }

        const auto now = Clock::now();

        for (int i = 0; i < 2; ++i) {
            auto& w = st.sides[i];
            if (!w.active) continue;

            const bool p1 = (i == 0);
            PlayerObject* p = p1 ? layer->m_player1 : layer->m_player2;

            auto send = [&](bool down) {
                st.injecting = true;
                layer->handleButton(down, kButtonJump, p1);
                st.injecting = false;
            };

            // Player gone or no longer in a supported mode: drop the buffer,
            // and never leave our own injected press held down.
            if (!p || !isCubeLike(p)) {
                if (w.pressed && p) send(false);
                w = Side{};
                continue;
            }

            const double waited = msBetween(w.clickTime, now);

            // 1. We already pressed once for this click: validate against the engine.
            if (w.pressed) {
                // Did the press take? Compare with the state at press time:
                // leaving the ground, or a velocity change far larger than one
                // step of gravity. This covers every orb type, including gravity
                // orbs, which the old absolute-velocity test mislabeled.
                const double dv = std::fabs(static_cast<double>(p->m_yVelocity) - w.velAtPress);
                const bool registered = (w.wasGround && !p->m_isOnGround) || dv > kRegisterDeltaV;

                // Same contact: still touching the SAME orb, or still on the ground.
                // Once the contact ends we stop, so a hold can never carry into a
                // different orb or a later landing.
                const bool sameContact = w.ringAtPress
                    ? (p->m_touchingRings && p->m_touchingRings->containsObject(w.ringAtPress))
                    : p->m_isOnGround;

                if (!registered && sameContact && waited < static_cast<double>(g_windowMs)) {
                    // Not confirmed yet, contact and time remain: keep the press
                    // held and re-check next sub-step. No release, no re-press.
                    ++w.heldSteps;
                    continue;
                }

                static int doneLogs = 0;
                if (doneLogs < 300) {
                    ++doneLogs;
                    log::info("JW: press done via {} (waited {:.1f} ms, registered: {}, held {} extra steps, dv={:.2f}, same contact: {}, old test: {}, ground={}, rings={})",
                        w.driver, w.waitedMs, registered, w.heldSteps, dv, sameContact,
                        jumpFired(p, w.wasGround), p->m_isOnGround, ringCount(p));
                }
                if (w.releaseDeferred) send(false);   // player already let go
                w = Side{};
                continue;
            }

            // 2. Time window used up: stop waiting.
            if (waited >= static_cast<double>(g_windowMs)) {
                static int expiredLogs = 0;
                if (expiredLogs < 300) {
                    ++expiredLogs;
                    log::info("JW: buffer expired after {:.1f} ms, no jump (clicked in air: {})",
                        waited, w.clickedInAir);
                }
                if (!w.releaseDeferred) {
                    // Still physically holding: hand the held press back, as vanilla would have.
                    send(true);
                    static int restoreLogs = 0;
                    if (restoreLogs < 20) {
                        ++restoreLogs;
                        log::info("JW: buffer expired while held, restored hold");
                    }
                }
                w = Side{};
                continue;
            }

            // 3. Hold until contact: press once, the moment a jump is legal.
            if (p->m_isOnGround || touchingOrb(p)) {
                static int pressLogs = 0;
                if (pressLogs < 300) {
                    ++pressLogs;
                    log::info("JW: press via {} after {:.1f} ms (ground={} rings={})",
                        driver, waited, p->m_isOnGround, ringCount(p));
                }
                w.wasGround   = p->m_isOnGround;
                w.velAtPress  = static_cast<double>(p->m_yVelocity);
                w.ringAtPress = (ringCount(p) > 0) ? p->m_touchingRings->objectAtIndex(0) : nullptr;
                w.heldSteps   = 0;
                w.waitedMs    = waited;
                w.driver      = driver;
                send(true);
                w.pressed = true;
            }
        }

        st.anyActive = st.sides[0].active || st.sides[1].active;
    }

    // Runs once per rendered frame, after the game's own update.
    inline void tick() {
        // Diagnostic: time since the previous frame, in ms.
        static auto lastFrame = Clock::now();
        const auto nowFrame = Clock::now();
        const double frameMs = msBetween(lastFrame, nowFrame);
        lastFrame = nowFrame;

        auto* pl = PlayLayer::get();
        if (!pl) {
            g_delayQ.clear();
            if (state().anyActive) resetSides();
            return;
        }

        updateLabel(pl);

        static int slowLogs = 0;
        if (frameMs > 25.0 && slowLogs < 20) {
            ++slowLogs;
            log::info("JW: diag slow frame {:.1f} ms", frameMs);
        }

        auto& st = state();

        // Deliver delayed wave inputs (flushed even if the assist was just
        // switched off, so a release is never lost).
        if (!g_delayQ.empty()) {
            for (auto it = g_delayQ.begin(); it != g_delayQ.end();) {
                if (--it->framesLeft <= 0) {
                    st.injecting = true;
                    pl->handleButton(it->down, kButtonJump, it->p1);
                    st.injecting = false;
                    it = g_delayQ.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Fallback driver: if the physics-step hook is not running, this keeps
        // the assist working at frame resolution.
        runBuffer(pl, "tick");
    }
}

$on_mod(Loaded) {
    log::info("JW: build 9 loaded (engine-validated hold, same-contact only)");
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
        log::info("JW: window changed to {} ms", jw::g_windowMs);
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

        if (st.injecting || button != jw::kButtonJump || !jw::assistActive()) {
            if (down && !st.injecting && button == jw::kButtonJump) {
                jw::logDecision(
                    jw::g_enabled ? "PASS: assist inactive (practice-only outside practice, or no level)"
                                  : "PASS: assist inactive (Enabled is off)",
                    isPlayer1 ? m_player1 : m_player2);
            }
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        static bool logged = false;
        if (!logged) { logged = true; log::info("JW: handleButton intercept running"); }

        PlayerObject* p = isPlayer1 ? m_player1 : m_player2;
        auto& w = st.sides[isPlayer1 ? 0 : 1];

        const int side = isPlayer1 ? 0 : 1;
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
            // Optional fixed, uniform delay for presses and releases alike.
            if (jw::g_waveDelay > 0) {
                jw::g_delayQ.push_back({down, isPlayer1, jw::g_waveDelay});
                return;
            }
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        jw::g_waveDown[side] = false; // not in wave: nothing to track

        if (down) {
            // Window of 0 or unsupported mode: vanilla behavior.
            if (jw::g_windowMs <= 0 || !jw::isCubeLike(p)) {
                jw::logDecision("PASS: window is 0 or mode not supported", p);
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
                return;
            }
            // Jump already legal (on ground or touching an orb): vanilla
            // timing, no swallow and no added latency.
            if (p->m_isOnGround || jw::touchingOrb(p)) {
                jw::logDecision("PASS: on ground or touching orb", p);
                jw::logReplaced(w);
                if (w.active && w.pressed) {   // release our own injected press first
                    GJBaseGameLayer::handleButton(false, button, isPlayer1);
                }
                w = jw::Side{};
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
                return;
            }
            // Airborne with nothing to click yet: swallow the click and hold it
            // until contact (or until the time window runs out).
            jw::logDecision("SWALLOWED: airborne, holding click for the window", p);
            jw::logReplaced(w);
            if (w.active && w.pressed) {       // release our own injected press first
                GJBaseGameLayer::handleButton(false, button, isPlayer1);
            }
            w = jw::Side{};
            w.active       = true;
            w.clickedInAir = true;
            w.clickTime    = jw::Clock::now();
            st.anyActive   = true;
            return;
        }

        // Release while holding: remember it, apply it when the buffer ends.
        if (w.active) {
            w.releaseDeferred = true;
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Runs once per physics step. Original first (it feeds queued clicks through
    // handleButton above), then let the buffer act inside this very step.
    void processQueuedButtons(float dt, bool clearInputQueue) {
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);

        GJBaseGameLayer* self = this;
        if (!typeinfo_cast<PlayLayer*>(self)) return;   // PlayLayer only, not the editor

        if (!jw::g_stepHookSeen) {
            jw::g_stepHookSeen = true;
            log::info("JW: physics-step hook is running");
        }
        jw::runBuffer(self, "step");
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
        jw::reset();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        jw::reset();
        PlayLayer::onQuit();
    }
};
