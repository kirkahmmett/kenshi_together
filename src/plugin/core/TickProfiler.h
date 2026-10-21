// TickProfiler - where the main-loop hook spends its time, and what the wire
// carries, as one log line per window.
//
// Nothing in the plugin measured either. mainLoop_hook runs ~30 subsystem passes
// every engine tick (100-125 Hz) and the net thread streams the full owned
// snapshot every 50 ms, yet a session log could not say which pass was expensive
// or how many KB/s a busy town cost - so every performance claim was a guess.
// This accumulates per-section time (sum + worst single call) over a window and
// formats it as:
//
//   [perf] win=10000ms ticks=1043 (104/s) hook=1.84/9.12ms engine=6.31/22.40ms
//          top: pubOwned=0.52/3.10 applyTargets=0.41/2.20 ...
//
// where a/b is the per-TICK average and the worst single call, both in ms. The
// net thread formats its own "[net] stats" line from ENet's host counters.
//
// Pure inline C++03 with no Win32/logger dependency (the caller supplies the
// tick counts and the counter frequency), matching ChangeGate.h / EngineFaults.h,
// so prototest locks the accumulation and the formatting directly.

#ifndef KENSHICOOP_TICK_PROFILER_H
#define KENSHICOOP_TICK_PROFILER_H

#include <stdio.h>  // _snprintf
#include <stdlib.h> // getenv, atol

namespace coop {
namespace perf {

// One slot per timed pass of mainLoop_hook. HOOK and ENGINE are reported on
// their own; every other section competes for the "top" list.
enum Section {
    S_HOOK = 0,       // the whole hook, engine included
    S_ENGINE,         // g_mainLoop_orig - Kenshi's own tick
    S_PANEL,          // F2 panel drive
    S_WORLD_SWAP,     // coordinated-load edge + session reset
    S_NET_EVENTS,     // connect/leave/handshake drain
    S_HARNESS,        // setup scene, auto-bake, re-arm, scenario hold/start/tick
    S_PUB_INGEST,     // entity + inventory ingest, reliable events
    S_PUB_OWNED,      // squad + NPC capture into the entity snapshot
    S_PUB_WEAPONS,    // weapon-drop detect + ground-gear reconcile
    S_PUB_INV,        // owned inventory snapshots
    S_PUB_XFER,       // cross-owner transfer detect
    S_PUB_WORLD,      // ground items
    S_MEDICAL,        // vitals, treatments, join-dealt combat hits
    S_STATS,          // character stats
    S_MONEY,          // shared money pool
    S_SQUAD,          // recruits + squad moves
    S_SAMPLED,        // faction/doors/buildings/prod/research/deeds/fixtures
    S_STEALTH,        // detection maps
    S_SPEED,          // consensus game speed
    S_TIME,           // game clock
    S_SPAWN,          // runtime-spawn proxies
    S_SAVE_LOAD,      // coordinated save + load
    S_APPLY_TARGETS,  // drive the peer's bodies
    S_APPLY_WEAPONS,  // weapon drop/pickup relocation
    S_APPLY_XFER,     // transfer intents + verdicts
    S_APPLY_INV,      // inventory reconcile
    S_APPLY_WORLD,    // ground-item proxies + claims
    S_CENSUS,         // NPC census apply/publish + authority enforcement
    S_CAM_CELL,       // camera hints + cell claims
    S_COUNT
};

inline const char* sectionName(int s) {
    static const char* const k[S_COUNT] = {
        "hook", "engine", "panel", "worldSwap", "netEvents", "harness",
        "pubIngest", "pubOwned", "pubWeapons", "pubInv", "pubXfer", "pubWorld",
        "medical", "stats", "money", "squad", "sampled", "stealth", "speed",
        "time", "spawn", "saveLoad", "applyTargets", "applyWeapons", "applyXfer",
        "applyInv", "applyWorld", "census", "camCell"
    };
    return (s >= 0 && s < S_COUNT) ? k[s] : "?";
}

// Window length from KENSHICOOP_PERF_MS: unset/empty = 10 s, 0 = off. Read by
// both the main thread and the net thread so their lines share one window.
inline unsigned long windowMsFromEnv() {
    const char* e = getenv("KENSHICOOP_PERF_MS");
    if (!e || !e[0]) return 10000ul;
    long v = atol(e);
    return v > 0 ? (unsigned long)v : 0ul;
}

struct Acc {
    unsigned __int64 sum; // counter ticks summed over the window
    unsigned __int64 max; // worst single call in the window
    unsigned int     n;   // calls in the window
};

class TickProfiler {
public:
    TickProfiler() : windowStartMs_(0), ticks_(0) { reset(); }

    void reset() {
        for (int i = 0; i < S_COUNT; ++i) { acc_[i].sum = 0; acc_[i].max = 0; acc_[i].n = 0; }
        ticks_ = 0;
    }

