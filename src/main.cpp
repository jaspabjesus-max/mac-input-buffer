#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <chrono>
#include <vector>

using namespace geode::prelude;

// ===========================================================================
// Core: timestamps, buffer, and step-accurate draining.
// Everything runs on the main thread, so no locking is needed.
// ===========================================================================
namespace ib {
    using Clock     = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    constexpr int    kButtonJump     = 1;
    constexpr double kMaxEventAge    = 0.25;   // drop buffered inputs older than this (s)
    constexpr double kMaxHardwareAge = 0.10;   // ignore implausible OS event ages (s)
    constexpr double kDefaultStep    = 1.0 / 240.0;

    inline Clock::duration toDuration(double seconds) {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    }

    inline double toSeconds(Clock::duration d) {
        return std::chrono::duration<double>(d).count();
    }

    // A jump input waiting to be applied at its matching physics step.
    struct Buffered {
        bool      down;
        int       button;
        bool      isPlayer1;
        TimePoint when;
    };

    struct State {
        bool                  enabled    = true;
        bool                  injecting  = false;
        bool                  haveLast   = false;
        TimePoint             lastUpdate{};
        TimePoint             frameStart{};    // real-time start of the frame being simulated
        double                simElapsed = 0;  // simulated seconds consumed so far this frame
        std::vector<Buffered> pending;
    };

    inline State& state() {
        static State s;
        return s;
    }

    inline void reset() {
        state().pending.clear();
    }

    inline bool isPlayLayer(GJBaseGameLayer* layer) {
        return layer && typeinfo_cast<PlayLayer*>(layer) != nullptr;
    }

    // Seconds since the most recent physical event of this kind (mouse or
    // keyboard, press or release). HID state = physical devices only.
    inline double ageOf(CGEventType type) {
        const double age = CGEventSourceSecondsSinceLastEventType(
            kCGEventSourceStateHIDSystemState, type
        );
        if (age < 0.0 || age > kMaxHardwareAge) return -1.0;
        return age;
    }

    // Real time of the physical input that produced this button event.
    // Takes whichever of mouse/keyboard happened most recently. Falls back to
    // "now" (vanilla timing) if the OS reports nothing plausible.
    inline TimePoint inputTime(bool down) {
        const double mouse = ageOf(down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp);
        const double key   = ageOf(down ? kCGEventKeyDown       : kCGEventKeyUp);

        double age = 0.0;
        if (mouse >= 0.0 && key >= 0.0) age = std::min(mouse, key);
        else if (mouse >= 0.0)          age = mouse;
        else if (key >= 0.0)            age = key;

        return Clock::now() - toDuration(age);
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

        s.pending.push_back({down, button, isPlayer1, ib::inputTime(down)});
    }

    // Runs once per physics step. After the game has fed its own queue
    // through handleButton (which buffers it), release every input that is
    // due at this step.
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
