#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <vector>

using namespace geode::prelude;

// ===========================================================================
// Core: timestamps, buffers, and step-accurate draining
// All of this runs on the main thread (GLFW polling, scheduler and physics
// share it), so no locking is needed.
// ===========================================================================
namespace ib {
    using Clock     = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    constexpr int    kButtonJump        = 1;
    constexpr int    kGlfwRelease       = 0;
    constexpr int    kGlfwPress         = 1;
    constexpr int    kGlfwRepeat        = 2;
    constexpr int    kGlfwMouseLeft     = 0;
    constexpr int    kGlfwKeySpace      = 32;
    constexpr int    kGlfwKeyW          = 87;
    constexpr int    kGlfwKeyUp         = 265;
    constexpr double kMaxEventAge       = 0.25;   // drop buffered inputs older than this (s)
    constexpr double kMaxHardwareAge    = 0.10;   // ignore implausible OS event ages (s)
    constexpr double kDefaultStep       = 1.0 / 240.0;
    constexpr size_t kMaxHardwareQueue  = 16;

    inline Clock::duration toDuration(double seconds) {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    }

    inline double toSeconds(Clock::duration d) {
        return std::chrono::duration<double>(d).count();
    }

    // A raw hardware event captured in a GLFW callback.
    struct HwEvent {
        bool      down;
        TimePoint when;
    };

    // A jump input waiting to be applied at its matching physics step.
    struct Buffered {
        bool      down;
        int       button;
        bool      isPlayer1;
        TimePoint when;
    };

    struct State {
        bool                  enabled   = true;
        bool                  injecting = false;
        bool                  haveLast  = false;
        TimePoint             lastUpdate{};
        TimePoint             frameStart{};   // real-time start of the frame being simulated
        double                simElapsed = 0; // simulated seconds consumed so far this frame
        std::deque<HwEvent>   hw;
        std::vector<Buffered> pending;
    };

    inline State& state() {
        static State s;
        return s;
    }

    inline void reset() {
        auto& s = state();
        s.hw.clear();
        s.pending.clear();
    }

    inline bool isPlayLayer(GJBaseGameLayer* layer) {
        return layer && typeinfo_cast<PlayLayer*>(layer) != nullptr;
    }

    // Seconds between the real hardware event and "now". Because GLFW delivers
    // events at poll time, the OS-level event age is what recovers the true
    // click time. HID state = physical devices only.
    inline double hardwareAge(bool down, bool mouse) {
        const CGEventType type = mouse
            ? (down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp)
            : (down ? kCGEventKeyDown       : kCGEventKeyUp);
        const double age = CGEventSourceSecondsSinceLastEventType(
            kCGEventSourceStateHIDSystemState, type
        );
        if (age < 0.0 || age > kMaxHardwareAge) return 0.0;
        return age;
    }

    // Called from the GLFW callbacks.
    inline void recordHardware(bool down, bool mouse) {
        auto& s = state();
        if (!s.enabled || !PlayLayer::get()) return;

        const TimePoint when = Clock::now() - toDuration(hardwareAge(down, mouse));
        if (s.hw.size() >= kMaxHardwareQueue) s.hw.pop_front();
        s.hw.push_back({down, when});
    }

    // Called from handleButton: find the oldest hardware event with a matching
    // direction. Falls back to "now" (vanilla timing) if none is available.
    inline TimePoint takeHardwareTime(bool down) {
        auto& s = state();
        const TimePoint now = Clock::now();

        while (!s.hw.empty() && toSeconds(now - s.hw.front().when) > kMaxEventAge) {
            s.hw.pop_front();
        }
        for (auto it = s.hw.begin(); it != s.hw.end(); ++it) {
            if (it->down == down) {
                const TimePoint t = it->when;
                s.hw.erase(it);
                return t;
            }
        }
        return now;
    }