    void add(int s, unsigned __int64 dt) {
        if (s < 0 || s >= S_COUNT) return;
        acc_[s].sum += dt;
        if (dt > acc_[s].max) acc_[s].max = dt;
        ++acc_[s].n;
    }

    void endTick() { ++ticks_; }

    // True once per window. The first call only opens the window. On true the
    // caller formats, logs, and calls reset(); the window restarts at nowMs.
    // Unsigned subtraction tolerates the millisecond clock wrapping.
    bool windowDue(unsigned long nowMs, unsigned long windowMs) {
        if (windowMs == 0) return false;
        if (windowStartMs_ == 0) { windowStartMs_ = nowMs ? nowMs : 1; return false; }
        if (nowMs - windowStartMs_ < windowMs) return false;
        windowStartMs_ = nowMs ? nowMs : 1;
        return true;
    }

    const Acc&   acc(int s) const { return acc_[s]; }
    unsigned int ticks() const    { return ticks_; }

    // Per-tick average and worst single call of a section, in ms.
    double avgMs(int s, unsigned __int64 freq) const {
        if (ticks_ == 0 || freq == 0) return 0.0;
        return (double)acc_[s].sum * 1000.0 / (double)freq / (double)ticks_;
    }
    double maxMs(int s, unsigned __int64 freq) const {
        if (freq == 0) return 0.0;
        return (double)acc_[s].max * 1000.0 / (double)freq;
    }

    // Formats the window line into out (always NUL-terminated). topN sections
    // other than HOOK/ENGINE are listed by window sum, largest first; sections
    // that never ran are left out. Returns the formatted length.
    int format(char* out, unsigned int cap, unsigned __int64 freq,
               unsigned long windowMs, int topN) const {
        if (!out || cap == 0) return 0;
        out[0] = '\0';
        int len = _snprintf(out, cap - 1,
            "[perf] win=%lums ticks=%u (%lu/s) hook=%.2f/%.2fms engine=%.2f/%.2fms top:",
            windowMs, ticks_,
            windowMs ? (unsigned long)((unsigned __int64)ticks_ * 1000ull / windowMs) : 0ul,
            avgMs(S_HOOK, freq), maxMs(S_HOOK, freq),
            avgMs(S_ENGINE, freq), maxMs(S_ENGINE, freq));
        if (len < 0 || (unsigned int)len >= cap - 1) { out[cap - 1] = '\0'; return (int)cap - 1; }

        bool used[S_COUNT];
        for (int i = 0; i < S_COUNT; ++i) used[i] = false;
        for (int k = 0; k < topN; ++k) {
            int best = -1;
            for (int i = 0; i < S_COUNT; ++i) {
                if (i == S_HOOK || i == S_ENGINE || used[i] || acc_[i].n == 0) continue;
                if (best < 0 || acc_[i].sum > acc_[best].sum) best = i;
            }
            if (best < 0) break;
            used[best] = true;
            int w = _snprintf(out + len, cap - 1 - len, " %s=%.2f/%.2f",
                              sectionName(best), avgMs(best, freq), maxMs(best, freq));
            if (w < 0 || (unsigned int)(len + w) >= cap - 1) { out[cap - 1] = '\0'; return (int)cap - 1; }
            len += w;
        }
        out[len] = '\0';
        return len;
    }

private:
    Acc           acc_[S_COUNT];
    unsigned long windowStartMs_;
    unsigned int  ticks_;
};

// The net thread's window line from ENet's host counters (bytes include ENet's
// own protocol overhead - it is what actually crosses the wire). rttMaxMs and
// lossPctMax are the worst connected peer. netLog adds the "[net] " prefix, so
// the line reads "[net] stats win=...".
inline int formatNetStats(char* out, unsigned int cap, unsigned long windowMs,
                          unsigned long sentBytes, unsigned long recvBytes,
                          unsigned long sentPkts, unsigned long recvPkts,
                          unsigned int peers, unsigned long rttMaxMs,
                          double lossPctMax) {
    if (!out || cap == 0) return 0;
    double secs = windowMs ? (double)windowMs / 1000.0 : 1.0;
    int len = _snprintf(out, cap - 1,
        "stats win=%lums peers=%u up=%.1fKB/s down=%.1fKB/s pkts up=%.0f/s down=%.0f/s "
        "rttMax=%lums lossMax=%.1f%%",
        windowMs, peers, (double)sentBytes / 1024.0 / secs, (double)recvBytes / 1024.0 / secs,
        (double)sentPkts / secs, (double)recvPkts / secs, rttMaxMs, lossPctMax);
    if (len < 0 || (unsigned int)len >= cap - 1) { out[cap - 1] = '\0'; return (int)cap - 1; }
    return len;
}

} // namespace perf
} // namespace coop

#endif // KENSHICOOP_TICK_PROFILER_H
