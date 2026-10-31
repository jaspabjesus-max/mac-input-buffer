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

    // Predictive layer.
    constexpr int kPredictHorizon    = 90;  // ticks the landing predictor looks ahead
    constexpr int kPredictExtraSteps = 8;   // a held-back click may outlive the window by this much if landing is predicted
    constexpr int kCoyoteMinSteps    = 2;   // late-jump grace after walking off an edge (scaled from window-ms, clamped)
    constexpr int kCoyoteMaxSteps    = 6;

    // Survival planner. Uses the level's hazard / solid rectangles to find the tick
    // at which a buffered input survives. If the GAME OBJECT API calls in
    // collectNearby() / playerRect() do not compile against your SDK, define
    // JW_NO_GEOMETRY (next line) and the rest of the mod still builds.
    // #define JW_NO_GEOMETRY
    // 0 = off, 1 = shadow (plans and logs, never changes timing), 2 = active.
    constexpr int   kSurviveMode      = 2;
    constexpr int   kCubeMaxDelay     = 60;     // most ticks a cube jump may wait for the surviving tick
    constexpr float kShieldLookaheadUnits = 170.0f; // how far ahead the wave shield looks for a way out
    constexpr float kHazardMargin     = 2.0f;   // vanilla must be lethal with the hitbox this much smaller,
                                                //   and the alternative safe with it this much larger
    constexpr float kSolidShrink      = 1.0f;   // support contact must not count as a wall hit
    constexpr float kHazardInnerFrac  = 0.30f;  // hazards use the player box shrunk by this fraction per side
    constexpr int   kArcMax           = 256;

    // Per-side motion tracker, updated once per tick BEFORE inputs are processed.
    // All heights are in "up" coordinates (flipped when gravity is flipped).
    struct Track {
        bool     wasGround     = false;
        bool     haveGround    = false;
        bool     fellOff       = false;  // left the ground without jumping (walked off an edge)
        bool     usedCoyote    = false;  // the late jump for this fall has been spent
        bool     upsideKey     = false;
        int      samples       = 0;
        int      coyoteSince   = 0;      // ticks since ground when the late jump was granted
        uint64_t lastGroundSeq = 0;
        double   groundY       = 0.0;    // up-coordinate of the last ground contact
        double   Y0 = 0.0, Y1 = 0.0, Y2 = 0.0; // up-coordinate now / one tick ago / two ticks ago
        double   X0 = 0.0, X1 = 0.0;           // world x now / one tick ago
        // The player's own jump arc, learned from real jumps (per-tick change in height, up-coordinates).
        bool     arcValid = false, arcUpside = false;
        int      arcLen = 0;
        float    arc[kArcMax] = {};
        bool     recording = false, recBad = false;
        int      recLen = 0;
        float    rec[kArcMax] = {};
    };

    // One cushioned press per player side.
    struct Side {
        bool     active         = false; // a press we are streaming/cushioning is in flight
        bool     releaseQueued  = false; // player let go; the release sits in the tick queue
        bool     releaseOk      = false; // gate: the queued release may now be delivered
        bool     deferred       = false; // click was HELD BACK: the engine has not seen this press yet
        bool     registered     = false; // the engine acted on the press
        bool     pressGround    = false; // diagnostics
        int      pressRings     = 0;     // diagnostics
        int      predTicks      = -1;    // landing predicted at click time (-1: no prediction)
        bool     planned        = false; // survival planner already consulted for this click
        uint64_t sendAt         = 0;     // planner: do not deliver the held-back press before this tick
        // Snapshot from the previous step, used to detect that the press took.
        bool     prevGround     = false;
        bool     prevUpsideDown = false;
        int      prevRings      = 0;
        double   prevUp         = 0.0;
        uint64_t pressSeq       = 0;     // step counter when the player clicked
        uint64_t sentSeq        = 0;     // step counter when the press actually reached the engine
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
        bool     desired = false; // wave: updates the player's DESIRED state; the shield decides what the engine sees
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
    inline double g_tps = kNominalTps;   // fixed at the nominal rate (a measured value was too noisy to trust)
    inline int    g_waveEvents = 0, g_waveSpaced = 0; // wave inputs seen / saved from a same-tick collision

    inline double g_modUs = 0.0; // microseconds this mod spent since the last frame (diagnostic)
    inline std::vector<Event> g_queue;
    inline uint64_t g_seq      = 0;    // event ordering
    inline uint64_t g_stepSeq  = 0;    // physics-step counter (the only clock gameplay logic uses)
    inline uint64_t g_lastDue[2] = {0, 0};
    inline uint64_t g_lastDelivered[2] = {0, 0}; // tick of the last input actually delivered, per side
    inline Track    g_track[2];
    inline int      g_predN = 0;          // landing predictions that were checked against reality
    inline long     g_predErrAbs = 0;     // sum of |actual - predicted| ticks
    inline int      g_coyoteTried = 0, g_coyoteOk = 0;

    // Per-side wave motion tracker (positions in world coordinates).
    struct WaveTrack {
        double   X0 = 0.0, X1 = 0.0, Y0 = 0.0, Y1 = 0.0;
        int      samples = 0;
        int      dir = 0;                  // +1 rising, -1 falling (last non-flat tick)
        double   vx = 0.0, stepUp = 0.0, stepDown = 0.0;   // measured per-tick movement
        uint64_t lastEdgeSeq = 0;
        double   period = 6.0;             // learned ticks between your wave inputs
    };
    inline WaveTrack g_wt[2];
    inline int g_surviveCube = 0, g_surviveWave = 0;   // interventions (or shadow-mode "would have")

    // Wave shield state, per side.
    inline bool g_userHeld[2]    = {false, false};  // the key exactly as the player holds it (always updated)
    inline bool g_waveDesired[2] = {false, false};  // that state after the wave-delay setting: what the shield tries to follow
    inline bool g_engineHeld[2]  = {false, false};  // what the engine was last told while the shield runs
    inline bool g_shieldOn[2]    = {false, false};
    inline int  g_overrideTicks[2] = {0, 0};        // consecutive ticks the shield has been holding your click back
    inline uint64_t g_boxSeq = 0;                   // tick the nearby-object cache was built (0 = none)

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
        g_track[0] = g_track[1] = Track{};
        g_wt[0] = g_wt[1] = WaveTrack{};
        g_shieldOn[0] = g_shieldOn[1] = false;
        g_overrideTicks[0] = g_overrideTicks[1] = 0;
        g_boxSeq = 0;
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

    // ---- Predictive layer -------------------------------------------------
    // Called once per tick before the engine processes queued inputs.
    inline void trackAir(GJBaseGameLayer* layer) {
        for (int i = 0; i < 2; ++i) {
            PlayerObject* p = (i == 0) ? layer->m_player1 : layer->m_player2;
            Track& t = g_track[i];
            if (p && p->m_isDart) {
                WaveTrack& wt = g_wt[i];
                wt.X1 = wt.X0; wt.Y1 = wt.Y0;
                wt.X0 = p->getPositionX(); wt.Y0 = p->getPositionY();
                if (wt.samples < 2) ++wt.samples;
                if (wt.samples >= 2) {
                    const double dx = wt.X0 - wt.X1, dy = wt.Y0 - wt.Y1;
                    if (dx > 0.01) wt.vx = dx;
                    if (dy > 0.05)       { wt.stepUp = dy;    wt.dir = 1;  }
                    else if (dy < -0.05) { wt.stepDown = -dy; wt.dir = -1; }
                }
            } else {
                g_wt[i].samples = 0;
            }
            if (!p || !isCubeLike(p)) { t = Track{}; continue; }

            if (t.samples > 0 && p->m_isUpsideDown != t.upsideKey) t.samples = 0;   // gravity flip breaks the trajectory
            t.upsideKey = p->m_isUpsideDown;

            const double Y = (p->m_isUpsideDown ? -1.0 : 1.0) * static_cast<double>(p->getPositionY());
            t.Y2 = t.Y1; t.Y1 = t.Y0; t.Y0 = Y;
            t.X1 = t.X0; t.X0 = static_cast<double>(p->getPositionX());
            if (t.samples < 3) ++t.samples;

            if (p->m_isOnGround) {
                t.lastGroundSeq = g_stepSeq;
                t.groundY       = Y;
                t.haveGround    = true;
                t.fellOff       = false;
                t.usedCoyote    = false;
            } else if (t.wasGround) {
                t.fellOff = upVel(p) <= 0.0;   // left the ground NOT rising = walked off an edge
            }

            // Learn the jump arc from real jumps: per-tick height change from takeoff to landing.
            if (!p->m_isOnGround) {
                if (!t.recording && t.wasGround && upVel(p) > 1.0 && t.samples >= 2) {
                    t.recording = true; t.recLen = 0; t.recBad = false;
                }
                if (t.recording) {
                    if (ringCount(p) > 0) t.recBad = true;          // orbs change the arc
                    if (t.recLen < kArcMax) t.rec[t.recLen++] = static_cast<float>(t.Y0 - t.Y1);
                    else                    t.recBad = true;
                    if (t.recLen >= 5) {                            // constant gravity? pads / portals break this
                        const double a1 = t.rec[t.recLen - 1] - t.rec[t.recLen - 2];
                        const double a0 = t.rec[t.recLen - 2] - t.rec[t.recLen - 3];
                        if (std::fabs(a1 - a0) > 0.25 * std::fabs(a0) + 0.02) t.recBad = true;
                    }
                }
            } else if (t.recording) {
                if (!t.recBad && t.recLen >= 8) {
                    t.arcLen = t.recLen - 1;                        // the last tick is clipped by the landing
                    for (int k = 0; k < t.arcLen; ++k) t.arc[k] = t.rec[k];
                    t.arcValid  = true;
                    t.arcUpside = p->m_isUpsideDown;
                    static int arcLogs = 0;
                    if (arcLogs < 20) { ++arcLogs; log::info("JW: learned your jump arc: {} ticks", t.arcLen); }
                }
                t.recording = false;
            }
            t.wasGround = p->m_isOnGround;
        }
    }

    // Kinematic landing predictor. Learns the per-tick acceleration from the last
    // three positions (so it needs no hardcoded gravity, speed or size) and rolls
    // the arc forward until it returns to the last ground height. Returns the
    // number of ticks until landing, or -1 when it cannot say (no ground height
    // known, not under gravity, or the arc does not come back inside the horizon).
    // It assumes landing at the same height; platforms at other heights make it
    // wrong, which is why it only ever EXTENDS a window by a bounded amount.
    inline int predictLanding(const Track& t) {
        if (!t.haveGround || t.samples < 3) return -1;
        double dy = t.Y0 - t.Y1;
        const double a = dy - (t.Y1 - t.Y2);
        if (a > -0.01) return -1;
        double Y = t.Y0;
        for (int n = 1; n <= kPredictHorizon; ++n) {
            dy += a;
            Y  += dy;
            if (Y <= t.groundY) return n;
        }
        return -1;
    }

    // Late-jump grace ("coyote time"): a click that arrives a few ticks after the
    // player walked off an edge (without jumping) is treated as if it were still
    // on the ground. Marks the player grounded for this one press; finishCoyote()
    // checks whether the engine really jumped and undoes the flag if not.
    inline bool tryCoyote(PlayerObject* p, bool p1) {
        if (g_windowMs <= 0 || p->m_isOnGround || ringCount(p) > 0) return false;
        Track& t = g_track[sideIdx(p1)];
        if (!t.haveGround || !t.fellOff || t.usedCoyote) return false;
        const int windowSteps = static_cast<int>(std::lround(static_cast<double>(g_windowMs) * g_tps / 1000.0));
        const int coyoteSteps = std::clamp(windowSteps / 4, kCoyoteMinSteps, kCoyoteMaxSteps);
        const uint64_t since  = g_stepSeq - t.lastGroundSeq;
        if (since > static_cast<uint64_t>(coyoteSteps)) return false;
        t.usedCoyote  = true;
        t.coyoteSince = static_cast<int>(since);
        ++g_coyoteTried;
        p->m_isOnGround = true;
        return true;
    }

    inline void finishCoyote(PlayerObject* p, bool p1) {
        const bool jumped = !p->m_isOnGround || upVel(p) > kRegisterDeltaV;
        if (jumped) ++g_coyoteOk;
        else        p->m_isOnGround = false;      // the engine did not jump here: undo the flag
        static int logs = 0;
        if (logs < 200) {
            ++logs;
            log::info("JW: COYOTE late jump ({} ticks after leaving the ground): engine jumped inside handleButton: {} [{} of {} so far]",
                g_track[sideIdx(p1)].coyoteSince, jumped ? "YES" : "NO (flag undone)", g_coyoteOk, g_coyoteTried);
        }
    }

    // ---- Survival planner -------------------------------------------------
    struct Box   { float x0, y0, x1, y1; bool hazard; };
    struct Rect4 { float x0, y0, x1, y1; };

#ifndef JW_NO_GEOMETRY
    // The only code that touches level objects. Collects hazard and solid
    // rectangles near the player (once per planning call, not per tick).
    inline void collectNearby(double minX, double maxX, double minY, double maxY, std::vector<Box>& out) {
        out.clear();
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_objects) return;
        CCArrayExt<GameObject*> objs = pl->m_objects;
        for (auto* obj : objs) {
            if (!obj) continue;
            const double ox = obj->getPositionX();
            if (ox < minX - 150.0 || ox > maxX + 150.0) continue;      // cheap pre-filter, generous for wide blocks
            const GameObjectType type = obj->m_objectType;
            const bool hazard = (type == GameObjectType::Hazard);
            if (!hazard && type != GameObjectType::Solid) continue;
            if (obj->m_isDisabled || obj->m_isGroupDisabled || obj->m_isPassable) continue;
            const auto& r = obj->getObjectRect();
            if (r.origin.y + r.size.height < minY || r.origin.y > maxY) continue;
            out.push_back(Box{r.origin.x, r.origin.y, r.origin.x + r.size.width, r.origin.y + r.size.height, hazard});
        }
    }

    inline bool playerRect(PlayerObject* p, Rect4& out) {
        const auto& r = p->getObjectRect();
        if (r.size.width <= 0.0f || r.size.height <= 0.0f) return false;
        out = Rect4{r.origin.x, r.origin.y, r.origin.x + r.size.width, r.origin.y + r.size.height};
        static int rectLogs = 0;
        if (rectLogs < 3) { ++rectLogs; log::info("JW: player hitbox used by the planner: {:.1f} x {:.1f}", r.size.width, r.size.height); }
        return true;
    }
