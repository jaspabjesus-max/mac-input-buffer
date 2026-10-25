#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include <CoreGraphics/CoreGraphics.h>

#include <chrono>
#include <cstring>
#include <vector>

using namespace geode::prelude;

namespace jw {
    constexpr int    kButtonJump = 1;
    constexpr double kJumpVel    = 5.0; // upward speed that means "a jump fired"

    struct Side {
        bool active          = false; // an early click is being held
        bool releaseDeferred = false; // player let go while we were holding
        bool pressed         = false; // we have a press in flight
        bool wasGround       = false;
        bool clickedInAir    = false; // diagnostic: was the click made while airborne
        int  framesLeft      = 0;
    };

    struct State {
        bool injecting = false;
        bool anyActive = false;
        Side sides[2];
    };

    // Uniform delay line for wave inputs.
    struct Delayed { bool down; bool p1; int framesLeft; };

    inline State& state() {
        static State s;
        return s;
    }

    inline bool g_enabled      = true;
    inline bool g_practiceOnly = false;
    inline int  g_window       = 2;
    inline int  g_waveDelay    = 0;
    inline std::vector<Delayed> g_delayQ;

    // Wave input guard state.
    inline bool g_waveGuard      = false;
    inline bool g_waveDown[2]    = {false, false}; // last wave state forwarded to the game
    inline int  g_stuckFrames[2] = {0, 0};
    inline int  g_guardLogs      = 0;

    // True if ANY mouse button or keyboard key is physically down. Checking
    // everything (not just default jump keys) means a rebound key can never
    // look "released" while you are still holding it.
    inline bool physicalJumpHeld() {
        const auto src = kCGEventSourceStateHIDSystemState;
        const CGMouseButton buttons[] = {kCGMouseButtonLeft, kCGMouseButtonRight, kCGMouseButtonCenter};
        for (auto b : buttons) {
            if (CGEventSourceButtonState(src, b)) return true;
        }
        for (CGKeyCode k = 0; k < 128; ++k) {
            if (CGEventSourceKeyState(src, k)) return true;
        }
        return false;
    }

    // Read-only latency diagnostics: these only log, they never change input.
    inline int g_diagLogs = 0;
    inline double clickAgeMs(bool mouse) {
        const auto src = kCGEventSourceStateHIDSystemState;
        const double a = CGEventSourceSecondsSinceLastEventType(
            src, mouse ? kCGEventLeftMouseDown : kCGEventKeyDown);
        return a < 0.0 ? -1.0 : a * 1000.0;
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
        g_stuckFrames[0] = g_stuckFrames[1] = 0;
    }

    inline bool assistActive() {
        auto* pl = PlayLayer::get();
        if (!g_enabled || !pl) return false;
        if (g_practiceOnly && !pl->m_isPracticeMode) return false;
        return true;
    }

    // One-off-action modes: cube, ball, robot, spider.
    // Ship, UFO, wave and swing pass through the early-click assist untouched.
    inline bool isCubeLike(PlayerObject* p) {
        return p && !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
    }

    inline bool touchingOrb(PlayerObject* p) {
        return p && p->m_touchingRings && p->m_touchingRings->count() > 0;
    }

    inline bool jumpFired(PlayerObject* p, bool wasGround) {
        const double vel = static_cast<double>(p->m_yVelocity);
        const double up  = p->m_isUpsideDown ? -vel : vel;
        return (wasGround && !p->m_isOnGround) || up > kJumpVel;
    }

    inline int ringCount(PlayerObject* p) {
        return (p && p->m_touchingRings) ? static_cast<int>(p->m_touchingRings->count()) : 0;
    }

    inline void logReplaced(const Side& w) {
        static int n = 0;
        if (w.active && n < 100) {
            ++n;
            log::info("JW: previous buffer replaced by a new click (pressed={}, framesLeft={})",
                w.pressed, w.framesLeft);
        }
    }

