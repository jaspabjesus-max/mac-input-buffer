#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include <CoreGraphics/CoreGraphics.h>

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

    // True if the mouse button or a default jump key is physically down.
    inline bool physicalJumpHeld() {
        const auto src = kCGEventSourceStateHIDSystemState;
        if (CGEventSourceButtonState(src, kCGMouseButtonLeft)) return true;
        for (CGKeyCode k : {(CGKeyCode)0x31, (CGKeyCode)0x7E, (CGKeyCode)0x0D}) { // Space, Up, W
            if (CGEventSourceKeyState(src, k)) return true;
        }
        return false;
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

    inline void updateLabel(PlayLayer* pl) {
        if (auto* label = pl->getChildByID("jw-label")) {
            label->setVisible(assistActive());
        }
    }

    // Runs once per rendered frame, after the game's own update.
    inline void tick() {
        auto* pl = PlayLayer::get();
        if (!pl) {
            g_delayQ.clear();
            if (state().anyActive) resetSides();
            return;
        }

        updateLabel(pl);

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

        // Stuck-hold watchdog: release only if the hardware says the button is up.
        if (g_waveGuard && g_delayQ.empty() && assistActive()) {
            for (int i = 0; i < 2; ++i) {
                if (!g_waveDown[i] || physicalJumpHeld()) { g_stuckFrames[i] = 0; continue; }
                if (++g_stuckFrames[i] >= 3) {   // 3 frames avoids racing a normal release
                    st.injecting = true;
                    pl->handleButton(false, kButtonJump, i == 0);
                    st.injecting = false;
                    g_waveDown[i] = false;
                    g_stuckFrames[i] = 0;
                    if (g_guardLogs < 20) {
                        ++g_guardLogs;
                        log::info("JW: released stuck wave hold (side {})", i);
                    }
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
            if (!p || !isCubeLike(p)) { w = Side{}; continue; }

            auto send = [&](bool down) {
                st.injecting = true;
                pl->handleButton(down, kButtonJump, p1);
                st.injecting = false;
            };

            // 1. Did our last press make the jump fire? Then we are done.
            if (w.pressed) {
                if (jumpFired(p, w.wasGround)) {
                    log::info("JW: jump fired, buffer done");
                    if (w.releaseDeferred) send(false);
                    w = Side{};
                    continue;
                }
                send(false);       // not legal after all: release so the next press is fresh
                w.pressed = false;
            }

            // 2. Window used up: stop and apply any held-back release.
            if (w.framesLeft <= 0) {
                if (w.releaseDeferred) send(false);
                w = Side{};
                continue;
            }

            // 3. Only press when a jump can actually happen.
            if (p->m_isOnGround || touchingOrb(p)) {
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
    jw::g_enabled      = Mod::get()->getSettingValue<bool>("assist-enabled");
    jw::g_practiceOnly = Mod::get()->getSettingValue<bool>("practice-only");
    jw::g_window       = static_cast<int>(Mod::get()->getSettingValue<int64_t>("window-frames"));
    jw::g_waveDelay    = static_cast<int>(Mod::get()->getSettingValue<int64_t>("wave-delay"));
    jw::g_waveGuard    = Mod::get()->getSettingValue<bool>("wave-guard");

    listenForSettingChanges<bool>("assist-enabled",   [](bool v)    { jw::g_enabled = v; });
    listenForSettingChanges<bool>("practice-only",    [](bool v)    { jw::g_practiceOnly = v; });
    listenForSettingChanges<int64_t>("window-frames", [](int64_t v) { jw::g_window = static_cast<int>(v); });
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

        if (st.injecting || button != jw::kButtonJump || !jw::assistActive()) {
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
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
                return;
            }
            // Swallow the click; the tick presses once a jump is legal.
            w = jw::Side{};
            w.active     = true;
            w.framesLeft = jw::g_window;
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