#else
    inline void collectNearby(double, double, double, double, std::vector<Box>& out) { out.clear(); }
    inline bool playerRect(PlayerObject*, Rect4&) { return false; }
#endif

    // The player's hazard hitbox. A box that is already small (wave) is used as is;
    // a large one (cube) is shrunk to its inner box. margin < 0 shrinks further
    // (lethal only when certain), margin > 0 grows it (safe only when certain).
    inline Rect4 hazardRect(const Rect4& pr, float margin) {
        const float w = pr.x1 - pr.x0, h = pr.y1 - pr.y0;
        const float mn   = std::min(w, h);
        const float frac = (mn < 16.0f) ? 0.0f : kHazardInnerFrac;
        const float m    = margin < 0.0f ? -std::min(-margin, 0.2f * mn) : margin;
        return Rect4{pr.x0 + w * frac - m, pr.y0 + h * frac - m, pr.x1 - w * frac + m, pr.y1 - h * frac + m};
    }

    // One tick of movement of the player box from offset (ox, oy) by (dx, dy).
    // Returns 0 = fine (new height in nyOut), 1 = hazard, 2 = wall, 3 = cube arc over (landed / bumped).
    inline int stepOnce(const std::vector<Box>& boxes, const Rect4& sr, const Rect4& hr,
                        float ox, float oy, float dx, float dy, bool slide, float& nyOut) {
        auto hit = [](const Rect4& r, float px, float py, const Box& b) {
            return r.x0 + px < b.x1 && r.x1 + px > b.x0 && r.y0 + py < b.y1 && r.y1 + py > b.y0;
        };
        const float nx = ox + dx;
        float       ny = oy + dy;
        for (const Box& b : boxes)                             // horizontal move: walls and spikes in front
            if (hit(b.hazard ? hr : sr, nx, oy, b)) return b.hazard ? 1 : 2;
        const bool vertical = dy != 0.0f;
        for (const Box& b : boxes) {                           // vertical move
            if (b.hazard) { if (hit(hr, nx, ny, b)) return 1; continue; }
            if (!vertical) continue;
            if (hit(sr, nx, ny, b)) {
                if (slide) ny = oy;                            // wave glides along it
                else       return 3;
            }
        }
        nyOut = ny;
        return 0;
    }

    // Rolls the player box along per-tick (dx, dy) steps against the nearby boxes.
    // Returns the 1-based tick of the first lethal contact, or 0 if none.
    // hazardMargin < 0 shrinks the hazard hitbox (lethal only when certain),
    // > 0 grows it (safe only when certain). slide: the wave glides along a
    // floor / ceiling instead of ending the simulation (the cube's arc ends there).
    inline int simPath(const std::vector<Box>& boxes, const Rect4& pr,
                       const std::vector<std::pair<float, float>>& steps,
                       float hazardMargin, bool slide, int& kind) {
        const Rect4 sr{pr.x0 + kSolidShrink, pr.y0 + kSolidShrink, pr.x1 - kSolidShrink, pr.y1 - kSolidShrink};
        const Rect4 hr = hazardRect(pr, hazardMargin);
        auto hit = [](const Rect4& r, float ox, float oy, const Box& b) {
            return r.x0 + ox < b.x1 && r.x1 + ox > b.x0 && r.y0 + oy < b.y1 && r.y1 + oy > b.y0;
        };
        float ox = 0.0f, oy = 0.0f;
        for (size_t t = 0; t < steps.size(); ++t) {
            const float nx = ox + steps[t].first;
            float       ny = oy + steps[t].second;
            for (const Box& b : boxes) {                       // horizontal move: walls and spikes in front
                if (hit(b.hazard ? hr : sr, nx, oy, b)) { kind = b.hazard ? 1 : 2; return static_cast<int>(t) + 1; }
            }
            const bool vertical = steps[t].second != 0.0f;
            for (const Box& b : boxes) {                       // vertical move
                if (b.hazard) {
                    if (hit(hr, nx, ny, b)) { kind = 1; return static_cast<int>(t) + 1; }
                    continue;
                }
                if (!vertical) continue;
                if (hit(sr, nx, ny, b)) {
                    if (slide) ny = oy;                        // wave slides along it
                    else       return 0;                       // cube landed or bumped: the arc is over
                }
            }
            ox = nx; oy = ny;
        }
        return 0;
    }

    // Cube: how many ticks after landing should the held-back jump wait so that
    // the learned arc clears everything? 0 = jump right away (vanilla).
    inline int planCubeDelay(PlayerObject* p, int i, int maxD, int& hitTick) {
        hitTick = 0;
        const Track& t = g_track[i];
        if (kSurviveMode == 0 || !t.arcValid || t.arcUpside != p->m_isUpsideDown) return 0;
        if (p->m_isBall || p->m_isRobot || p->m_isSpider) return 0;
        const double vx = t.X0 - t.X1;
        if (vx < 0.05 || t.samples < 2) return 0;
        Rect4 pr;
        if (!playerRect(p, pr)) return 0;
        const float sign = p->m_isUpsideDown ? -1.0f : 1.0f;

        static std::vector<Box> boxes;
        static std::vector<std::pair<float, float>> steps;
        collectNearby(pr.x0 - 80.0, pr.x1 + vx * (maxD + t.arcLen) + 80.0, pr.y0 - 400.0, pr.y1 + 400.0, boxes);
        if (boxes.empty()) return 0;

        auto build = [&](int d) {
            steps.clear();
            for (int k = 0; k < d + t.arcLen; ++k)
                steps.emplace_back(static_cast<float>(vx), k < d ? 0.0f : sign * t.arc[k - d]);
        };
        int kind = 0;
        build(0);
        const int now = simPath(boxes, pr, steps, -kHazardMargin, false, kind);
        if (now == 0) return 0;                                // vanilla survives (or we cannot tell): do nothing
        hitTick = now;
        for (int d = 1; d <= maxD; ++d) {
            build(d);
            int k2 = 0;
            if (simPath(boxes, pr, steps, +kHazardMargin, false, k2) == 0) return d;
        }
        return 0;                                              // nothing clearly safe: vanilla
    }

    // ---- Wave shield: model ------------------------------------------------
    struct WaveModel { double vx, su, sd; Rect4 pr; };

    inline bool waveModel(PlayerObject* p, int i, WaveModel& m) {
        const WaveTrack& w = g_wt[i];
        if (w.samples < 2 || w.vx < 0.05) return false;
        m.su = w.stepUp   > 0.05 ? w.stepUp   : w.stepDown;
        m.sd = w.stepDown > 0.05 ? w.stepDown : w.stepUp;
        if (m.su < 0.05) return false;
        m.vx = w.vx;
        return playerRect(p, m.pr);
    }

    // Nearby hazards / solids, rebuilt every ~30 ticks or when the player nears the edge of the cache.
    inline std::vector<Box> g_boxCache;
    inline double g_boxX0 = 0.0, g_boxX1 = -1.0;
    inline void ensureBoxes(double px, double reach, double py0, double py1) {
        const bool stale = g_boxSeq == 0 || g_stepSeq - g_boxSeq >= 30
                        || px - 100.0 < g_boxX0 || px + reach > g_boxX1;
        if (!stale) return;
        g_boxX0 = px - 300.0;
        g_boxX1 = px + reach + 900.0;
        collectNearby(g_boxX0, g_boxX1, py0 - 700.0, py1 + 700.0, g_boxCache);
        g_boxSeq = g_stepSeq;
    }

    // Is there ANY way to keep surviving for the next H ticks if the engine holds
    // (holdFirst) or releases for the coming tick? Every later tick is free to go
    // either way, so this is the set of heights the wave can still reach alive.
    inline bool waveViable(const WaveModel& m, bool holdFirst, bool upside) {
        const int holdDir = upside ? -1 : 1;
        const int H = std::clamp(static_cast<int>(std::ceil(kShieldLookaheadUnits / m.vx)), 20, 64);
        const double maxStep = std::max(m.su, m.sd);
        ensureBoxes(m.pr.x0, m.vx * H + 120.0, m.pr.y0, m.pr.y1);

        static std::vector<Box> local;
        local.clear();
        const float lx0 = m.pr.x0 - 10.0f, lx1 = m.pr.x1 + static_cast<float>(m.vx * H) + 10.0f;
        const float ly0 = m.pr.y0 - static_cast<float>(maxStep * H) - 10.0f;
        const float ly1 = m.pr.y1 + static_cast<float>(maxStep * H) + 10.0f;
        for (const Box& b : g_boxCache)
            if (b.x1 > lx0 && b.x0 < lx1 && b.y1 > ly0 && b.y0 < ly1) local.push_back(b);
        if (local.empty()) return true;

        const Rect4 sr{m.pr.x0 + kSolidShrink, m.pr.y0 + kSolidShrink, m.pr.x1 - kSolidShrink, m.pr.y1 - kSolidShrink};
        const Rect4 hr = hazardRect(m.pr, -kHazardMargin);

        static std::vector<float> cur, nxt;
        cur.assign(1, 0.0f);
        for (int t = 1; t <= H; ++t) {
            nxt.clear();
            const float ox = static_cast<float>(m.vx * (t - 1));
            for (const float y : cur) {
                for (int c = 0; c < 2; ++c) {
                    int dir;
                    if (t == 1) { if (c == 1) break; dir = holdFirst ? holdDir : -holdDir; }
                    else        { dir = (c == 0) ? 1 : -1; }
                    const float dy = dir > 0 ? static_cast<float>(m.su) : -static_cast<float>(m.sd);
                    float ny = 0.0f;
                    if (stepOnce(local, sr, hr, ox, y, static_cast<float>(m.vx), dy, true, ny) == 0) nxt.push_back(ny);
                }
            }
            if (nxt.empty()) return false;
            std::sort(nxt.begin(), nxt.end());
            nxt.erase(std::unique(nxt.begin(), nxt.end(), [](float a, float b) { return std::fabs(a - b) < 0.05f; }), nxt.end());
            if (nxt.size() > 600) {                            // keep the cost bounded
                size_t k = 0;
                for (size_t j = 0; j < nxt.size(); j += 2) nxt[k++] = nxt[j];
                nxt.resize(k);
            }
            cur.swap(nxt);
        }
        return true;
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
    inline bool enqueue(bool down, bool p1, uint64_t dueStep, bool cushion, uint64_t minGap = 0, bool toDesired = false) {
        const int i = sideIdx(p1);
        if (dueStep < g_lastDue[i]) dueStep = g_lastDue[i];
        bool spaced = false;
        if (minGap > 0) {
            const uint64_t earliest = std::max(g_lastDue[i], g_lastDelivered[i]) + minGap;
            if (dueStep < earliest) { dueStep = earliest; spaced = true; }
        }
        g_lastDue[i] = dueStep;
        g_queue.push_back(Event{dueStep, ++g_seq, down, p1, cushion, toDesired});
        return spaced;
    }

    // Wave delay is configured in frames; the queue counts physics ticks.
    inline uint64_t waveDelaySteps() {
        if (g_waveDelay <= 0) return 0;
        const long steps = std::lround(static_cast<double>(g_waveDelay) * g_stepsPerFrame);
        return static_cast<uint64_t>(std::max<long>(steps, g_waveDelay));
    }

    inline void recordOutcome(const Side& w);

    inline void deliver(GJBaseGameLayer* layer, const Event& ev, bool force = false) {
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
        if (ev.desired && !force) {
            // Wave: this only updates what the player WANTS. The shield, once per
            // tick, decides what the engine is actually told.
            PlayerObject* dp = ev.p1 ? layer->m_player1 : layer->m_player2;
            if (dp && dp->m_isDart) { g_waveDesired[i] = ev.down; return; }
        }
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
            deliver(layer, ev, force);
        }
        keep.insert(keep.end(), g_queue.begin(), g_queue.end());
        g_queue = std::move(keep);
        refreshLastDue();
    }

    inline void pump(GJBaseGameLayer* layer)               { drain(layer, false); }
    inline void flushSide(GJBaseGameLayer* layer, bool p1) { drain(layer, true, sideIdx(p1)); }
    inline void flushAll(GJBaseGameLayer* layer)           { drain(layer, true); }

    // Forget every queued input of one side (used for clicks the engine never saw).
    inline void dropSide(bool p1) {
        const int i = sideIdx(p1);
        g_queue.erase(std::remove_if(g_queue.begin(), g_queue.end(),
                          [i](const Event& e) { return sideIdx(e.p1) == i; }),
                      g_queue.end());
        refreshLastDue();
    }

    // A held-back press whose key is still physically down must not be lost
    // when we stop managing it: hand it to the engine now.
    inline void sendHeldBack(GJBaseGameLayer* layer) {
        auto& st = state();
        for (int i = 0; i < 2; ++i) {
            auto& w = st.sides[i];
            if (w.active && w.deferred && !w.releaseQueued) inject(layer, true, i == 0);
        }
    }

    // ---- Wave shield -------------------------------------------------------
    // Runs once per tick, after the player's inputs for this tick are known.
    // It follows what the player WANTS (their latest key state, with any early
    // click remembered, however early) and applies it on the first tick where
    // the wave can still survive. If wanting it this tick leaves no way out but
    // the opposite input does, the opposite input is used until the wanted
    // state is survivable again, then the saved click is applied.
    inline void waveShield(GJBaseGameLayer* layer) {
        for (int i = 0; i < 2; ++i) {
            const bool p1 = (i == 0);
            PlayerObject* p = p1 ? layer->m_player1 : layer->m_player2;

            if (!p || !p->m_isDart) {                          // not a wave (any more): hand the key back
                if (g_shieldOn[i]) {
                    g_shieldOn[i] = false;
                    g_overrideTicks[i] = 0;
                    if (g_engineHeld[i] != g_userHeld[i]) { g_engineHeld[i] = g_userHeld[i]; inject(layer, g_userHeld[i], p1); }
                }
                continue;
            }
            if (!g_shieldOn[i]) {                              // entering a wave: sync the engine to the key
                g_shieldOn[i] = true;
                g_overrideTicks[i] = 0;
                g_waveDesired[i] = g_userHeld[i];
                g_engineHeld[i]  = !g_userHeld[i];             // forces one injection below
            }

            const bool want = g_waveDesired[i];
            bool choose = want;
            if (kSurviveMode > 0) {
                WaveModel m;
                if (waveModel(p, i, m) && !waveViable(m, want, p->m_isUpsideDown)
                                       && waveViable(m, !want, p->m_isUpsideDown)) {
                    choose = !want;
                }
            }

            if (choose != want) {
                ++g_overrideTicks[i];
                if (g_overrideTicks[i] == 1) {
                    ++g_surviveWave;
                    static int saveLogs = 0;
                    if (saveLogs < 300) {
                        ++saveLogs;
                        log::info("JW: SAVED wave: your {} would leave no way out, holding the {} state until it is safe{}",
                            want ? "press" : "release", want ? "released" : "pressed",
                            kSurviveMode == 2 ? "" : " (shadow: not applied)");
                    }
                }
                if (kSurviveMode != 2) choose = want;
            } else if (g_overrideTicks[i] > 0) {
                static int appLogs = 0;
                if (appLogs < 300) {
                    ++appLogs;
                    log::info("JW: saved wave {} applied after {} ticks", want ? "press" : "release", g_overrideTicks[i]);
                }
                g_overrideTicks[i] = 0;
            }

            if (choose != g_engineHeld[i]) {
                g_engineHeld[i] = choose;
                inject(layer, choose, p1);
            }
        }
    }

    inline void beginCushion(Side& w, PlayerObject* p) {
        w = Side{};
        w.active         = true;
        w.clickTime      = Clock::now();   // diagnostics only
        w.pressSeq       = g_stepSeq;
        w.sentSeq        = g_stepSeq;      // overwritten later if the click is held back
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
            log::info("JW: outcomes after {}: ground/orb ack={} miss={} | early-air ack={} miss={} (ack after avg {:.1f} / max {} ticks, budget {} ticks, {} ticks saved in total) | predictor: {} checked, mean error {:.1f} ticks | coyote: {}/{} jumped",
                total, g_outGroundAck, g_outGroundMiss, g_outAirAck, g_outAirMiss,
                g_outAirAck > 0 ? static_cast<double>(g_airAckStepsSum) / g_outAirAck : 0.0,
                g_airAckStepsMax, w.maxSteps, g_savedTicks,
                g_predN, g_predN > 0 ? static_cast<double>(g_predErrAbs) / g_predN : 0.0,
                g_coyoteOk, g_coyoteTried);
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

            // Player gone or no longer in a supported mode: stop managing it.
            // A held-back press whose key is still down goes to the engine now;
            // a held-back tap that was already released is simply forgotten.
            if (!p || !isCubeLike(p)) {
                if (w.deferred) {
                    if (w.releaseQueued) dropSide(p1);
                    else if (p)          inject(layer, true, p1);
                }
                w = Side{};
                continue;
            }

            const int age = static_cast<int>(g_stepSeq - w.pressSeq);   // ticks since the player clicked
            w.heldSteps = age;

            auto snapshot = [&]() {
                w.prevGround     = p->m_isOnGround;
                w.prevUpsideDown = p->m_isUpsideDown;
                w.prevRings      = ringCount(p);
                w.prevUp         = upVel(p);
            };

            // ---- Held-back click: the engine has NOT seen it. Deliver it on the
            // first tick where it is legal (landed, or touching an orb).
            if (w.deferred) {
                if (p->m_isOnGround || ringCount(p) > 0) {
                    // Landed: ask the planner whether jumping this very tick survives.
                    if (p->m_isOnGround && ringCount(p) == 0 && !w.planned) {
                        w.planned = true;
                        int hit = 0;
                        const int d = planCubeDelay(p, i, kCubeMaxDelay, hit);
                        if (d > 0) {
                            ++g_surviveCube;
                            static int cubeLogs = 0;
                            if (cubeLogs < 300) {
                                ++cubeLogs;
                                log::info("JW: SURVIVE cube: jumping now hits something in {} ticks, waiting {} ticks clears it{}",
                                    hit, d, kSurviveMode == 2 ? "" : " (shadow: not applied)");
                            }
                            if (kSurviveMode == 2) w.sendAt = g_stepSeq + static_cast<uint64_t>(d);
                        }
                    }
                    if (w.sendAt > g_stepSeq) { snapshot(); continue; }   // still waiting for the surviving tick
                    snapshot();                       // judge the press against the state right before it
                    w.deferred = false;
                    w.sentSeq  = g_stepSeq;
                    if (w.predTicks >= 0 && p->m_isOnGround) {          // check the predictor against reality
                        ++g_predN;
                        g_predErrAbs += std::abs(age - w.predTicks);
                    }
                    static int sendLogs = 0;
                    if (sendLogs < 300) {
                        ++sendLogs;
                        log::info("JW: held-back click delivered to the engine {} ticks after the click ({}; predicted landing in {} ticks)",
                            age, p->m_isOnGround ? "landed" : "orb contact", w.predTicks);
                    }
                    inject(layer, true, p1);
                } else if (const int pred = predictLanding(g_track[i]);
                           age >= w.maxSteps && pred >= 0 && age + pred <= w.maxSteps + kPredictExtraSteps) {
                    // Past the window, but the predictor says landing is imminent: keep holding.
                    static int extLogs = 0;
                    if (age == w.maxSteps && extLogs < 100) {
                        ++extLogs;
                        log::info("JW: window over at {} ticks, predictor says landing in {} more: still holding", age, pred);
                    }
                    snapshot();
                    continue;
                } else if (age >= w.maxSteps) {
                    // Too early: no legal moment inside the window.
                    recordOutcome(w);
                    static int expLogs = 0;
                    if (expLogs < 300) {
                        ++expLogs;
                        log::info("JW: held-back click expired after {} ticks with no legal moment ({})",
                            age, w.releaseQueued ? "already released: dropped" : "key still down: forwarded to the engine");
                    }
                    if (w.releaseQueued) dropSide(p1);
                    else                 inject(layer, true, p1);
                    w = Side{};
                    continue;
                } else {
                    snapshot();                       // keep waiting, engine sees nothing
                    continue;
                }
            }

            const int live = static_cast<int>(g_stepSeq - w.sentSeq);   // ticks since the engine got the press

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
                            log::info("JW: SAVED early click #{}: jump fired {} ticks ({:.1f} ms at {:.0f} TPS) after the click",
                                g_savedClicks, w.ackSteps, w.ackSteps * 1000.0 / g_tps, g_tps);
                        }
                    }
                }
            };

            detect();   // catches a press that took inside handleButton / the previous streamed press

            const bool overBudget = age >= w.maxSteps && live >= kMinHoldSteps;
            const bool nearOrb    = ringCount(p) > 0;
            const bool extend     = !w.registered && overBudget && nearOrb && age < kHardCapSteps;

            // Continuous streaming: one more press on every tick until acknowledged.
            if (!w.registered && live >= 1 && (!overBudget || extend)) {
                inject(layer, true, p1);
                ++w.streamed;
                if (overBudget) ++w.extSteps;
                detect();   // the streamed press may itself have fired the jump
            }

            // Only now roll the snapshot forward, for the next step's comparison.
            snapshot();

            w.releaseOk = (w.registered && live >= kMinHoldSteps)
                       || (overBudget && !extend);

            // Player is still holding and we are done: nothing to release, stop tracking.
            if (!w.releaseQueued && w.releaseOk) {
                recordOutcome(w);
                static int doneLogs = 0;
                if (doneLogs < 400) {
                    ++doneLogs;
                    log::info("JW: press tracking ended while held (registered: {}, age {}/{} ticks, streamed {}, extended {})",
                        w.registered, age, w.maxSteps, w.streamed, w.extSteps);
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
    log::info("JW: build 18 loaded (wave shield + survival planner for cube, clicks held until the surviving tick, landing predictor, coyote late jump, macOS)");
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
        if (!st.injecting && button == jw::kButtonJump) jw::g_userHeld[isPlayer1 ? 0 : 1] = down;   // the key as the player holds it

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
            // The click is remembered as what the player WANTS; the wave shield applies
            // it on the first tick where the wave can survive it (immediately when safe).
            const bool spaced = jw::enqueue(down, isPlayer1, jw::g_stepSeq + jw::waveDelaySteps(), false, jw::kWaveMinGapSteps, true);
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

            // Airborne with nothing to grab: the click is HELD BACK. The engine
            // never sees it now; runBuffer delivers it on the first tick where
            // it is legal (landed, or touching an orb).
            const bool coyote   = jw::tryCoyote(p, isPlayer1);   // may mark the player grounded for this one press
            const bool airborne = !p->m_isOnGround && jw::ringCount(p) == 0;

            // A click on the ground can be too early as well: if jumping this very tick
            // is certain death but a later tick clearly clears it, hold it for that tick.
            int groundDelay = 0;
            if (!airborne && !coyote && p->m_isOnGround && jw::ringCount(p) == 0 && jw::kSurviveMode > 0) {
                int hit = 0;
                groundDelay = jw::planCubeDelay(p, jw::sideIdx(isPlayer1), jw::kCubeMaxDelay, hit);
                if (groundDelay > 0) {
                    ++jw::g_surviveCube;
                    static int groundLogs = 0;
                    if (groundLogs < 300) {
                        ++groundLogs;
                        log::info("JW: SURVIVE cube (ground click): jumping now hits something in {} ticks, waiting {} ticks clears it{}",
                            hit, groundDelay, jw::kSurviveMode == 2 ? "" : " (shadow: not applied)");
                    }
                }
            }
            const bool groundHold = groundDelay > 0 && jw::kSurviveMode == 2;

            jw::logDecision(airborne
                ? "CUSHIONED: early click held back, the engine sees nothing until it is legal"
                : groundHold ? "CUSHIONED: ground click held back until the surviving tick"
                             : "CUSHIONED: press forwarded at once, streamed until the engine acknowledges", p);
            jw::logReplaced(w);
            if (w.active && w.deferred) jw::dropSide(isPlayer1);   // the older held-back tap never reached the engine
            else                        jw::flushSide(self, isPlayer1);   // a stale queued release goes first
            jw::beginCushion(w, p);
            st.anyActive = true;
            if (airborne || groundHold) {
                w.deferred  = true;
                if (airborne) w.predTicks = jw::predictLanding(jw::g_track[jw::sideIdx(isPlayer1)]);
                if (groundHold) {
                    w.planned  = true;
                    w.sendAt   = jw::g_stepSeq + static_cast<uint64_t>(groundDelay);
                    w.maxSteps = std::max(w.maxSteps, groundDelay + 8);
                }
                return;                               // NOT passed to GJBaseGameLayer::handleButton
            }
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            if (coyote) jw::finishCoyote(p, isPlayer1);
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
        jw::trackAir(self);                  // motion state for this tick, before any input is processed
        jw::g_inTick = true;
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
        jw::g_inTick = false;

        if (!jw::g_stepHookSeen) {
            jw::g_stepHookSeen = true;
            log::info("JW: physics-step hook is running");
        }

        // Assist switched off (or level gone): never strand a queued input.
        if (!jw::assistActive()) {
            jw::g_shieldOn[0] = jw::g_shieldOn[1] = false;
            jw::sendHeldBack(self);
            jw::flushAll(self);
            jw::resetSides();
            return;
        }

        const auto t0 = jw::Clock::now();
        jw::runBuffer(self);
        jw::pump(self);
        jw::waveShield(self);
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