    // Diagnostic: which path did a real click take, and why?
    // Counters + a summary every 25 clicks, so the log never runs out of budget.
    inline int g_cntTotal = 0, g_cntReady = 0, g_cntSwallowed = 0, g_cntUnsupported = 0, g_cntInactive = 0;
    inline void logDecision(const char* why, PlayerObject* p) {
        const bool swallowed = std::strncmp(why, "SWALLOWED", 9) == 0;
        if (swallowed)                          ++g_cntSwallowed;
        else if (std::strstr(why, "on ground")) ++g_cntReady;
        else if (std::strstr(why, "not supported")) ++g_cntUnsupported;
        else                                    ++g_cntInactive;
        ++g_cntTotal;
        if (g_cntTotal % 25 == 0) {
            log::info("JW: summary after {} clicks: passed-through(ground/orb)={} swallowed={} unsupported-mode={} inactive={}",
                g_cntTotal, g_cntReady, g_cntSwallowed, g_cntUnsupported, g_cntInactive);
        }

        // Always log swallowed clicks (up to 300); log only the first 40 others.
        static int detail = 0;
        if (swallowed) { if (g_cntSwallowed > 300) return; }
        else           { if (detail >= 40) return; ++detail; }

        log::info("JW: click -> {} | window={} ground={} rings={} ship={} ufo={} ball={} wave={} robot={} spider={} swing={}",
            why, g_window,
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

    // Runs once per rendered frame, after the game's own update.
    inline void tick() {
        // Diagnostic: time since the previous frame, in ms.
        static auto lastFrame = std::chrono::steady_clock::now();
        const auto nowFrame = std::chrono::steady_clock::now();
        const double frameMs =
            std::chrono::duration<double, std::milli>(nowFrame - lastFrame).count();
        lastFrame = nowFrame;

        auto* pl = PlayLayer::get();
        if (!pl) {
            g_delayQ.clear();
            if (state().anyActive) resetSides();
            return;
        }

        updateLabel(pl);

        static int slowLogs = 0;
        if (frameMs > 25.0 && slowLogs < 40) {
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

        if (!st.anyActive) return;

        // Assist switched off mid-buffer: stop immediately.
        if (!assistActive()) { resetSides(); return; }

        static bool logged = false;
        if (!logged) { logged = true; log::info("JW: scheduler tick running"); }

        for (int i = 0; i < 2; ++i) {
            auto& w = st.sides[i];
            if (!w.active) continue;

            const bool p1 = (i == 0);
            PlayerObject* p = p1 ? pl->m_player1 : pl->m_player2;
            if (!p || !isCubeLike(p)) {
                if (w.pressed && p) {   // do not leave our injected press held
                    st.injecting = true;
                    pl->handleButton(false, kButtonJump, p1);
                    st.injecting = false;
                }
                w = Side{};
                continue;
            }

            auto send = [&](bool down) {
                st.injecting = true;
                pl->handleButton(down, kButtonJump, p1);
                st.injecting = false;
            };

            // 1. We already pressed once for this click. ONE press per click:
            //    never retry, because a retry would press the NEXT orb the
            //    player reaches, an input they never made.
            if (w.pressed) {
                const bool fired = jumpFired(p, w.wasGround);
                static int outcomeLogs = 0;
                if (outcomeLogs < 300) {
                    ++outcomeLogs;
                    log::info("JW: press done (looks like a jump: {}, frames used: {}/{}, vel={:.2f}, ground={}, rings={})",
                        fired, g_window - w.framesLeft, g_window,
                        static_cast<double>(p->m_yVelocity), p->m_isOnGround, ringCount(p));
                }
                if (w.releaseDeferred) send(false);   // player already let go
                w = Side{};
                continue;
            }

            // 2. Window used up: stop and apply any held-back release.
            if (w.framesLeft <= 0) {
                static int expiredLogs = 0;
                if (expiredLogs < 300) {
                    ++expiredLogs;
                    log::info("JW: buffer expired, no jump (clicked in air: {})", w.clickedInAir);
                }
                if (!w.releaseDeferred) {
                    // Still physically holding: give the game the held press
                    // back, exactly as vanilla would have had it.
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

            // 3. Only press when a jump can actually happen.
            if (p->m_isOnGround || touchingOrb(p)) {
                static int pressLogs = 0;
                if (pressLogs < 300) {
                    ++pressLogs;
                    log::info("JW: tick pressed (ground={} orb={} framesLeft={})",
                        p->m_isOnGround, touchingOrb(p), w.framesLeft);
                }
                w.wasGround = p->m_isOnGround;
                send(true);
                w.pressed = true;
            }
            --w.framesLeft;
        }

        st.anyActive = st.sides[0].active || st.sides[1].active;
    }
}

$on_mod(Loaded) {
    log::info("JW: build 7 loaded (one press per click, no retries, wave watchdog removed)");
    jw::g_enabled      = Mod::get()->getSettingValue<bool>("assist-enabled");
    jw::g_practiceOnly = Mod::get()->getSettingValue<bool>("practice-only");
    jw::g_window       = static_cast<int>(Mod::get()->getSettingValue<int64_t>("window-frames"));
    jw::g_waveDelay    = static_cast<int>(Mod::get()->getSettingValue<int64_t>("wave-delay"));
    log::info("JW: loaded settings enabled={} practiceOnly={} window={} waveDelay={}",
        jw::g_enabled, jw::g_practiceOnly, jw::g_window, jw::g_waveDelay);
    jw::g_waveGuard    = Mod::get()->getSettingValue<bool>("wave-guard");

    listenForSettingChanges<bool>("assist-enabled",   [](bool v)    { jw::g_enabled = v; });
    listenForSettingChanges<bool>("practice-only",    [](bool v)    { jw::g_practiceOnly = v; });
    listenForSettingChanges<int64_t>("window-frames", [](int64_t v) {
        jw::g_window = static_cast<int>(v);
        log::info("JW: window changed to {}", jw::g_window);
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
        if (down && !st.injecting && button == jw::kButtonJump && jw::g_diagLogs < 60) {
            ++jw::g_diagLogs;
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
            // Window of 1 or non-cube-like modes: vanilla behavior.
            if (jw::g_window <= 1 || !jw::isCubeLike(p)) {
                jw::logDecision("PASS: window is 1 or mode not supported", p);
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
                return;
            }
            // Jump is already legal (on ground or touching an orb): vanilla
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
            // Airborne with nothing to click yet: swallow the click; the
            // tick presses once a jump becomes legal.
            jw::logDecision("SWALLOWED: airborne, holding click for the window", p);
            jw::logReplaced(w);
            if (w.active && w.pressed) {       // release our own injected press first
                GJBaseGameLayer::handleButton(false, button, isPlayer1);
            }
            w = jw::Side{};
            w.active     = true;
            w.framesLeft = jw::g_window;
            w.clickedInAir = p && !p->m_isOnGround;
            st.anyActive = true;
            return;
        }

        // Release while holding: remember it, apply it when the buffer ends.
        if (w.active) {
            w.releaseDeferred = true;
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
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
