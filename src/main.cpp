#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace jw {
    constexpr int    kButtonJump = 1;
    constexpr double kJumpDelta  = 3.0; // vertical velocity change that counts as "jump happened"

    // One retry window per player side (0 = player 1, 1 = player 2).
    struct Side {
        bool   active          = false;
        bool   fresh           = false; // created during the current step, skip retry once
        bool   releaseDeferred = false;
        int    stepsLeft       = 0;
        bool   wasGround       = false;
        double lastVel         = 0.0;
    };

    struct State {
        bool injecting = false;
        Side sides[2];
    };

    inline State& state() {
        static State s;
        return s;
    }

    inline void reset() {
        state().sides[0] = Side{};
        state().sides[1] = Side{};
        state().injecting = false;
    }

    inline bool isPlayLayer(GJBaseGameLayer* layer) {
        return layer && typeinfo_cast<PlayLayer*>(layer) != nullptr;
    }
}

class $modify(JWGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& st = jw::state();

        if (st.injecting || button != jw::kButtonJump || !jw::isPlayLayer(this) ||
            !Mod::get()->getSettingValue<bool>("assist-enabled")) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto& w = st.sides[isPlayer1 ? 0 : 1];

        if (down) {
            PlayerObject* p = isPlayer1 ? m_player1 : m_player2;
            const int window = static_cast<int>(Mod::get()->getSettingValue<int64_t>("window-frames"));

            w = jw::Side{};
            w.active    = true;
            w.fresh     = true;
            w.stepsLeft = std::max(0, window - 1);
            w.wasGround = p && p->m_isOnGround;
            w.lastVel   = p ? static_cast<double>(p->m_yVelocity) : 0.0;

            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Release: hold it back until the window finishes.
        if (w.active) {
            w.releaseDeferred = true;
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processQueuedButtons(float dt, bool clearInputQueue) {
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);

        if (!jw::isPlayLayer(this)) return;
        auto& st = jw::state();

        for (int i = 0; i < 2; ++i) {
            auto& w = st.sides[i];
            if (!w.active) continue;

            const bool p1 = (i == 0);
            PlayerObject* p = p1 ? m_player1 : m_player2;
            if (!p) { w = jw::Side{}; continue; }

            // The click was created during this very step, before the player
            // has been updated. Take a baseline and wait for the next step.
            if (w.fresh) {
                w.fresh     = false;
                w.wasGround = p->m_isOnGround;
                w.lastVel   = static_cast<double>(p->m_yVelocity);
                continue;
            }

            const double vel = static_cast<double>(p->m_yVelocity);
            const bool consumed =
                (w.wasGround && !p->m_isOnGround) ||
                std::fabs(vel - w.lastVel) > jw::kJumpDelta;

            auto send = [&](bool down) {
                st.injecting = true;
                this->handleButton(down, jw::kButtonJump, p1);
                st.injecting = false;
            };

            if (!consumed && w.stepsLeft > 0) {
                // Re-click: release then press, so orbs and pads see a fresh press.
                send(false);
                send(true);
                --w.stepsLeft;
                w.wasGround = p->m_isOnGround;
                w.lastVel   = vel;
                continue;
            }

            // Window over (jump happened or steps used up).
            if (w.releaseDeferred) send(false);
            w = jw::Side{};
        }
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
