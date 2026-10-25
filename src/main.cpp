#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>

using namespace geode::prelude;

namespace jw {
    constexpr int    kButtonJump = 1;
    constexpr double kJumpVel    = 5.0; // upward speed that means "a jump fired"

    struct Side {
        bool   active          = false; // a swallowed click is being retried
        bool   releaseDeferred = false; // user let go while we were retrying
        int    stepsLeft       = 0;
        bool   wasGround       = false; // snapshot taken when we last pressed
    };

    struct State {
        bool injecting = false;
        bool anyActive = false;
        Side sides[2];
    };

    inline State& state() {
        static State s;
        return s;
    }

    inline bool g_enabled = true;
    inline int  g_window  = 6;

    inline void reset() {
        auto& s = state();
        s.sides[0] = Side{};
        s.sides[1] = Side{};
        s.injecting = false;
        s.anyActive = false;
    }

    inline bool isPlayLayer(GJBaseGameLayer* layer) {
        return layer && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == layer;
    }

    inline bool isCubeLike(PlayerObject* p) {
        return p && !(p->m_isShip || p->m_isBird || p->m_isBall || p->m_isDart ||
                      p->m_isRobot || p->m_isSpider || p->m_isSwing);
    }

    inline bool jumpFired(PlayerObject* p, bool wasGround) {
        const double vel = static_cast<double>(p->m_yVelocity);
        const double up  = p->m_isUpsideDown ? -vel : vel;
        return (wasGround && !p->m_isOnGround) || up > kJumpVel;
    }
}

$on_mod(Loaded) {
    jw::g_enabled = Mod::get()->getSettingValue<bool>("assist-enabled");
    jw::g_window  = static_cast<int>(Mod::get()->getSettingValue<int64_t>("window-frames"));
    listenForSettingChanges<bool>("assist-enabled",   [](bool v)    { jw::g_enabled = v; });
    listenForSettingChanges<int64_t>("window-frames", [](int64_t v) { jw::g_window = static_cast<int>(v); });
}

class $modify(JWGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& st = jw::state();

        if (st.injecting || !jw::g_enabled || button != jw::kButtonJump || !jw::isPlayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        PlayerObject* p = isPlayer1 ? m_player1 : m_player2;
        auto& w = st.sides[isPlayer1 ? 0 : 1];

        if (down) {
            // Window of 1 or non-cube modes: vanilla behavior.
            if (jw::g_window <= 1 || !jw::isCubeLike(p)) {
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
                return;
            }
            // SWALLOW the click. The retry loop will press for us.
            w = jw::Side{};
            w.active    = true;
            w.stepsLeft = jw::g_window;
            st.anyActive = true;
            return;
        }

        // Release while retrying: remember it, apply it when the loop ends.
        if (w.active) {
            w.releaseDeferred = true;
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processQueuedButtons(float dt, bool clearInputQueue) {
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);

        auto& st = jw::state();
        if (!st.anyActive) return;
        if (!jw::isPlayLayer(this)) { jw::reset(); return; }

        for (int i = 0; i < 2; ++i) {
            auto& w = st.sides[i];
            if (!w.active) continue;

            const bool p1 = (i == 0);
            PlayerObject* p = p1 ? m_player1 : m_player2;
            if (!p || !jw::isCubeLike(p)) { w = jw::Side{}; continue; }

            auto send = [&](bool down) {
                st.injecting = true;
                this->handleButton(down, jw::kButtonJump, p1);
                st.injecting = false;
            };

            // stepsLeft == g_window means we have not pressed yet.
            const bool pressedBefore = (w.stepsLeft < jw::g_window);

            if (pressedBefore) {
                // Did our last press make the jump fire during the last update?
                if (jw::jumpFired(p, w.wasGround)) {
                    // Perfect timing found: stop swallowing, back to normal.
                    if (w.releaseDeferred) send(false);
                    w = jw::Side{};
                    continue;
                }
                if (w.stepsLeft <= 0) {
                    // Window used up without a jump.
                    if (w.releaseDeferred) send(false);
                    w = jw::Side{};
                    continue;
                }
                send(false); // swallow it back up so the next press is fresh
            }

            w.wasGround = p->m_isOnGround;
            send(true);
            --w.stepsLeft;
        }

        st.anyActive = st.sides[0].active || st.sides[1].active;
    }
};

class $modify(JWPlayLayer, PlayLayer) {
    void resetLevel() {
        jw::reset();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        jw::reset();
        PlayLayer::onQuit();
    }
};