    // Advance one physics step and hand every buffered input whose real
    // timestamp falls at or before the end of that step to `apply`.
    template <class Apply>
    void drain(float stepDt, Apply&& apply) {
        auto& s = state();
        s.simElapsed += (stepDt > 0.f) ? static_cast<double>(stepDt) : kDefaultStep;
        if (s.pending.empty()) return;

        const TimePoint now    = Clock::now();
        const TimePoint cutoff = s.enabled
            ? s.frameStart + toDuration(s.simElapsed)
            : TimePoint::max(); // mod disabled: flush everything immediately

        std::stable_sort(s.pending.begin(), s.pending.end(),
            [](const Buffered& a, const Buffered& b) { return a.when < b.when; });

        std::vector<Buffered> due;
        auto it = s.pending.begin();
        while (it != s.pending.end() && it->when <= cutoff) {
            due.push_back(*it);
            ++it;
        }
        s.pending.erase(s.pending.begin(), it);

        for (const auto& in : due) {
            if (toSeconds(now - in.when) > kMaxEventAge) continue; // stale (e.g. after a pause)
            apply(in);
        }
    }
}

// ===========================================================================
// Engine update loop: marks the real-time window of each rendered frame
// ===========================================================================
class $modify(IBScheduler, CCScheduler) {
    void update(float dt) {
        auto& s = ib::state();
        const ib::TimePoint now = ib::Clock::now();

        s.enabled    = Mod::get()->getSettingValue<bool>("enabled");
        s.frameStart = s.haveLast ? s.lastUpdate : now - ib::toDuration(dt);
        s.lastUpdate = now;
        s.haveLast   = true;
        s.simElapsed = 0.0;

        CCScheduler::update(dt);
    }
};

// ===========================================================================
// GLFW window events: stamp left-click and jump-key events with true time
// ===========================================================================
class $modify(IBEGLView, CCEGLView) {
    void onGLFWMouseCallBack(GLFWwindow* window, int button, int action, int mods) {
        if (button == ib::kGlfwMouseLeft &&
            (action == ib::kGlfwPress || action == ib::kGlfwRelease)) {
            ib::recordHardware(action == ib::kGlfwPress, true);
        }
        CCEGLView::onGLFWMouseCallBack(window, button, action, mods);
    }

    void onGLFWKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
        const bool jumpKey = key == ib::kGlfwKeySpace || key == ib::kGlfwKeyUp || key == ib::kGlfwKeyW;
        if (jumpKey && action != ib::kGlfwRepeat) {
            ib::recordHardware(action == ib::kGlfwPress, false);
        }
        CCEGLView::onGLFWKeyCallback(window, key, scancode, action, mods);
    }
};

// ===========================================================================
// Input + physics: buffer jump presses, inject them at the matching step
// ===========================================================================
class $modify(IBGameLayer, GJBaseGameLayer) {
    // Intercept jump input and hold it (with its true timestamp) for injection.
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = ib::state();

        if (s.injecting || !s.enabled || button != ib::kButtonJump || !ib::isPlayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        s.pending.push_back({down, button, isPlayer1, ib::takeHardwareTime(down)});
    }

    // Runs once per physics step. After the game has fed its own queue
    // through handleButton (which buffers it), release every input that is
    // due at this step, before the player update that follows.
    void processQueuedButtons(float dt, bool clearInputQueue) {
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);

        if (!ib::isPlayLayer(this)) return;

        ib::drain(dt, [this](const ib::Buffered& in) {
            auto& s = ib::state();
            s.injecting = true;
            this->handleButton(in.down, in.button, in.isPlayer1);
            s.injecting = false;
        });
    }
};

// ===========================================================================
// Level lifecycle: never carry buffered inputs across restarts or exits
// ===========================================================================
class $modify(IBPlayLayer, PlayLayer) {
    void resetLevel() {
        ib::reset();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        ib::reset();
        PlayLayer::onQuit();
    }
};
