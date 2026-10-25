// Plugin - RE_Kenshi / KenshiLib plugin entry point (clean rebuild).
//
// Responsibilities (kept deliberately thin - everything substantive lives in the
// core/, net/, game/, sync/ modules):
//   * startPlugin(): read config, open the log, install the main-loop + title
//     hooks, start networking.
//   * mainLoop_hook(): the single main-thread safe point. Detects "gameplay
//     live", drains net events, drives per-stage sync, and (in test mode)
//     self-exits after the configured duration.
//   * titleUpdate_hook(): auto-loads the configured save once the menu settles.
//
// Stage 0 scope: modular skeleton + ENet handshake (version-checked) + lifecycle.
// Sync of entities is added from Stage 1 onward.

#define BOOST_ALL_NO_LIB 1
#define BOOST_ERROR_CODE_HEADER_ONLY 1
#define BOOST_SYSTEM_NO_DEPRECATED 1

#include <Debug.h>                  // DebugLog / ErrorLog
#include <core/Functions.h>         // KenshiLib::AddHook / GetRealAddress / SUCCESS
#include <kenshi/GameWorld.h>       // GameWorld (main-loop hook signature)
#include <kenshi/gui/TitleScreen.h> // TitleScreen::_NV_update (auto-load trigger)

#include <windows.h>
#include <shlobj.h>                  // SHGetFolderPathA (problem report -> Desktop)
#pragma comment(lib, "shell32.lib")
#include <cstdio>
#include <string>
#include <deque>

#include "CoopLog.h"
#include "core/Config.h"
#include "core/CrashDump.h"
#include "core/OwnRanks.h"
#include "core/Inbound.h"
#include "core/TickProfiler.h"
#include "net/NetLink.h"
#include "net/SteamP2P.h"
#include "net/SteamInvite.h"
#include <algorithm>
#include "game/Engine.h"
#include "game/EngineUi.h"       // Phase 5a: F2 co-op panel + status overlay
#include "game/EngineScenario.h" // Phase 5a: auto-bake scene builders
#include "sync/Replicator.h"
#include "sync/SaveXfer.h"
#ifdef KENSHICOOP_HARNESS
#include "test/Scenario.h" // scenario runner: Harness/Debug builds only (Phase 1)
#endif

namespace {

// PluginHost (Phase 3): composition root owning the four long-lived subsystem
// singletons as one cohesive object instead of four loose globals. The g_cfg /
// g_net / g_inbound / g_repl names remain as references INTO the host so every
// existing call site is unchanged; later phases thread the host explicitly and
// fold the session-state globals in behind a SessionController.
struct PluginHost {
    coop::Config     cfg;
    coop::NetLink    net;
    coop::Inbound    inbound;
    coop::Replicator repl;
};
PluginHost g_host;

coop::Config&     g_cfg     = g_host.cfg;
coop::NetLink&    g_net     = g_host.net;
coop::Inbound&    g_inbound = g_host.inbound;
coop::Replicator& g_repl    = g_host.repl;
coop::u32        g_tick = 0;

// Last GameWorld seen by the main-loop hook. The F2-panel UI callbacks
// (coopUiConnect/coopUiDisconnect) run without a GameWorld argument, but the
// world is live when the user hits Connect/Disconnect, so we despawn our
// minted proxies (NPC + world-item, Phase 3) through this cached pointer to
// avoid leaking duplicate bodies into the save. Only touched on the main thread.
GameWorld*       g_lastGw = 0;

// Cross-owner trade veto owner classifier (engine InvOwnerClassFn): forwards a
// save-stable owner hand to the Replicator's squad-ownership sets. Free function
// so the engine layer (which must not know about the Replicator) can call it.
static int coopInvOwnerClass(const unsigned int h[5]) {
    return g_repl.ownerClassForHand(h);
}

// assault_mint's victim query (ScenarioContext::pickMintedProxy). Same shape and
// same reason as the classifier above: the scenario layer must not know about the
// Replicator, and only the Replicator knows which local bodies are mints.
static bool coopScenarioPickMintedProxy(const unsigned int refHand[5],
                                        unsigned int outLocal[5],
                                        unsigned int outCanon[5],
                                        float* outDist) {
    if (!g_lastGw) return false;
    return g_repl.pickMintedProxyNear(g_lastGw, refHand, outLocal, outCanon, outDist);
}

// SessionController (Phase 3): the mutable per-session lifecycle state that used
// to live as ~18 loose file globals - peer presence, the gameplay-start edge,
// the title auto-load gate, and the coordinated save/load (protocol 31/32)
// coordination counters + latches. Grouping them behind one object makes the
// session boundary explicit and gives the reset helpers (sessionResetForUi /
// sessionResetForWorldReload) a single, named place to clear. As with
// PluginHost, the historical g_* names remain as references INTO the controller
// so every call site is unchanged (zero churn). C++03: defaults are set in the
// constructor (no in-class member initializers).
struct SessionController {
    bool         gameStarted;      // gameplay has been live at least once
    DWORD        gameStartTick;    // GetTickCount at the gameplay-start edge
    bool         autoLoadDone;     // title auto-load fired (settle gate)
    DWORD        titleFirstTick;   // first title tick (settle gate base)
    bool         peerPresent;      // a peer is connected right now
    // Coordinated save (protocol 31).
    std::string  savePending;      // host: save name awaiting quiescence
    coop::u32    saveReqId;        // join: monotonic PKT_SAVE_REQ counter
    // Push-save-on-connect bootstrap (host): when a peer connects the host bakes
    // a fresh save of its live world and, once the folder quiesces, sends the
    // join a LOAD_GO for it (not a blind stream) - the join loads it if it has an
    // identical copy, else NACKs and driveLoadSync's fallback transfer streams
    // the folder first. Lets a joiner enter the host's world with no pre-shared
    // save.
    bool         bootstrapArmed;   // host: a connect-triggered save is baking
    std::string  bootstrapName;    // host: that save's name (== savePending)
    // World-swap edge detection (protocol 32): once gameplay has started,
    // gameplayLive dropping means the engine is swapping worlds (a load); live
    // again = the reload edge (session-reset point). Sub-second dips are FLICKER
    // and do NOT count - a real load screen lasts seconds and a spurious reset
    // would wipe live session state mid-game.
    DWORD        swapStartTick;    // != 0: gameplay non-live, swap running
    coop::u32    swapHookTicks;    // mainLoop ticks observed during the swap
    // Coordinated load (protocol 32).
    bool         loadSuppressOn;   // join: current suppression lever state
    coop::u32    loadIdOut;        // host: monotonic LOAD_GO id
    coop::u32    loadIdSeen;       // join: newest GO loadId handled
    coop::u32    loadReqId;        // join: monotonic PKT_LOAD_REQ counter
    std::string  loadXferPending;  // host: save awaiting post-reload transfer (NACK)
    std::string  loadAfterCommit;  // join: save to load once its transfer commits
    coop::u32    loadCommitBase;   // join: savexfer::commitSeq() at NACK time
    // Deferred-signal backstop: SaveManager::load only SETS the LOADGAME signal;
    // it can sit unconsumed mid-session. Armed on every coordinated load issue;
    // if the swap hasn't started after the grace window, pump execute() once.
    DWORD        loadPumpArmTick;  // != 0: backstop armed at this tick

    SessionController()
      : gameStarted(false), gameStartTick(0), autoLoadDone(false),
        titleFirstTick(0), peerPresent(false),
        saveReqId(0), bootstrapArmed(false),
        swapStartTick(0), swapHookTicks(0),
        loadSuppressOn(false), loadIdOut(0), loadIdSeen(0), loadReqId(0),
        loadCommitBase(0), loadPumpArmTick(0) {}
};
SessionController g_session;

bool&        g_gameStarted    = g_session.gameStarted;
DWORD&       g_gameStartTick   = g_session.gameStartTick;
bool&        g_autoLoadDone    = g_session.autoLoadDone;
DWORD&       g_titleFirstTick  = g_session.titleFirstTick;
bool&        g_peerPresent     = g_session.peerPresent;
// Who is connected (protocol 57): the host tracks each join's id, a join holds
// the host (0). g_peerPresent stays the "anyone there" summary.
std::set<coop::u32> g_peers;
std::string& g_savePending     = g_session.savePending;
coop::u32&   g_saveReqId       = g_session.saveReqId;
bool&        g_bootstrapArmed  = g_session.bootstrapArmed;
std::string& g_bootstrapName   = g_session.bootstrapName;
DWORD&       g_swapStartTick   = g_session.swapStartTick;
coop::u32&   g_swapHookTicks   = g_session.swapHookTicks;
bool&        g_loadSuppressOn  = g_session.loadSuppressOn;
coop::u32&   g_loadIdOut       = g_session.loadIdOut;
coop::u32&   g_loadIdSeen      = g_session.loadIdSeen;
coop::u32&   g_loadReqId       = g_session.loadReqId;
std::string& g_loadXferPending = g_session.loadXferPending;
std::string& g_loadAfterCommit = g_session.loadAfterCommit;
coop::u32&   g_loadCommitBase  = g_session.loadCommitBase;
DWORD&       g_loadPumpArmTick  = g_session.loadPumpArmTick;

// Scenario harness state. Harness/Debug builds only - the shipped Release DLL
// excludes test/Scenario*.cpp and does not define KENSHICOOP_HARNESS (Phase 1).
#ifdef KENSHICOOP_HARNESS
coop::Scenario* g_scenario        = 0;
bool            g_scenarioStarted = false;
DWORD           g_scenarioStartTick = 0;
unsigned int    g_scenarioTick    = 0;
DWORD           g_scenarioDoneTick = 0; // !=0 once RESULT logged; begins capture hold
const DWORD     SCENARIO_HOLD_MS  = 4000; // hold synced state on screen for capture
#endif

// "Multiplayer+ (Wanderer x2)" start (dist/mods/KenshiCoop/KenshiCoop.mod). The FCS record
// sets the 500k shared wallet, but a Kenshi Character record only carries the
// GROUPED stat fields ("combat stats", "ranged stats", "stealth stats", "strength",
// "unarmed stats") and randomises them - there is no way to say "50 in everything"
// in data. So the start marks itself with the template below and the plugin writes
// the stat line at gameplay start. Keep the id in sync with Sid(4) in
// tools/MultiplayerStartGen/Program.cs.
const char* const WPX2_MARKER_SID = "4-KenshiCoop-MultiplayerStart.mod";
const float       WPX2_STAT_LEVEL = 50.0f;

// Test-scene setup (host-only): one-shot world spawn the user then saves.
bool            g_setupDone     = false;
const DWORD     SETUP_DELAY_MS  = 4000; // let the world settle before spawning
DWORD           g_lastCraftRearmTick = 0; // throttle host craft re-arm
DWORD           g_bakeSaveTick  = 0;     // != 0: auto-bake save armed at this tick
const DWORD     CRAFT_REARM_MS  = 3000; // re-issue the work goal at most this often

// Session-state timing constants (the mutable coordination state itself now
// lives in SessionController / g_session above).
const DWORD  SWAP_MIN_MS        = 400;   // shorter world-swap dips are flicker, not a reload
const DWORD  LOAD_PUMP_GRACE_MS = 2000;  // deferred-LOADGAME backstop grace window

// Original function pointers, filled by KenshiLib::AddHook.
void (*g_mainLoop_orig)(GameWorld*, float) = 0;
void (*g_titleUpdate_orig)(TitleScreen*)   = 0;

// In-game co-op panel (F2) connect/disconnect handlers. Defined after
// startNetworking() (which coopUiConnect reuses); forward-declared here so
// mainLoop_hook can hand their addresses to coopPanelTick.
void startNetworking();
void coopUiConnect(bool isHost, bool useSteam, unsigned long long peerId);
void coopUiDisconnect();

// Log to BOTH our dedicated per-line-flushed file (what the test runner reads)
// and the engine's kenshi.log (handy when attached live).
void coopLog(const char* msg) { coop::logLine(msg);    DebugLog(msg); }
void coopErr(const char* msg) { coop::logErrLine(msg); ErrorLog(msg); }

// Per-section main-loop cost (core/TickProfiler.h): one "[perf]" line per
// KENSHICOOP_PERF_MS window (default 10 s, 0 = off). KC_PERF times one statement
// with the performance counter; it holds no C++ objects, so it is safe inside
// the SEH-guarded tick functions too.
coop::perf::TickProfiler g_perf;
unsigned long            g_perfWindowMs = 0;   // set in startPlugin
unsigned __int64         g_perfFreq     = 0;

inline unsigned __int64 perfNow() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (unsigned __int64)c.QuadPart;
}

#define KC_PERF(sec, stmt) do { \
    if (g_perfWindowMs) { \
        unsigned __int64 kcPerfT0_ = perfNow(); \
        stmt; \
        g_perf.add(coop::perf::sec, perfNow() - kcPerfT0_); \
    } else { stmt; } \
} while (0)

// ---- Host settings for the join (protocol 56, issue #42) ----------------------
// Host: the live choice (seeded from config, switched in the F2 panel), sent on
// connect, on change and every PERM_RESEND_MS. Join: applies the newest set it
// received and tells the player for a few seconds when an action was refused.
bool          g_permBuild = true, g_permContainers = true, g_permBuy = true;
bool          g_permTake = false; // v58: players may take from each other (host switch)
coop::u32     g_permSeq = 0;
bool          g_permDirty = true;
DWORD         g_permSentTick = 0;
coop::u32     g_permSeenSeq = 0;      // join: newest host seq applied
unsigned int  g_permRefusalSeen = 0;  // join: refusals already announced
DWORD         g_permNoticeTick = 0;
int           g_permNoticeKind = 0;
const DWORD   PERM_RESEND_MS = 10000;

// Player names on the squad tabs (protocol 57): id -> Steam name. The host
// learns each join's from its HELLO and sends the whole roster on connect and
// every ROSTER_RESEND_MS; every client names tab <id> after player <id>.
std::map<coop::u32, std::string> g_names;
bool          g_namesDirty = false;
DWORD         g_namesSentTick = 0;
DWORD         g_namesApplyTick = 0;
const DWORD   ROSTER_RESEND_MS = 10000;

// "Always host my games" (panel switch), kept in KenshiCoop_prefs.txt next to
// the logs. When on, a loaded game is opened to friends once per load.
bool          g_autoHost = false;
bool          g_autoHostDone = false; // already hosted (or tried) for this load
bool          g_prefsLoaded = false;
bool          g_firstWorldLoaded = true; // set at the first gameplay start
const char*   PREFS_FILE = "KenshiCoop_prefs.txt";
// New game settings (lobby panel): what a NEW game starts with. Index 0 keeps
// whatever the chosen start gives (e.g. Multiplayer+ = 500,000 cats, stats 50).
// Applied once, at the first gameplay start of a game that was NOT loaded from a
// save, on the host (or solo) side - a join receives the host's world as is.
const int   NG_MONEY[]      = { -1, 0, 1000, 10000, 50000, 100000, 500000 };
const char* NG_MONEY_TEXT[] = { "Start default", "0 cats", "1,000 cats", "10,000 cats",
                                "50,000 cats", "100,000 cats", "500,000 cats" };
const int   NG_STATS[]      = { -1, 10, 25, 50, 75, 100 };
const char* NG_STATS_TEXT[] = { "Start default", "10 in every stat", "25 in every stat",
                                "50 in every stat", "75 in every stat", "100 in every stat" };
const int   NG_MONEY_N = sizeof(NG_MONEY) / sizeof(NG_MONEY[0]);
const int   NG_STATS_N = sizeof(NG_STATS) / sizeof(NG_STATS[0]);
int         g_ngMoney = 0;
int         g_ngStats = 0;

void loadPrefs() {
    if (g_prefsLoaded) return;
    g_prefsLoaded = true;
    FILE* f = fopen(PREFS_FILE, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v = 0;
        if (strncmp(line, "autoHost=1", 10) == 0) g_autoHost = true;
        else if (sscanf(line, "newGameMoney=%d", &v) == 1 && v >= 0 && v < NG_MONEY_N) g_ngMoney = v;
        else if (sscanf(line, "newGameStats=%d", &v) == 1 && v >= 0 && v < NG_STATS_N) g_ngStats = v;
    }
    fclose(f);
}
void savePrefs() {
    FILE* f = fopen(PREFS_FILE, "w");
    if (!f) return;
    fprintf(f, "autoHost=%d\n", g_autoHost ? 1 : 0);
    fprintf(f, "newGameMoney=%d\n", g_ngMoney);
    fprintf(f, "newGameStats=%d\n", g_ngStats);
    fclose(f);
}
void onNewGameCycle(int which) {
    loadPrefs();
    if (which == 0) g_ngMoney = (g_ngMoney + 1) % NG_MONEY_N;
    else            g_ngStats = (g_ngStats + 1) % NG_STATS_N;
    savePrefs();
    char b[96];
    _snprintf(b, sizeof(b) - 1, "[coop-ui] new game: money=%s stats=%s",
              NG_MONEY_TEXT[g_ngMoney], NG_STATS_TEXT[g_ngStats]);
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
}
void onAutoHostToggle() {
    g_autoHost = !g_autoHost;
    g_autoHostDone = g_net.isRunning(); // switching on mid-game hosts now if offline
    savePrefs();
    coopLog(g_autoHost ? "[coop-ui] always host: ON" : "[coop-ui] always host: OFF");
}
const DWORD   NAMES_APPLY_MS   = 2000;
const DWORD   PERM_NOTICE_MS = 5000;

void onPermToggle(int which) {
    bool* f = (which == 0) ? &g_permBuild : (which == 1) ? &g_permContainers :
              (which == 2) ? &g_permBuy : &g_permTake;
    *f = !*f;
    g_permDirty = true;
    char b[112];
    _snprintf(b, sizeof(b) - 1, "[perm] host set join %s = %s",
              which == 0 ? "build" : which == 1 ? "containers" : which == 2 ? "buy" : "take",
              *f ? "ON" : "OFF");
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
}

// ---- Character editor (protocol 60) ------------------------------------------
// A player who joins gets a squad the host made for them - a random character,
// or a free start character. The host OFFERS that player Kenshi's own editor once
// they have loaded; when they close it, their look (appearance file, race, name)
// goes to the host, which writes it into its copy, saves and reloads everyone.
// Anyone can reopen it from the panel ("Edit my character").
std::set<std::string> g_lookOfferNames; // host: players to offer the editor once loaded
bool          g_lookWanted   = false;   // open the editor when the world is settled
DWORD         g_lookWantTick = 0;
bool          g_lookEditing  = false;   // our editor is open (or opening)
bool          g_lookSeenOpen = false;
DWORD         g_lookOpenTick = 0;
unsigned int  g_lookHand[5]  = { 0, 0, 0, 0, 0 };
DWORD         g_lastWorldResetTick = 0;
DWORD         g_lookAwaitReload = 0;      // != 0: closed for a reload at this tick; reopen only after it
const char*   LOOK_OUT_FILE  = "KenshiCoop_look_out.tmp";
const char*   LOOK_IN_FILE   = "KenshiCoop_look_in.tmp";

void sendLookOffer(coop::u32 playerId) {
    coop::CharLookHeader h;
    memset(&h, 0, sizeof(h));
    h.type    = (coop::u8)coop::PKT_CHAR_LOOK;
    h.kind    = coop::CHAR_LOOK_OFFER;
    h.ownerId = g_net.localId();
    h.target  = playerId;
    g_net.queueCharLook(h, std::string(), std::string(), std::vector<coop::u8>());
    char b[96];
    _snprintf(b, sizeof(b) - 1, "[look] offered the character editor to player %u", (unsigned)playerId);
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
}

// Host: a player finished loading - offer the editor if their squad is new.
void offerLookIfPending(coop::u32 playerId) {
    std::map<coop::u32, std::string>::const_iterator nm = g_names.find(playerId);
    if (nm == g_names.end()) return;
    std::set<std::string>::iterator it = g_lookOfferNames.find(nm->second);
    if (it == g_lookOfferNames.end()) return;
    g_lookOfferNames.erase(it);
    sendLookOffer(playerId);
}

// Kenshi does not run a load while its character editor is open, so a GO that
// arrives mid-edit would leave this client on the old world (and the host waiting
// for it). Close the editor first - quietly, nothing is sent - and reopen it once
// the new world has settled.
bool closeEditorForLoad() {
    if (!g_lookEditing) return false;
    g_lookEditing = false;
    g_lookWanted = true;
    g_lookWantTick = GetTickCount();
    g_lookAwaitReload = GetTickCount();
    if (g_lastGw) coop::engine::closeCharacterEditor(g_lastGw);
    coopLog("[look] world reload: editor closed, it reopens after loading");
    return true;
}

void onEditLookRequest() {
    if (g_lookEditing) return;
    g_lookWanted = true;
    g_lookWantTick = GetTickCount() - 3000; // the panel press is "now"
    coopLog("[look] editor requested from the panel");
}

// ---- Ready barrier (protocol 59) -------------------------------------------
// When the host sends a LOAD_GO (a new player joining, or everyone reloading)
// it holds the game paused for everyone until each connected join reports it
// has loaded that world and is back in gameplay - a player still on the loading
// screen no longer falls behind a running world. READY_TIMEOUT_MS bounds it.
std::set<coop::u32> g_readyWait;
// Players still loading when the wait timed out. Their client is on the OLD
// world, so its rows name characters this world does not have - read as new
// bodies they became ghost copies at the origin (a "Nameless" tab). Their rows
// are ignored until they report the current GO loaded.
std::set<coop::u32> g_lateLoaders;
coop::u32           g_readyLoadId = 0;
DWORD               g_readyStart  = 0;
const DWORD         READY_TIMEOUT_MS = 90000;

void startReadyWait(coop::u32 loadId) {
    g_readyWait.clear();
    for (std::set<coop::u32>::const_iterator it = g_peers.begin(); it != g_peers.end(); ++it)
        if (*it != 0) g_readyWait.insert(*it);
    if (g_readyWait.empty()) return;
    g_readyLoadId = loadId;
    g_readyStart  = GetTickCount();
    char b[112];
    _snprintf(b, sizeof(b) - 1, "[ready] holding the game until %u player(s) load (GO id=%u)",
              (unsigned)g_readyWait.size(), (unsigned)loadId);
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
}

// Join: tell the host we are back in gameplay after its LOAD_GO.
void sendLoadReady() {
    if (g_cfg.isHost || !g_peerPresent || g_loadIdSeen == 0) return;
    coop::LoadReadyPacket r;
    memset(&r, 0, sizeof(r));
    r.type    = (coop::u8)coop::PKT_LOAD_READY;
    r.ownerId = g_net.localId();
    r.loadId  = g_loadIdSeen;
    g_net.queueLoadReady(r);
    char b[80];
    _snprintf(b, sizeof(b) - 1, "[ready] loaded GO id=%u - told the host", (unsigned)g_loadIdSeen);
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
}

void tickReady() {
    std::deque<coop::InboundLoadReady> got;
    g_inbound.drainLoadReadys(got);
    if (!g_cfg.isHost) return;
    for (std::deque<coop::InboundLoadReady>::iterator it = got.begin(); it != got.end(); ++it) {
        if (it->pkt.loadId == g_readyLoadId && g_lateLoaders.erase(it->ownerId)) {
            g_repl.playerJoined(it->ownerId); // their rows are about this world again
            char lb[96];
            _snprintf(lb, sizeof(lb) - 1, "[ready] player %u loaded late - listening to them again",
                      (unsigned)it->ownerId);
            lb[sizeof(lb) - 1] = '\0'; coopLog(lb);
            offerLookIfPending(it->ownerId);
            continue;
        }
        if (it->pkt.loadId != g_readyLoadId || !g_readyWait.erase(it->ownerId)) continue;
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[ready] player %u loaded (%u still loading)",
                  (unsigned)it->ownerId, (unsigned)g_readyWait.size());
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
        offerLookIfPending(it->ownerId);
    }
    // Someone who left is not waited for.
    for (std::set<coop::u32>::iterator it = g_readyWait.begin(); it != g_readyWait.end();) {
        if (!g_peers.count(*it)) g_readyWait.erase(it++); else ++it;
    }
    if (!g_readyWait.empty() && GetTickCount() - g_readyStart > READY_TIMEOUT_MS) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[ready] TIMEOUT - %u player(s) still loading; resuming",
                  (unsigned)g_readyWait.size());
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
        for (std::set<coop::u32>::iterator lt = g_readyWait.begin(); lt != g_readyWait.end(); ++lt) {
            g_lateLoaders.insert(*lt);
            g_repl.forgetPlayer(g_lastGw, *lt, false); // mute their old-world rows
        }
        g_readyWait.clear();
    }
    // A late loader who left is not waited for either.
    for (std::set<coop::u32>::iterator lt = g_lateLoaders.begin(); lt != g_lateLoaders.end();) {
        if (!g_peers.count(*lt)) g_lateLoaders.erase(lt++); else ++lt;
    }
    static bool s_held = false;
    bool hold = !g_readyWait.empty();
    if (s_held && !hold) {
        char b[80];
        _snprintf(b, sizeof(b) - 1, "[ready] everyone loaded after %lu ms - resuming",
                  (unsigned long)(GetTickCount() - g_readyStart));
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
    s_held = hold;
    g_repl.setSpeedHold(hold);
}

// ---- Mod list check (v59) ---------------------------------------------------
// FNV-1a over data\mods.cfg's non-empty lines, lower-cased, in load order. Only
// a warning: same-name mods of different versions still hash the same.
void readModList(coop::u32* hash, coop::u16* count) {
    coop::u32 h = 2166136261u;
    coop::u16 n = 0;
    FILE* f = fopen("data\\mods.cfg", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            size_t k = strlen(line);
            while (k > 0 && (line[k - 1] == '\n' || line[k - 1] == '\r' || line[k - 1] == ' ')) line[--k] = '\0';
            if (k == 0) continue;
            for (size_t i = 0; i < k; ++i) {
                h ^= (coop::u32)(unsigned char)tolower((unsigned char)line[i]);
                h *= 16777619u;
            }
            h ^= 0x0Au; h *= 16777619u; // line separator
            ++n;
        }
        fclose(f);
    }
    *hash = h; *count = n;
}

std::map<coop::u32, std::string> g_modWarn; // player id -> warning text
DWORD g_modWarnTick = 0;
const DWORD MODWARN_BANNER_MS = 20000;

void tickModCheck() {
    std::deque<coop::InboundModCheck> got;
    g_inbound.drainModMismatches(got);
    for (std::deque<coop::InboundModCheck>::iterator it = got.begin(); it != got.end(); ++it) {
        char b[160];
        if (it->playerId == 0)
            _snprintf(b, sizeof(b) - 1, "Warning: your mods differ from the host's (%u vs %u) - expect desync",
                      (unsigned)it->ourCount, (unsigned)it->theirCount);
        else
            _snprintf(b, sizeof(b) - 1, "Warning: player %u has a different mod list (%u vs your %u)",
                      (unsigned)it->playerId, (unsigned)it->theirCount, (unsigned)it->ourCount);
        b[sizeof(b) - 1] = '\0';
        g_modWarn[it->playerId] = b;
        g_modWarnTick = GetTickCount();
        coopLog((std::string("[mods] ") + b).c_str());
    }
    // Forget players who left.
    for (std::map<coop::u32, std::string>::iterator it = g_modWarn.begin(); it != g_modWarn.end();) {
        if (it->first != 0 && !g_peers.count(it->first)) g_modWarn.erase(it++); else ++it;
    }
    if (!g_peerPresent) g_modWarn.clear();
}

// What the joins are told (the same rule the host applies to itself).
bool allowTakeForJoins() {
    char e[4];
    DWORD n2 = GetEnvironmentVariableA("KENSHICOOP_PLAYERS_TAKE", e, sizeof(e));
    if (n2 > 0 && n2 < sizeof(e)) return e[0] != '0';
    return g_permTake || !(g_cfg.scenario.empty() && g_cfg.testSeconds == 0);
}

void tickPermissions() {
    // Refusals on ANY client (the take rule applies to the host too).
    unsigned int rs0 = coop::engine::permRefusalSeq();
    if (rs0 != g_permRefusalSeen) {
        g_permRefusalSeen = rs0;
        g_permNoticeKind = coop::engine::permRefusalKind();
        g_permNoticeTick = GetTickCount();
    }
    if (g_cfg.isHost) {
        // The host follows its own take rule, while anyone is connected.
        coop::engine::setTakeFromPlayers(allowTakeForJoins() || !g_peerPresent);
        DWORD now = GetTickCount();
        if (g_peerPresent && g_net.isRunning() &&
            (g_permDirty || now - g_permSentTick >= PERM_RESEND_MS)) {
            coop::PermsPacket p;
            p.type = (coop::u8)coop::PKT_PERMS;
            p.seq = ++g_permSeq;
            p.joinBuild = g_permBuild ? 1 : 0;
            p.joinContainers = g_permContainers ? 1 : 0;
            p.joinBuy = g_permBuy ? 1 : 0;
            p.takeFromPlayers = allowTakeForJoins() ? 1 : 0;
            g_net.queuePerms(p);
            g_permDirty = false;
            g_permSentTick = now;
        }
        return;
    }
    std::deque<coop::InboundPerms> got;
    g_inbound.drainPerms(got);
    for (std::deque<coop::InboundPerms>::iterator it = got.begin(); it != got.end(); ++it) {
        if (it->pkt.seq <= g_permSeenSeq && g_permSeenSeq != 0) continue; // stale resend
        g_permSeenSeq = it->pkt.seq;
        coop::engine::setJoinPermissions(it->pkt.joinBuild != 0, it->pkt.joinContainers != 0,
                                         it->pkt.joinBuy != 0);
        coop::engine::setTakeFromPlayers(it->pkt.takeFromPlayers != 0);
    }
}

// A real player session (not a scripted test): squad tabs follow player names.
// Scripted scenarios keep the old rank rule (host tab 0, join tab = its id).
bool realSession() { return g_cfg.scenario.empty() && g_cfg.testSeconds == 0; }

// Our own name as the session knows it (the host may have made it unique).
std::string myPlayerName() {
    std::map<coop::u32, std::string>::const_iterator it =
        g_names.find(g_cfg.isHost ? 0u : g_net.localId());
    return it != g_names.end() ? it->second : std::string();
}

// Take in names that arrived (HELLO on the host, PKT_ROSTER on a join). The
// host keeps them unique - two players with the same Steam name would own the
// same tab - by suffixing the newcomer's id.
void ingestRoster() {
    std::deque<coop::InboundRoster> got;
    g_inbound.drainRoster(got);
    if (g_cfg.isHost) {
        std::string me = coop::steaminvite::selfName();
        if (!me.empty() && g_names[0] != me) { g_names[0] = me; g_namesDirty = true; }
    }
    for (std::deque<coop::InboundRoster>::iterator it = got.begin(); it != got.end(); ++it) {
        std::string name = it->name;
        if (g_cfg.isHost && it->playerId != 0) {
            for (std::map<coop::u32, std::string>::const_iterator o = g_names.begin();
                 o != g_names.end(); ++o) {
                if (o->first == it->playerId || o->second != name) continue;
                char sfx[16];
                _snprintf(sfx, sizeof(sfx) - 1, " #%u", (unsigned)it->playerId);
                sfx[sizeof(sfx) - 1] = '\0';
                name += sfx;
                break;
            }
        }
        if (g_names[it->playerId] != name) {
            g_names[it->playerId] = name;
            g_namesDirty = true;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "[names] player %u is '%s'",
                      (unsigned)it->playerId, name.c_str());
            b[sizeof(b) - 1] = '\0';
            coopLog(b);
        }
    }
}

bool squadTabLess(const struct SquadTab& a, const struct SquadTab& b);
struct SquadTab {
    std::pair<coop::u32, coop::u32> key; // container hand: the tab
    unsigned int hand[5];                // one member, to reach the squad
    std::string  name;
};
bool squadTabLess(const SquadTab& a, const SquadTab& b) { return a.key < b.key; }
void listSquadTabs(GameWorld* gw, std::vector<SquadTab>& out) {
    out.clear();
    coop::EntityState sq[64];
    unsigned int n = coop::engine::captureSquad(gw, /*leaderOnly*/ false, sq, 64);
    for (unsigned int i = 0; i < n; ++i) {
        std::pair<coop::u32, coop::u32> k(sq[i].hContainer, sq[i].hContainerSerial);
        bool seen = false;
        for (size_t t = 0; t < out.size(); ++t) if (out[t].key == k) { seen = true; break; }
        if (seen) continue;
        SquadTab tab;
        tab.key = k;
        tab.hand[0] = sq[i].hType; tab.hand[1] = sq[i].hContainer;
        tab.hand[2] = sq[i].hContainerSerial; tab.hand[3] = sq[i].hIndex;
        tab.hand[4] = sq[i].hSerial;
        char nm[64];
        coop::engine::getSquadNameByHand(tab.hand, nm, sizeof(nm));
        tab.name = nm;
        out.push_back(tab);
    }
    std::sort(out.begin(), out.end(), squadTabLess);
}

std::set<std::string> connectedJoinNames() {
    std::set<std::string> out;
    for (std::set<coop::u32>::const_iterator it = g_peers.begin(); it != g_peers.end(); ++it) {
        std::map<coop::u32, std::string>::const_iterator nm = g_names.find(*it);
        if (*it != 0 && nm != g_names.end() && !nm->second.empty()) out.insert(nm->second);
    }
    return out;
}

// ---- Away mode (2026-10-23) --------------------------------------------------
// A player who leaves keeps their characters in the world, but safely: the host
// renames their tab "<name> (away)", and while a tab carries that suffix its
// members neither think (AI suspended - they stand where they are) nor take
// melee damage. The suffix rides in the save, so a later session keeps them
// safe; when the player rejoins, assignSquads drops the suffix and hands the
// tab back.
const char* AWAY_SUFFIX = " (away)";
std::vector<std::vector<unsigned int> > g_awayHands; // members of away tabs
DWORD g_awayScanTick = 0;

bool isAwayName(const std::string& n) {
    size_t k = strlen(AWAY_SUFFIX);
    return n.size() > k && n.compare(n.size() - k, k, AWAY_SUFFIX) == 0;
}

// Host leave edge: mark the leaver's tab away.
void markAway(GameWorld* gw, coop::u32 id) {
    if (!g_cfg.isHost || !realSession() || !gw || !coop::engine::gameplayLive(gw)) return;
    std::map<coop::u32, std::string>::const_iterator nm = g_names.find(id);
    if (nm == g_names.end() || nm->second.empty()) return;
    std::vector<SquadTab> tabs;
    listSquadTabs(gw, tabs);
    for (size_t t = 0; t < tabs.size(); ++t) {
        if (tabs[t].name != nm->second) continue;
        std::string away = nm->second + AWAY_SUFFIX;
        if (coop::engine::setSquadNameByHand(tabs[t].hand, away.c_str()) == 1) {
            coop::engine::refreshSquadTabs();
            char b[160];
            _snprintf(b, sizeof(b) - 1, "[away] '%s' left: their squad waits, frozen and protected",
                      nm->second.c_str());
            b[sizeof(b) - 1] = '\0';
            coopLog(b);
        }
        g_awayScanTick = 0; // rescan now
        return;
    }
}

// Host, every tick after the replicator rebuilt its suspend/guard sets.
void applyAwayGuards(GameWorld* gw) {
    if (!g_cfg.isHost || !gw) return;
    DWORD now = GetTickCount();
    if (g_awayScanTick == 0 || now - g_awayScanTick >= 1000) {
        g_awayScanTick = now;
        g_awayHands.clear();
        std::vector<SquadTab> tabs;
        listSquadTabs(gw, tabs);
        std::set<std::pair<coop::u32, coop::u32> > awayTabs;
        for (size_t t = 0; t < tabs.size(); ++t)
            if (isAwayName(tabs[t].name)) awayTabs.insert(tabs[t].key);
        if (!awayTabs.empty()) {
            coop::EntityState sq[64];
            unsigned int n = coop::engine::captureSquad(gw, /*leaderOnly*/ false, sq, 64);
            for (unsigned int i = 0; i < n; ++i) {
                if (!awayTabs.count(std::make_pair(sq[i].hContainer, sq[i].hContainerSerial)))
                    continue;
                std::vector<unsigned int> h(5);
                h[0] = sq[i].hType; h[1] = sq[i].hContainer; h[2] = sq[i].hContainerSerial;
                h[3] = sq[i].hIndex; h[4] = sq[i].hSerial;
                g_awayHands.push_back(h);
            }
        }
    }
    unsigned int guarded = 0;
    for (size_t i = 0; i < g_awayHands.size(); ++i) {
        const std::vector<unsigned int>& h = g_awayHands[i];
        Character* c = coop::engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) continue;
        coop::engine::addAiSuspend(c);
        coop::engine::addDamageGuard(c);
        ++guarded;
    }
    static unsigned int s_lastGuarded = 0;
    if (guarded != s_lastGuarded) {
        s_lastGuarded = guarded;
        char b[80];
        _snprintf(b, sizeof(b) - 1, "[away] guarding %u character(s)", guarded);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
}

// HOST, before the world is sent to a join: every connected player gets a
// squad tab named after them - the one already carrying their name, else an
// unnamed one ("NAMELESS_n", a co-op start's spare), else a new character in a
// new tab. The host keeps the first tab. Renames travel in the pushed save.
// Returns true when it CREATED a character: a body made this session has a
// different object id in the save than in the host's live world, so the host
// must reload that save too (see armConnectPush).
bool g_bootReloadHost = false;
bool assignSquads(GameWorld* gw) {
    if (!g_cfg.isHost || !realSession() || !gw) return false;
    std::vector<SquadTab> tabs;
    listSquadTabs(gw, tabs);
    if (tabs.empty()) return false;
    bool created = false;
    const std::string me = myPlayerName();
    const std::set<std::string> joins = connectedJoinNames();
    std::set<std::string> taken;
    for (size_t t = 0; t < tabs.size(); ++t) taken.insert(tabs[t].name);
    bool changed = false;
    if (!me.empty() && !taken.count(me) && !joins.count(tabs[0].name) &&
        coop::engine::setSquadNameByHand(tabs[0].hand, me.c_str()) == 1) {
        tabs[0].name = me; taken.insert(me); changed = true;
    }
    for (std::set<std::string>::const_iterator j = joins.begin(); j != joins.end(); ++j) {
        if (taken.count(*j)) continue;
        // Back from being away: the tab kept their name with " (away)".
        int away = -1;
        for (size_t t = 0; t < tabs.size() && away < 0; ++t)
            if (tabs[t].name == *j + AWAY_SUFFIX) away = (int)t;
        if (away >= 0 && coop::engine::setSquadNameByHand(tabs[away].hand, j->c_str()) == 1) {
            char rb[160];
            _snprintf(rb, sizeof(rb) - 1, "[away] '%s' is back", j->c_str());
            rb[sizeof(rb) - 1] = '\0';
            coopLog(rb);
            tabs[away].name = *j;
            taken.insert(*j);
            changed = true;
            continue;
        }
        int pick = -1;
        for (size_t t = 1; t < tabs.size() && pick < 0; ++t) {
            const std::string& nm = tabs[t].name;
            if (nm == me || joins.count(nm)) continue;
            std::string low = nm;
            for (size_t c = 0; c < low.size(); ++c) low[c] = (char)tolower((unsigned char)low[c]);
            if (low.find("nameless") == 0 || low.find("(co-op)") != std::string::npos)
                pick = (int)t;
        }
        char b[160];
        if (pick >= 0 && coop::engine::setSquadNameByHand(tabs[pick].hand, j->c_str()) == 1) {
            _snprintf(b, sizeof(b) - 1, "[names] squad '%s' -> '%s' (free tab)",
                      tabs[pick].name.c_str(), j->c_str());
            tabs[pick].name = *j;
            g_lookOfferNames.insert(*j); // a start character: let them make it theirs
        } else if (coop::engine::createPlayerSquad(gw, j->c_str()) == 1) {
            created = true;
            g_lookOfferNames.insert(*j); // a random character: let them make it theirs
            _snprintf(b, sizeof(b) - 1, "[names] new character + squad '%s'", j->c_str());
        } else {
            _snprintf(b, sizeof(b) - 1, "[names] FAILED to give '%s' a squad", j->c_str());
        }
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
        taken.insert(*j);
        changed = true;
    }
    if (changed) coop::engine::refreshSquadTabs();
    return created;
}

// EVERY client: who controls which tab, by name. A join owns the tab with its
// name; the host owns every tab not named after a connected join.
std::map<std::pair<coop::u32, coop::u32>, bool> g_lastTabOwn;
void applyTabOwnership(GameWorld* gw) {
    const std::string me = myPlayerName();
    if (me.empty()) return;
    std::vector<SquadTab> tabs;
    listSquadTabs(gw, tabs);
    const std::set<std::string> joins = connectedJoinNames();
    std::map<std::pair<coop::u32, coop::u32>, bool> own;
    for (size_t t = 0; t < tabs.size(); ++t)
        own[tabs[t].key] = g_cfg.isHost ? (joins.count(tabs[t].name) == 0)
                                        : (tabs[t].name == me);
    if (own == g_lastTabOwn) return;
    g_lastTabOwn = own;
    g_repl.setTabNameOwnership(own);
    std::string mine;
    for (size_t t = 0; t < tabs.size(); ++t)
        if (own[tabs[t].key]) mine += (mine.empty() ? "" : ", ") + tabs[t].name;
    char b[200];
    _snprintf(b, sizeof(b) - 1, "[names] I control: %s",
              mine.empty() ? "(no tab yet)" : mine.c_str());
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
}

void tickRoster(GameWorld* gw) {
    ingestRoster();
    DWORD now = GetTickCount();
    if (g_cfg.isHost && g_peerPresent && g_net.isRunning() &&
        (g_namesDirty || now - g_namesSentTick >= ROSTER_RESEND_MS)) {
        for (std::map<coop::u32, std::string>::const_iterator it = g_names.begin();
             it != g_names.end(); ++it) {
            if (it->second.empty()) continue;
            coop::RosterPacket p;
            memset(&p, 0, sizeof(p));
            p.type = (coop::u8)coop::PKT_ROSTER;
            p.playerId = it->first;
            size_t nl = it->second.size();
            if (nl > coop::ROSTER_NAME_MAX) nl = coop::ROSTER_NAME_MAX;
            p.nameLen = (coop::u8)nl;
            memcpy(p.name, it->second.data(), nl);
            g_net.queueRoster(p);
        }
        g_namesSentTick = now;
    }
    if (!realSession() || g_names.empty() || !gw || !g_gameStarted ||
        !coop::engine::gameplayLive(gw)) return;
    if (!g_namesDirty && now - g_namesApplyTick < NAMES_APPLY_MS) return;
    g_namesApplyTick = now;
    g_namesDirty = false;
    // Hosting with nobody connected yet: name our own tab now (no join, so
    // assignSquads only renames - it never creates a character here).
    if (g_cfg.isHost && g_net.isRunning() && g_peers.empty()) assignSquads(gw);
    applyTabOwnership(gw);
}

// Pre-save hook target: copy the save folder Kenshi is about to overwrite into
// the rolling backups (sync/SaveXfer.h). A first save has nothing to copy.
void backupBeforeSave(const char* name, bool /*autosave*/) {
    if (name && name[0]) coop::savexfer::backupCopy(name);
}

// End-of-hook: close the tick and, once per window, log the line and restart.
void perfEndTick(unsigned __int64 hookT0) {
    if (!g_perfWindowMs) return;
    g_perf.add(coop::perf::S_HOOK, perfNow() - hookT0);
    g_perf.endTick();
    if (g_perf.windowDue(GetTickCount(), g_perfWindowMs)) {
        char line[512];
        g_perf.format(line, sizeof(line), g_perfFreq, g_perfWindowMs, 8);
        coopLog(line);
        g_perf.reset();
    }
}

// Blank-portraits diagnostic (protocol 36): loading a save folder without its
// portrait atlas blanks the squad-tab avatars until the next portrait rebuild.
// Warn at every coordinated-load issue point so a session log pinpoints WHICH
// load introduced the blank avatars (host reload, join GO, or a transferred
// copy that shipped without the atlas).
void warnIfNoPortraits(const std::string& name) {
    std::string p = coop::savexfer::saveFolderFor(name) + "\\portraits_texture.png";
    if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "[load] WARN save '%s' has no portraits_texture.png "
                  "(squad avatars will render blank)", name.c_str());
        b[sizeof(b) - 1] = '\0'; coopErr(b);
    }
}

// SessionController reset primitives (Phase 3). The old world / peer is gone, so
// both drop the Replicator's session maps and the Inbound world-state queues in
// a fixed order; centralizing them keeps that order from drifting between the
// several edges that each need one.

// UI connect/disconnect teardown: the world is live (reconnect/disconnect from
// within a running game), so despawn our minted NPC + world-item proxies before
// clearing the maps - else a reconnect leaves orphaned duplicates or bakes them
// into the next save. Falls back to a plain map reset if no world has ticked yet.
void sessionResetForUi() {
    g_peerPresent = false;
    g_peers.clear();
    if (g_lastGw) g_repl.clearPeerReplicationState(g_lastGw);
    else          g_repl.resetSession();
    g_inbound.flushWorldState();
    g_net.bumpSessionEpoch(); // v44: fence off any in-flight prior-session batch
}

// World-reload session reset (protocol 32): the old world is gone - every
// pointer cache and session map describes it. Peer presence and the suppression
// levers survive (the connection never dropped). Both roles run this on their
// OWN reload edge, and the synchronous-swap backstop reuses it, so the reset
// order (repl maps, then inbound queues) is defined in exactly one place.
void sessionResetForWorldReload() {
    g_repl.resetSession();
    g_inbound.flushWorldState();
    g_net.bumpSessionEpoch(); // v44: post-reload batches supersede the old session
    coopLog("[load] inbound world-state queues flushed");
    // An editor open across a world swap edits a character that no longer
    // exists: drop it (an unfinished offer is kept and reopens on the new world).
    if (g_lookEditing) { g_lookEditing = false; g_lookWanted = true; g_lookWantTick = GetTickCount(); }
    g_lastWorldResetTick = GetTickCount();
    g_lookAwaitReload = 0; // the new world is here; a waiting editor may reopen
}

// Push-save-on-connect (host): bake a fresh save of the live world and arm the
// bootstrap so driveSaveSync announces it to the join with a LOAD_GO once the
// folder quiesces. Called for either connect ordering: a peer connecting while
// the host is already in-game (processNetEvents), or the host's gameplay
// starting with a peer already connected (mainLoop_hook gameplay-start edge).
// Host + saveSync + in-game are the caller's responsibility.
void armConnectPush() {
    // Every connected player gets a named tab first: it rides in this save.
    ingestRoster();
    if (assignSquads(g_lastGw)) g_bootReloadHost = true;
    char cur[64];
    cur[0] = '\0';
    coop::engine::saveInfo(cur, sizeof(cur), 0, 0);
    std::string name = cur[0] ? cur : "coopresume";
    g_bootstrapArmed = true;
    g_bootstrapName  = name;
    char b[144];
    _snprintf(b, sizeof(b) - 1,
              "[boot] baking save '%s' to push to join on connect", name.c_str());
    b[sizeof(b) - 1] = '\0'; coopLog(b);
    if (!coop::engine::saveGameAs(name))
        coopErr("[boot] connect-push save FAILED to issue");
}

// The character this client plays: the squad tab named after us, else the first
// squad member the owner classifier calls ours.
bool myCharacterHand(GameWorld* gw, unsigned int out[5]) {
    std::vector<SquadTab> tabs;
    listSquadTabs(gw, tabs);
    std::map<coop::u32, std::string>::const_iterator me = g_names.find(g_net.localId());
    if (me != g_names.end())
        for (size_t i = 0; i < tabs.size(); ++i)
            if (tabs[i].name == me->second) { memcpy(out, tabs[i].hand, sizeof(tabs[i].hand)); return true; }
    for (size_t i = 0; i < tabs.size(); ++i)
        if (g_repl.ownerClassForHand(tabs[i].hand) == 1) { memcpy(out, tabs[i].hand, sizeof(tabs[i].hand)); return true; }
    return false;
}

void sendLookApply(GameWorld* gw) {
    std::string name, race;
    if (!coop::engine::exportCharacterLook(g_lookHand, LOOK_OUT_FILE, name, race)) {
        coopErr("[look] could not read the edited character");
        return;
    }
    std::vector<coop::u8> data;
    FILE* f = fopen(LOOK_OUT_FILE, "rb");
    if (f) {
        unsigned char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0 && data.size() <= coop::CHAR_LOOK_DATA_MAX)
            data.insert(data.end(), buf, buf + got);
        fclose(f);
    }
    if (data.empty() || data.size() > coop::CHAR_LOOK_DATA_MAX) {
        coopErr("[look] appearance file missing or too large - not sent");
        return;
    }
    if (name.size() > coop::CHAR_LOOK_NAME_MAX) name.resize(coop::CHAR_LOOK_NAME_MAX);
    if (race.size() > coop::CHAR_LOOK_RACE_MAX) race.clear();
    coop::CharLookHeader h;
    memset(&h, 0, sizeof(h));
    h.type    = (coop::u8)coop::PKT_CHAR_LOOK;
    h.kind    = coop::CHAR_LOOK_APPLY;
    h.ownerId = g_net.localId();
    h.target  = g_net.localId();
    for (int i = 0; i < 5; ++i) h.hand[i] = g_lookHand[i];
    h.nameLen = (coop::u16)name.size();
    h.raceLen = (coop::u16)race.size();
    h.dataLen = (coop::u32)data.size();
    g_net.queueCharLook(h, name, race, data);
    char b[192];
    _snprintf(b, sizeof(b) - 1, "[look] sent my character to the host: name='%s' race='%s' bytes=%u",
              name.c_str(), race.c_str(), (unsigned)data.size());
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
}

// Host: a player's edited character. Only their own squad may be changed.
void applyLook(GameWorld* gw, const coop::InboundCharLook& cl) {
    std::map<coop::u32, std::string>::const_iterator nm = g_names.find(cl.senderId);
    char sq[64];
    sq[0] = '\0';
    coop::engine::getSquadNameByHand(cl.hdr.hand, sq, sizeof(sq));
    // Real sessions name each player's squad after them; scripted tests have no
    // names, so there it is enough that the character is another player's.
    bool theirs = realSession() ? (nm != g_names.end() && nm->second == sq)
                                : g_repl.ownerClassForHand(cl.hdr.hand) == 2;
    if (!theirs) {
        char b[160];
        _snprintf(b, sizeof(b) - 1, "[look] REFUSED player %u's look: character is in squad '%s', not theirs",
                  (unsigned)cl.senderId, sq);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
        return;
    }
    FILE* f = fopen(LOOK_IN_FILE, "wb");
    if (!f) { coopErr("[look] cannot write the look file"); return; }
    if (!cl.data.empty()) fwrite(&cl.data[0], 1, cl.data.size(), f);
    fclose(f);
    unsigned int hand[5];
    for (int i = 0; i < 5; ++i) hand[i] = cl.hdr.hand[i];
    bool ok = coop::engine::importCharacterLook(gw, hand, LOOK_IN_FILE, cl.name, cl.race);
    char b[192];
    _snprintf(b, sizeof(b) - 1, "[look] player %u's character: name='%s' race='%s' %s",
              (unsigned)cl.senderId, cl.name.c_str(), cl.race.c_str(), ok ? "applied" : "FAILED");
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
    if (ok && g_cfg.saveSync) {
        // Everyone reloads the saved world, so every client draws the new look.
        g_bootReloadHost = true;
        armConnectPush();
    }
}

void tickLook(GameWorld* gw) {
    std::deque<coop::InboundCharLook> got;
    g_inbound.drainCharLooks(got);
    for (std::deque<coop::InboundCharLook>::iterator it = got.begin(); it != got.end(); ++it) {
        if (it->hdr.kind == coop::CHAR_LOOK_OFFER && !g_cfg.isHost &&
            it->hdr.target == g_net.localId()) {
            g_lookWanted = true;
            g_lookWantTick = GetTickCount();
            coopLog("[look] the host offered the character editor");
        } else if (it->hdr.kind == coop::CHAR_LOOK_APPLY && g_cfg.isHost && gw) {
            applyLook(gw, *it);
        }
    }
    if (!gw || !g_gameStarted || !coop::engine::gameplayLive(gw)) return;
    DWORD now = GetTickCount();
    // Test lever (KENSHICOOP_LOOK_TEST=N): a join opens the editor N s into the
    // game and closes it 4 s later, so the export -> send -> apply -> reload path
    // runs without a mouse. Off by default.
    static int lookTestS = -1;
    static bool lookTestFired = false;
    if (lookTestS < 0) {
        const char* e = getenv("KENSHICOOP_LOOK_TEST");
        lookTestS = (e && e[0]) ? atoi(e) : 0;
    }
    if (lookTestS > 0 && !g_cfg.isHost && g_peerPresent && !lookTestFired &&
        now - g_gameStartTick >= (DWORD)lookTestS * 1000u) {
        lookTestFired = true;
        g_lookWanted = true;
        g_lookWantTick = now - 3000;
        coopLog("[look] TEST: opening the editor");
    }
    if (lookTestS > 0 && g_lookEditing && g_lookSeenOpen && now - g_lookOpenTick > 4000) {
        coopLog("[look] TEST: closing the editor");
        coop::engine::closeCharacterEditor(gw);
    }
    // Closed for a reload: wait for that reload's world (60 s backstop), or the
    // reopened editor blocks the very load it made way for.
    if (g_lookAwaitReload != 0 && now - g_lookAwaitReload > 60000) g_lookAwaitReload = 0;
    if (g_lookWanted && !g_lookEditing && g_lookAwaitReload == 0 && now - g_lookWantTick >= 3000 &&
        now - g_lastWorldResetTick >= 5000 && g_readyWait.empty()) {
        g_lookWanted = false;
        if (!myCharacterHand(gw, g_lookHand)) {
            coopLog("[look] no character of mine found - editor not opened");
        } else if (coop::engine::openCharacterEditor(gw, g_lookHand)) {
            g_lookEditing = true; g_lookSeenOpen = false; g_lookOpenTick = now;
            coopLog("[look] character editor opened");
        } else {
            coopErr("[look] character editor FAILED to open");
        }
    }
    if (g_lookEditing) {
        int st = coop::engine::characterEditorOpen(gw);
        if (st == 1) g_lookSeenOpen = true;
        if (st == 0 && g_lookSeenOpen) {
            g_lookEditing = false;
            coopLog("[look] character editor closed");
            if (!g_cfg.isHost) {
                if (g_peerPresent) sendLookApply(gw);
            } else if (g_peerPresent && g_cfg.saveSync) {
                // The host's own edit: reload everyone so they see it too.
                g_bootReloadHost = true;
                armConnectPush();
            }
        } else if (!g_lookSeenOpen && now - g_lookOpenTick > 5000) {
            g_lookEditing = false;
            coopErr("[look] character editor never opened - giving up");
        }
    }
}

// ---- Resync (protocol 61) and problem report ----------------------------------
// Resync: the host saves the live world and every client reloads it (the same
// coordinated reload a new player's squad uses). A join asks with
// PKT_RESYNC_REQ. One every RESYNC_MIN_MS, so a stuck button cannot loop it.
DWORD       g_resyncTick = 0;
const DWORD RESYNC_MIN_MS = 30000;
std::string g_uiNotice;
DWORD       g_uiNoticeTick = 0;
const DWORD UI_NOTICE_MS = 8000;

void uiNotice(const std::string& msg) { g_uiNotice = msg; g_uiNoticeTick = GetTickCount(); }

void hostResync(coop::u32 askedBy) {
    DWORD now = GetTickCount();
    char b[128];
    if (!g_cfg.saveSync || !g_gameStarted) {
        coopLog("[resync] not available (no save sync or not in a game)");
        return;
    }
    if (g_resyncTick != 0 && now - g_resyncTick < RESYNC_MIN_MS) {
        _snprintf(b, sizeof(b) - 1, "[resync] request from player %u ignored (one every %lu s)",
                  (unsigned)askedBy, (unsigned long)(RESYNC_MIN_MS / 1000));
        b[sizeof(b) - 1] = '\0'; coopLog(b);
        return;
    }
    g_resyncTick = now;
    _snprintf(b, sizeof(b) - 1, "[resync] player %u asked - saving and reloading everyone", (unsigned)askedBy);
    b[sizeof(b) - 1] = '\0'; coopLog(b);
    uiNotice("Resync: everyone reloads the host's world");
    g_bootReloadHost = true;
    armConnectPush();
}

void onResyncRequest() {
    if (g_cfg.isHost) { hostResync(0); return; }
    coop::ResyncReqPacket r;
    memset(&r, 0, sizeof(r));
    r.type = (coop::u8)coop::PKT_RESYNC_REQ;
    r.ownerId = g_net.localId();
    g_net.queueResyncReq(r);
    coopLog("[resync] asked the host to resync everyone");
    uiNotice("Resync requested - the host will reload everyone");
}

void onReportRequest(); // below
void tickResync() {
    // Test lever (KENSHICOOP_RESYNC_TEST=N): a join saves a problem report and
    // asks for a resync N s into the game - the panel buttons without a mouse.
    static int rsTestS = -1;
    static bool rsTestFired = false;
    if (rsTestS < 0) {
        const char* e = getenv("KENSHICOOP_RESYNC_TEST");
        rsTestS = (e && e[0]) ? atoi(e) : 0;
    }
    if (rsTestS > 0 && !rsTestFired && !g_cfg.isHost && g_peerPresent && g_gameStarted &&
        GetTickCount() - g_gameStartTick >= (DWORD)rsTestS * 1000u) {
        rsTestFired = true;
        coopLog("[resync] TEST: report + resync request");
        onReportRequest();
        onResyncRequest();
    }
    std::deque<coop::u32> got;
    g_inbound.drainResyncReqs(got);
    if (!g_cfg.isHost) return;
    for (std::deque<coop::u32>::iterator it = got.begin(); it != got.end(); ++it) hostResync(*it);
}

// Problem report: copy this client's logs into a fresh Desktop folder the player
// can zip and send. Plain file copies - nothing leaves the machine by itself.
void onReportRequest() {
    char desk[MAX_PATH];
    if (FAILED(SHGetFolderPathA(0, CSIDL_DESKTOPDIRECTORY, 0, 0, desk))) {
        coopErr("[report] Desktop folder not found");
        uiNotice("Could not find your Desktop folder");
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char leaf[64];
    _snprintf(leaf, sizeof(leaf) - 1, "KenshiTogether-report-%04u%02u%02u-%02u%02u%02u",
              t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    leaf[sizeof(leaf) - 1] = '\0';
    std::string dir = std::string(desk) + "\\" + leaf;
    if (!CreateDirectoryA(dir.c_str(), 0)) { coopErr("[report] could not create the folder"); return; }
    const char* files[] = { "KenshiCoop_host.log", "KenshiCoop_join.log", "kenshi.log",
                            "RE_Kenshi_log.txt", "KenshiCoop_prefs.txt", "data\\mods.cfg" };
    unsigned int copied = 0;
    // The log this session is writing first (a custom KENSHICOOP_LOG path is not
    // one of the default names), then the usual files beside the game.
    std::vector<std::string> srcs;
    if (!g_cfg.logPath.empty()) srcs.push_back(g_cfg.logPath);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        if (g_cfg.logPath != files[i]) srcs.push_back(files[i]);
    for (size_t i = 0; i < srcs.size(); ++i) {
        std::string src = srcs[i];
        std::string base = src.substr(src.find_last_of('\\') == std::string::npos ? 0
                                      : src.find_last_of('\\') + 1);
        if (CopyFileA(src.c_str(), (dir + "\\" + base).c_str(), FALSE)) ++copied;
    }
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA("crashDump*", &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (CopyFileA(fd.cFileName, (dir + "\\" + fd.cFileName).c_str(), FALSE)) ++copied;
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    FILE* f = fopen((dir + "\\report.txt").c_str(), "w");
    if (f) {
        fprintf(f, "Kenshi Together problem report\n");
        fprintf(f, "time: %04u-%02u-%02u %02u:%02u:%02u\n", t.wYear, t.wMonth, t.wDay,
                t.wHour, t.wMinute, t.wSecond);
        fprintf(f, "plugin build: %s %s, protocol %u\n", __DATE__, __TIME__,
                (unsigned)coop::PROTOCOL_VERSION);
        fprintf(f, "role: %s, local id %u, connected: %s, in game: %s\n",
                g_cfg.isHost ? "host" : "join", (unsigned)g_net.localId(),
                g_peerPresent ? "yes" : "no", g_gameStarted ? "yes" : "no");
        for (std::map<coop::u32, std::string>::const_iterator it = g_names.begin(); it != g_names.end(); ++it)
            fprintf(f, "player %u: %s%s\n", (unsigned)it->first, it->second.c_str(),
                    it->first == g_net.localId() ? " (you)" :
                    (it->first == 0 || g_peers.count(it->first)) ? "" : " (not connected)");
        // Memory: every load crash in our own tests (2026-10-22..24) matched a
        // Windows low-virtual-memory event - Kenshi needs 4-5 GB per instance.
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms))
            fprintf(f, "memory: RAM %.1f GB (%.1f free), commit %.1f GB (%.1f free), load %lu%%\n",
                    ms.ullTotalPhys / 1073741824.0, ms.ullAvailPhys / 1073741824.0,
                    ms.ullTotalPageFile / 1073741824.0, ms.ullAvailPageFile / 1073741824.0,
                    (unsigned long)ms.dwMemoryLoad);
        fprintf(f, "\nWhat happened (write it here before sending):\n\n");
        fclose(f);
        ++copied;
    }
    char b[200];
    _snprintf(b, sizeof(b) - 1, "[report] saved %u file(s) to Desktop \\ %s", copied, leaf);
    b[sizeof(b) - 1] = '\0'; coopLog(b);
    uiNotice(std::string("Report saved on your Desktop: ") + leaf);
}

// ---- Consent trade (protocol 62) ---------------------------------------------
// A take the rule refused becomes a request to the item's owner; they answer in
// the panel; on accept THEIR client moves the item (owner-authoritative
// inventory) and the answer goes back. Requests expire after TRADE_EXPIRE_MS.
struct PendingTrade {
    coop::TradeReqPacket req;
    DWORD                tick;
};
std::vector<PendingTrade> g_tradeIn;      // requests waiting for OUR answer
coop::u32                 g_tradeReqSeq = 0;
const DWORD               TRADE_EXPIRE_MS = 120000;
const size_t              TRADE_MAX_PENDING = 4;
std::string               g_tradeText[4];  // panel lines (kept alive for the panel)

std::string playerName(coop::u32 id) {
    std::map<coop::u32, std::string>::const_iterator it = g_names.find(id);
    if (it != g_names.end()) return it->second;
    char b[24];
    _snprintf(b, sizeof(b) - 1, id == 0 ? "the host" : "player %u", (unsigned)id);
    b[sizeof(b) - 1] = '\0';
    return b;
}

// Which player owns the character at `hand`: the player its squad is named after.
// An away squad's owner is offline (returns false). Scripted sessions have no
// names: there the other side of a two-player test is the answer.
bool ownerOfCharacter(const unsigned int hand[5], coop::u32* out, bool* away) {
    *away = false;
    char sq[64];
    sq[0] = '\0';
    coop::engine::getSquadNameByHand(hand, sq, sizeof(sq));
    if (isAwayName(sq)) { *away = true; return false; }
    for (std::map<coop::u32, std::string>::const_iterator it = g_names.begin(); it != g_names.end(); ++it)
        if (it->second == sq) { *out = it->first; return true; }
    if (realSession()) { *out = 0; return true; } // unnamed squads are the host's
    if (!g_cfg.isHost) { *out = 0; return true; }
    if (!g_peers.empty()) { *out = *g_peers.begin(); return true; }
    return false;
}

void sendTradeRequest(const coop::engine::TakeRequestEdge& e) {
    coop::u32 owner = 0;
    bool away = false;
    if (!ownerOfCharacter(e.src, &owner, &away)) {
        uiNotice(away ? "That player is away - ask them when they are back"
                      : "Could not tell whose item that is");
        return;
    }
    if (owner == g_net.localId()) return;
    coop::TradeReqPacket r;
    memset(&r, 0, sizeof(r));
    r.type = (coop::u8)coop::PKT_TRADE_REQ;
    r.ownerId = g_net.localId();
    r.target = owner;
    r.reqId = ++g_tradeReqSeq;
    memcpy(r.src, e.src, sizeof(r.src));
    memcpy(r.dst, e.dst, sizeof(r.dst));
    r.typeCat = e.typeCat;
    r.qty = e.qty;
    memcpy(r.sid, e.sid, sizeof(r.sid));
    memcpy(r.name, e.name, sizeof(r.name));
    r.sid[sizeof(r.sid) - 1] = '\0'; r.name[sizeof(r.name) - 1] = '\0';
    g_net.queueTradeReq(r);
    char b[200];
    _snprintf(b, sizeof(b) - 1, "[trade] asked %s for '%s' x%d (req %u)",
              playerName(owner).c_str(), r.name, (int)r.qty, (unsigned)r.reqId);
    b[sizeof(b) - 1] = '\0'; coopLog(b);
    uiNotice("Asked " + playerName(owner) + " for " + r.name + " - waiting for their answer");
}

void answerTrade(size_t i, bool accept, bool expired) {
    if (i >= g_tradeIn.size()) return;
    coop::TradeReqPacket q = g_tradeIn[i].req;
    g_tradeIn.erase(g_tradeIn.begin() + i);
    int moved = 0;
    if (accept && g_lastGw && g_repl.ownerClassForHand(q.src) == 1) {
        moved = coop::engine::moveItemBetweenContainers(g_lastGw, q.src, q.dst, q.sid,
                                                        q.typeCat, q.qty, /*suspendVeto*/ true);
        if (moved == 0 && coop::engine::lastMoveFailReason() == 2) moved = -2; // no room
    }
    coop::TradeRespPacket p;
    memset(&p, 0, sizeof(p));
    p.type = (coop::u8)coop::PKT_TRADE_RESP;
    p.ownerId = g_net.localId();
    p.target = q.ownerId;
    p.reqId = q.reqId;
    p.accepted = accept ? 1 : 0;
    p.moved = moved;
    g_net.queueTradeResp(p);
    char b[200];
    _snprintf(b, sizeof(b) - 1, "[trade] %s %s's request for '%s' x%d - moved %d",
              expired ? "expired" : (accept ? "accepted" : "declined"),
              playerName(q.ownerId).c_str(), q.name, (int)q.qty, moved);
    b[sizeof(b) - 1] = '\0'; coopLog(b);
    if (accept && moved == 0) uiNotice(std::string("Could not give ") + q.name + " - it is no longer there");
    if (accept && moved == -2) uiNotice(std::string("Could not give ") + q.name + " - their character has no room");
}

void onTradeAnswer(int index, bool accept) { answerTrade((size_t)index, accept, false); }

// ---- Shared pings (protocol 64) -----------------------------------------------
// A player marks a world point (key, default G, or the panel button); every
// client shows "[ name ]" there for PING_SHOW_MS. One label per player - a new
// ping moves it.
struct PingMark { void* label; DWORD until; };
std::map<coop::u32, PingMark> g_pings;
const DWORD PING_SHOW_MS = 30000;
const DWORD PING_COOLDOWN_MS = 1000;
DWORD g_pingSentTick = 0;
int   g_pingVk = 0;
char  g_pingKeyName[8] = { 0 };

void initPingKey() {
    if (g_pingVk) return;
    const char* e = getenv("KENSHICOOP_PING_KEY");
    char k = (e && e[0]) ? (char)toupper((unsigned char)e[0]) : 'G';
    if (!((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9'))) k = 'G';
    g_pingVk = (int)k;
    g_pingKeyName[0] = k; g_pingKeyName[1] = '\0';
}

void showPing(coop::u32 owner, float x, float y, float z) {
    std::map<coop::u32, PingMark>::iterator old = g_pings.find(owner);
    if (old != g_pings.end()) { coop::engine::markerDestroy(old->second.label); g_pings.erase(old); }
    // MyGUI reads '#' as the start of a colour code ("#RRGGBB"): escape it,
    // or "Name #2" (a duplicate Steam name) renders as "Name".
    std::string name = playerName(owner), safe;
    for (size_t i = 0; i < name.size(); ++i) { safe += name[i]; if (name[i] == '#') safe += '#'; }
    std::string text = "> " + safe;
    void* l = coop::engine::pingLabelCreate(text.c_str(), x, y + 3.0f, z);
    if (!l) return;
    PingMark m; m.label = l; m.until = GetTickCount() + PING_SHOW_MS;
    g_pings[owner] = m;
}

void onPingRequest() {
    if (!g_lastGw || !g_peerPresent) return;
    DWORD now = GetTickCount();
    if (g_pingSentTick != 0 && now - g_pingSentTick < PING_COOLDOWN_MS) return;
    float p[3];
    bool mouse = false;
    if (!coop::engine::pingPoint(g_lastGw, p, &mouse)) { uiNotice("Could not find a spot to ping"); return; }
    g_pingSentTick = now;
    coop::PingPacket pk;
    memset(&pk, 0, sizeof(pk));
    pk.type = (coop::u8)coop::PKT_PING;
    pk.ownerId = g_net.localId();
    pk.x = p[0]; pk.y = p[1]; pk.z = p[2];
    g_net.queuePing(pk);
    showPing(g_net.localId(), p[0], p[1], p[2]);
    char b[128];
    _snprintf(b, sizeof(b) - 1, "[ping] sent %s at %.0f,%.0f,%.0f", mouse ? "mouse point" : "camera center",
              p[0], p[1], p[2]);
    b[sizeof(b) - 1] = '\0'; coopLog(b);
}

void tickPing() {
    initPingKey();
    DWORD now = GetTickCount();
    // A world reload drops the GUI's world labels with the old world.
    static DWORD s_resetSeen = 0;
    if (g_lastWorldResetTick != s_resetSeen) { s_resetSeen = g_lastWorldResetTick; g_pings.clear(); }
    // Hotkey edge, only while Kenshi is the foreground window (GetAsyncKeyState
    // is system-wide).
    static bool s_down = false;
    bool down = false;
    if (g_peerPresent && g_gameStarted) {
        HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        down = fgPid == GetCurrentProcessId() && (GetAsyncKeyState(g_pingVk) & 0x8000) != 0;
    }
    if (down && !s_down) onPingRequest();
    s_down = down;
    // Test lever (KENSHICOOP_PING_TEST=N): a join pings N s into the game.
    static int ptS = -1;
    static bool ptFired = false;
    if (ptS < 0) { const char* e = getenv("KENSHICOOP_PING_TEST"); ptS = (e && e[0]) ? atoi(e) : 0; }
    if (ptS > 0 && !ptFired && !g_cfg.isHost && g_peerPresent && g_gameStarted &&
        now - g_gameStartTick >= (DWORD)ptS * 1000u) {
        ptFired = true;
        coopLog("[ping] TEST: pinging");
        onPingRequest();
    }
    std::deque<coop::PingPacket> got;
    g_inbound.drainPings(got);
    for (std::deque<coop::PingPacket>::iterator it = got.begin(); it != got.end(); ++it) {
        if (it->ownerId == g_net.localId()) continue;
        showPing(it->ownerId, it->x, it->y, it->z);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "[ping] %s pinged %.0f,%.0f,%.0f label=%d", playerName(it->ownerId).c_str(),
                  it->x, it->y, it->z, g_pings.count(it->ownerId) ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coopLog(b);
        uiNotice(playerName(it->ownerId) + " pinged a spot");
    }
    for (std::map<coop::u32, PingMark>::iterator it = g_pings.begin(); it != g_pings.end();) {
        if ((long)(now - it->second.until) >= 0) {
            coop::engine::markerDestroy(it->second.label);
            g_pings.erase(it++);
        } else ++it;
    }
}

// ---- Trader sync (protocol 63) -------------------------------------------------
// A join's purchase from a trader is reported to the host, which takes the same
// units out of its copy of the trader (the stock it publishes to everyone).
void tickShop() {
    coop::engine::ShopBuyEdge buys[8];
    unsigned int nb = coop::engine::drainShopBuys(buys, 8);
    if (!g_cfg.isHost && g_peerPresent) {
        for (unsigned int i = 0; i < nb; ++i) {
            // From a trader (not a squad member) to one of our own characters.
            if (g_repl.ownerClassForHand(buys[i].seller) != 0) continue;
            if (g_repl.ownerClassForHand(buys[i].buyer) != 1) continue;
            coop::ShopTakePacket p;
            memset(&p, 0, sizeof(p));
            p.type = (coop::u8)coop::PKT_SHOP_TAKE;
            p.ownerId = g_net.localId();
            memcpy(p.trader, buys[i].seller, sizeof(p.trader));
            p.typeCat = buys[i].typeCat;
            p.qty = buys[i].qty;
            memcpy(p.sid, buys[i].sid, sizeof(p.sid));
            p.sid[sizeof(p.sid) - 1] = '\0';
            g_net.queueShopTake(p);
            char b[160];
            _snprintf(b, sizeof(b) - 1, "[shop] told the host: bought '%s' x%d from trader %u,%u",
                      p.sid, (int)p.qty, p.trader[3], p.trader[4]);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        }
    }
    std::deque<coop::ShopTakePacket> takes;
    g_inbound.drainShopTakes(takes);
    if (!g_cfg.isHost) return;
    for (std::deque<coop::ShopTakePacket>::iterator it = takes.begin(); it != takes.end(); ++it) {
        it->sid[sizeof(it->sid) - 1] = '\0';
        int removed = coop::engine::removeItemsBySid(it->trader, it->sid, it->typeCat, it->qty);
        char b[176];
        _snprintf(b, sizeof(b) - 1, "[shop] %s bought '%s' x%d from trader %u,%u - removed %d from its stock",
                  playerName(it->ownerId).c_str(), it->sid, (int)it->qty, it->trader[3], it->trader[4], removed);
        b[sizeof(b) - 1] = '\0'; coopLog(b);
    }
}

void tickTrade(GameWorld* gw) {
    // Our refused takes -> requests.
    coop::engine::TakeRequestEdge edges[8];
    unsigned int ne = coop::engine::drainTakeRequests(edges, 8);
    if (g_peerPresent)
        for (unsigned int i = 0; i < ne; ++i) sendTradeRequest(edges[i]);
    // Test lever (KENSHICOOP_TRADE_TEST=N): N s into the game a join asks for the
    // first loose item of a character of the other player's; the host accepts
    // any request 3 s after it arrives.
    static int ttS = -1;
    static bool ttFired = false;
    if (ttS < 0) { const char* e = getenv("KENSHICOOP_TRADE_TEST"); ttS = (e && e[0]) ? atoi(e) : 0; }
    DWORD now = GetTickCount();
    if (ttS > 0 && !ttFired && !g_cfg.isHost && g_peerPresent && g_gameStarted && gw &&
        now - g_gameStartTick >= (DWORD)ttS * 1000u) {
        ttFired = true;
        std::vector<SquadTab> tabs;
        listSquadTabs(gw, tabs);
        const SquadTab* theirs = 0; const SquadTab* mine = 0;
        for (size_t t = 0; t < tabs.size(); ++t) {
            int c = g_repl.ownerClassForHand(tabs[t].hand);
            if (c == 2 && !theirs) theirs = &tabs[t];
            if (c == 1 && !mine) mine = &tabs[t];
        }
        coop::engine::TakeRequestEdge e;
        memset(&e, 0, sizeof(e));
        if (theirs && mine &&
            coop::engine::firstItemOf(theirs->hand, e.sid, sizeof(e.sid), &e.typeCat, e.name, sizeof(e.name))) {
            memcpy(e.src, theirs->hand, sizeof(e.src));
            memcpy(e.dst, mine->hand, sizeof(e.dst));
            e.qty = 1;
            coopLog("[trade] TEST: asking for an item");
            sendTradeRequest(e);
        } else {
            char tb[128];
            _snprintf(tb, sizeof(tb) - 1, "[trade] TEST: no item to ask for (tabs=%u theirs=%d mine=%d)",
                      (unsigned)tabs.size(), theirs ? 1 : 0, mine ? 1 : 0);
            tb[sizeof(tb) - 1] = 0;
            coopLog(tb);
        }
    }
    // Requests for our characters.
    std::deque<coop::TradeReqPacket> reqs;
    g_inbound.drainTradeReqs(reqs);
    for (std::deque<coop::TradeReqPacket>::iterator it = reqs.begin(); it != reqs.end(); ++it) {
        if (it->target != g_net.localId() || it->ownerId == g_net.localId()) continue;
        it->sid[sizeof(it->sid) - 1] = '\0'; it->name[sizeof(it->name) - 1] = '\0';
        PendingTrade pt; pt.req = *it; pt.tick = now;
        g_tradeIn.push_back(pt);
        if (g_tradeIn.size() > TRADE_MAX_PENDING) answerTrade(0, false, true); // oldest goes
        char b[200];
        _snprintf(b, sizeof(b) - 1, "[trade] %s asks for '%s' x%d (req %u)",
                  playerName(it->ownerId).c_str(), it->name, (int)it->qty, (unsigned)it->reqId);
        b[sizeof(b) - 1] = '\0'; coopLog(b);
        char kb[16];
        _snprintf(kb, sizeof(kb) - 1, "F%d", g_cfg.panelKeyVk - 0x70 + 1);
        kb[sizeof(kb) - 1] = '\0';
        uiNotice(playerName(it->ownerId) + " asks for your " + it->name + " - press " + kb + " to answer");
    }
    // Answers to our requests.
    std::deque<coop::TradeRespPacket> resps;
    g_inbound.drainTradeResps(resps);
    for (std::deque<coop::TradeRespPacket>::iterator it = resps.begin(); it != resps.end(); ++it) {
        if (it->target != g_net.localId()) continue;
        char b[160];
        _snprintf(b, sizeof(b) - 1, "[trade] %s %s request %u (moved %d)",
                  playerName(it->ownerId).c_str(), it->accepted ? "accepted" : "declined",
                  (unsigned)it->reqId, (int)it->moved);
        b[sizeof(b) - 1] = '\0'; coopLog(b);
        uiNotice(playerName(it->ownerId) + (it->accepted && it->moved > 0 ? " gave you the item"
                                            : it->accepted && it->moved == -2 ? " agreed, but your character has no room"
                                            : it->accepted ? " agreed, but the item was gone"
                                                           : " declined your request"));
    }
    // Expiry + the test lever's auto-accept.
    for (size_t i = 0; i < g_tradeIn.size();) {
        DWORD age = now - g_tradeIn[i].tick;
        if (ttS > 0 && age >= 3000) { answerTrade(i, true, false); continue; }
        if (age >= TRADE_EXPIRE_MS) { answerTrade(i, false, true); continue; }
        ++i;
    }
    if (!g_peerPresent && !g_tradeIn.empty()) g_tradeIn.clear();
}

// Drain peer connect/leave events and surface a single game-thread confirmation
// per event. The net thread already logs the handshake; this proves the event
// reached the game thread cleanly (and is where later stages spawn/sweep).
void processNetEvents(GameWorld* gw) {
    std::deque<coop::u32> conns, leaves;
    g_inbound.drainConnects(conns);
    g_inbound.drainLeaves(leaves);
    for (std::deque<coop::u32>::iterator it = conns.begin(); it != conns.end(); ++it) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "handshake: peer present id=%u (local id=%u)",
                  (unsigned)*it, (unsigned)g_net.localId());
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
        g_repl.playerJoined(*it);
        // Connect-edge resync (protocol 30): re-announce placed buildings and
        // force an immediate resend pass across all change-gated channels, so
        // a late joiner / reconnector converges now instead of waiting out
        // per-channel safety resends (or never minting a pre-connect build).
        if (g_cfg.latejoinSync) g_repl.onPeerConnected(g_net, g_net.localId());
        else coopLog("[latejoin] connect edge seen, resync OFF (gate)");
        g_peers.insert(*it);
        g_peerPresent = true;
        // A join's squad tab is its player id (protocol 57: join 1 -> tab 1,
        // join 2 -> tab 2), known only once the host's WELCOME arrives.
        if (!g_cfg.isHost && !g_cfg.ownRanksFromEnv && g_net.localId() != 0) {
            std::set<unsigned int> r;
            r.insert((unsigned int)g_net.localId());
            if (r != g_cfg.ownRanks) {
                g_cfg.ownRanks = r;
                g_repl.setOwnRanks(r);
                char rb[96];
                _snprintf(rb, sizeof(rb) - 1, "[coop] player id %u -> own squad tab %u",
                          (unsigned)g_net.localId(), (unsigned)g_net.localId());
                rb[sizeof(rb) - 1] = '\0';
                coopLog(rb);
            }
        }
        g_permDirty = true; // host: send the join-permission set now (protocol 56)
        g_namesDirty = true; // host: and the player names (protocol 57)
        // Coordinated save (protocol 31): while connected under save-sync,
        // the JOIN never writes a save locally - the host's save is
        // authoritative and a local save press forwards as PKT_SAVE_REQ.
        if (!g_cfg.isHost && g_cfg.saveSync) {
            coop::engine::setSaveSuppress(true);
            coopLog("[save] JOIN save suppression ON (host save is authoritative)");
        }
        // Push-save-on-connect (host): if already in a game, bake+announce the
        // live world so the join can enter it with no pre-shared save. If the
        // host is NOT yet in-game (title/loading), the gameplay-start edge in
        // mainLoop_hook arms this instead - covers either connect ordering.
        if (g_cfg.isHost && g_cfg.saveSync && g_gameStarted)
            armConnectPush();
    }
    for (std::deque<coop::u32>::iterator it = leaves.begin(); it != leaves.end(); ++it) {
        char b[64];
        _snprintf(b, sizeof(b) - 1, "handshake: peer left id=%u", (unsigned)*it);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
        if (g_cfg.isHost && *it != coop::OWNER_ID_ALL) markAway(gw, *it);
        // Carried-body sync (protocol 18) + furniture occupancy (protocol 19):
        // the departed peer's stream will never author its drop/exit edges -
        // release any carry or occupancy its driven copies still hold.
        if (gw && (g_cfg.carrySync || g_cfg.furnSync)) g_repl.sweepCarries(gw);
        if (*it == coop::OWNER_ID_ALL) g_peers.clear();
        else                           g_peers.erase(*it);
        g_peerPresent = !g_peers.empty();
        // On a join, only the HOST going away (OWNER_ID_ALL) ends the session;
        // another join leaving (PKT_PEER_LEFT, protocol 57) does not.
        bool hostGone = !g_cfg.isHost && *it == coop::OWNER_ID_ALL;
        // Join alone again: the host's restrictions no longer apply, and a new
        // host session numbers its permission sets from 1.
        if (hostGone) {
            coop::engine::setJoinPermissions(true, true, true);
            coop::engine::setTakeFromPlayers(true);
            g_permSeenSeq = 0;
        }
        // Coordinated save: disconnected = solo again; local saves must work.
        if (hostGone && g_cfg.saveSync) {
            coop::engine::setSaveSuppress(false);
            coopLog("[save] JOIN save suppression OFF (peer left)");
        }
    }
    // Phase 2 crash hardening: a peer drop leaves this side's minted proxies
    // standing AND its drive maps pointing at bodies with no fresh authority
    // (the engine will eventually reap them, and the next drive touches a freed
    // pointer - the "join crash -> host follow-on crash" chain). Despawn the
    // minted proxies and clear the peer maps, mirroring coopUiDisconnect(). Runs
    // once per leave batch - but only when nobody is left to play with (or
    // the host went away). With 3-4 players, one leaving must not wipe the
    // others' replication state or drop their in-flight reliable events: then
    // only the leaver's own entries go (Replicator::forgetPlayer). Their ground
    // items stay when the world reloads on the next join (the host's save holds
    // them once); without load sync they would duplicate, so they go too.
    if (!leaves.empty()) {
        bool hostGone = false;
        for (std::deque<coop::u32>::iterator it = leaves.begin(); it != leaves.end(); ++it)
            if (*it == coop::OWNER_ID_ALL) hostGone = true;
        if (g_peers.empty() || hostGone) {
            g_repl.clearPeerReplicationState(gw);
            g_inbound.flushWorldState();
        } else {
            for (std::deque<coop::u32>::iterator it = leaves.begin(); it != leaves.end(); ++it)
                g_repl.forgetPlayer(gw, *it, !g_cfg.loadSync);
        }
    }
}

// SaveLoadCoordinator receive primitive (protocol 31): the join-side save-
// transfer receive pump. Drain staged BEGIN/FILE/DONE packets, verify + commit
// the streamed folder, and ACK the outcome. This identical work is needed on two
// paths - the normal saveSync receiver half (driveSaveSync) and the saveSync-
// gated-off fallback transfer (driveLoadSync) - so it lives here once instead of
// being copy-pasted into both. Emits no log line, so the oracle log contract is
// unaffected by the extraction.
void pumpSaveReceive() {
    // All three at once, so a DONE is never verified ahead of FILE chunks that
    // arrived before it (see Inbound::drainSaveTransfer).
    std::deque<coop::InboundSaveBegin> begins;
    std::deque<coop::InboundSaveFile> chunks;
    std::deque<coop::InboundSaveDone> dones;
    g_inbound.drainSaveTransfer(begins, chunks, dones);
    for (std::deque<coop::InboundSaveBegin>::iterator it = begins.begin();
         it != begins.end(); ++it)
        coop::savexfer::onSaveBegin(it->pkt);

    for (std::deque<coop::InboundSaveFile>::iterator it = chunks.begin();
         it != chunks.end(); ++it)
        coop::savexfer::onSaveFile(it->hdr, it->path.c_str(),
                                   it->data.empty() ? 0 : &it->data[0]);

    for (std::deque<coop::InboundSaveDone>::iterator it = dones.begin();
         it != dones.end(); ++it) {
        coop::u16 files = 0;
        unsigned __int64 bytes = 0;
        int ok = coop::savexfer::onSaveDone(it->hdr,
                                            it->crcs.empty() ? 0 : &it->crcs[0],
                                            &files, &bytes);
        coop::SaveAckPacket ack;
        memset(&ack, 0, sizeof(ack));
        ack.type    = (coop::u8)coop::PKT_SAVE_ACK;
        ack.ownerId = g_net.localId();
        ack.xferId  = it->hdr.xferId;
        ack.ok      = ok ? 1 : 0;
        ack.files   = files;
        ack.bytes   = bytes;
        g_net.queueSaveAck(ack);
    }
}

// Coordinated save + session resume (protocol 31), both roles, once per tick.
//
// HOST: any local save edge (menu/quicksave/autosave/programmatic - the
// SaveManager::save detour catches them all) arms the folder-quiescence
// watch; when the folder settles, the whole save folder streams to the join
// (SaveXfer sender, paced chunks); the join's ACK closes the loop. A join
// PKT_SAVE_REQ funnels into the same path via engine::saveGameAs.
//
// JOIN: local saves are suppressed while connected (the detour skips the
// engine write); a suppressed MANUAL edge forwards to the host as
// PKT_SAVE_REQ (autosave edges are dropped - the host's own autosave already
// drives a coordinated save). Received BEGIN/FILE/DONE stage + verify +
// commit the host's folder; the ACK reports the outcome.
void driveSaveSync() {
    // Local save edges from the detour (max 8 queued per tick).
    coop::engine::SaveEdge edges[8];
    unsigned int nEdges = coop::engine::drainSaveEdges(edges, 8);
    for (unsigned int i = 0; i < nEdges; ++i) {
        std::string name = edges[i].name[0] ? edges[i].name : "coopresume";
        if (g_cfg.isHost) {
            g_savePending = name;
            coop::savexfer::armWatch(name);
        } else if (edges[i].suppressed && !edges[i].autosave) {
            coop::SaveReqPacket rq;
            memset(&rq, 0, sizeof(rq));
            rq.type    = (coop::u8)coop::PKT_SAVE_REQ;
            rq.ownerId = g_net.localId();
            rq.reqId   = ++g_saveReqId;
            strncpy(rq.name, name.c_str(), sizeof(rq.name) - 1);
            g_net.queueSaveReq(rq);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "[save] REQ->host id=%u name='%s'",
                      rq.reqId, name.c_str());
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        }
    }

    if (g_cfg.isHost) {
        // Join save requests -> the ONE authoritative save (the saveGameAs
        // below re-enters the detour, whose edge arms the watch above).
        std::deque<coop::InboundSaveReq> reqs;
        g_inbound.drainSaveReqs(reqs);
        for (std::deque<coop::InboundSaveReq>::iterator it = reqs.begin();
             it != reqs.end(); ++it) {
            char name[sizeof(it->pkt.name) + 1];
            memcpy(name, it->pkt.name, sizeof(it->pkt.name));
            name[sizeof(it->pkt.name)] = '\0';
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "[save] REQ from join id=%u name='%s' -> saving",
                      it->pkt.reqId, name);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            if (!coop::engine::saveGameAs(name[0] ? name : "coopresume"))
                coopErr("[save] join-requested save FAILED to issue");
        }

        // Quiescence watch -> start the transfer once the save is on disk.
        if (coop::savexfer::watching()) {
            unsigned int files = 0;
            unsigned __int64 bytes = 0;
            unsigned long waited = 0;
            int rc = coop::savexfer::tickWatch(&files, &bytes, &waited);
            if (rc == 1 || rc == 2) {
                char b[176];
                _snprintf(b, sizeof(b) - 1,
                          "[save] QUIESCED kind=%s name='%s' files=%u bytes=%I64u waitMs=%lu",
                          rc == 1 ? "settled" : "timeout", g_savePending.c_str(),
                          files, bytes, waited);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                if (g_bootstrapArmed && g_savePending == g_bootstrapName && g_bootReloadHost) {
                    // A character was created for a new player: its id in this
                    // save differs from the live one, so the HOST loads the save
                    // too - the coordinated load sends every join the same GO.
                    g_bootReloadHost = false;
                    std::string nm = g_bootstrapName;
                    g_bootstrapArmed = false;
                    g_bootstrapName.clear();
                    g_savePending.clear();
                    coopLog("[boot] new player squad: host reloads the save so all clients share its ids");
                    if (!coop::engine::loadSave(nm)) coopErr("[boot] host reload FAILED to issue");
                } else if (g_bootstrapArmed && g_savePending == g_bootstrapName) {
                    // Connect-push: announce the freshly-baked save with a
                    // LOAD_GO instead of a blind stream. The join loads it
                    // directly if its on-disk copy matches the fingerprint;
                    // otherwise it NACKs and driveLoadSync's fallback transfer
                    // streams the folder before the join loads. Reuses the
                    // whole existing LOAD_GO/NACK/transfer/commit machinery.
                    coop::LoadGoPacket go;
                    memset(&go, 0, sizeof(go));
                    go.type        = (coop::u8)coop::PKT_LOAD_GO;
                    go.ownerId     = g_net.localId();
                    go.loadId      = ++g_loadIdOut;
                    go.fingerprint = coop::savexfer::folderFingerprint(g_bootstrapName);
                    strncpy(go.name, g_bootstrapName.c_str(), sizeof(go.name) - 1);
                    g_net.queueLoadGo(go);
                    startReadyWait(go.loadId);
                    g_loadPumpArmTick = GetTickCount();
                    char b2[192];
                    _snprintf(b2, sizeof(b2) - 1,
                              "[boot] GO->join id=%u name='%s' fp=%08x (push on connect)",
                              go.loadId, g_bootstrapName.c_str(), go.fingerprint);
                    b2[sizeof(b2) - 1] = '\0'; coopLog(b2);
                    warnIfNoPortraits(g_bootstrapName);
                    g_bootstrapArmed = false;
                    g_bootstrapName.clear();
                    g_savePending.clear();
                } else if (g_peerPresent)
                    coop::savexfer::beginSend(g_net, g_net.localId(), g_savePending);
                else
                    coopLog("[save] no peer connected; transfer skipped");
            }
        }
        // Paced chunk pump for an in-flight transfer.
        if (coop::savexfer::sending())
            coop::savexfer::tickSend(g_net, g_net.localId());

        // Join commit acknowledgements.
        std::deque<coop::InboundSaveAck> acks;
        g_inbound.drainSaveAcks(acks);
        for (std::deque<coop::InboundSaveAck>::iterator it = acks.begin();
             it != acks.end(); ++it) {
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "[save] XFER-ACK id=%u ok=%u files=%u bytes=%I64u",
                      it->pkt.xferId, (unsigned)it->pkt.ok,
                      (unsigned)it->pkt.files, it->pkt.bytes);
            b[sizeof(b) - 1] = '\0';
            if (it->pkt.ok) coopLog(b); else coopErr(b);
            coop::savexfer::noteAck(it->pkt.xferId, it->pkt.ok ? 1 : 0);
        }
    } else {
        // Receiver half: stage, verify, commit, acknowledge (shared pump).
        pumpSaveReceive();
    }
}

// Coordinated load (protocol 32), both roles, once per tick.
//
// HOST (load-authoritative, mirroring the save arbitration): any local load
// edge (menu or programmatic - the SaveManager::load detour catches both)
// broadcasts PKT_LOAD_GO with the save's folder fingerprint, then the host
// loads natively (never delayed). A join PKT_LOAD_REQ funnels into the same
// path via engine::loadSave. A join PKT_LOAD_NACK (copy missing/diverged)
// queues a protocol-31 SaveXfer of the folder, fired once the host's own
// reload has completed; the join loads after the verified commit.
//
// JOIN: local loads are suppressed while connected under the gate (the
// detour swallows the engine call); a suppressed manual edge forwards as
// PKT_LOAD_REQ. On LOAD_GO the join fingerprints its local copy: match =
// load now (bypass-once through the suppressed detour); mismatch/missing =
// NACK + a pending latch that loads after the matching transfer commits.
void driveLoadSync(GameWorld* gw) {
    // Local load edges from the detour (max 8 queued per tick).
    coop::engine::LoadEdge edges[8];
    unsigned int nEdges = coop::engine::drainLoadEdges(edges, 8);
    for (unsigned int i = 0; i < nEdges; ++i) {
        if (!edges[i].name[0]) continue;
        std::string name = edges[i].name;
        if (g_cfg.isHost) {
            // The title-screen auto-load precedes gameplay (and any peer) -
            // only a MID-SESSION load edge coordinates.
            if (!g_gameStarted) continue;
            // A load supersedes any in-flight save coordination.
            coop::savexfer::abortAll();
            g_savePending.clear();
            coop::LoadGoPacket go;
            memset(&go, 0, sizeof(go));
            go.type        = (coop::u8)coop::PKT_LOAD_GO;
            go.ownerId     = g_net.localId();
            go.loadId      = ++g_loadIdOut;
            go.fingerprint = coop::savexfer::folderFingerprint(name);
            strncpy(go.name, name.c_str(), sizeof(go.name) - 1);
            g_net.queueLoadGo(go);
            startReadyWait(go.loadId);
            g_loadPumpArmTick = GetTickCount();
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "[load] GO->join id=%u name='%s' fp=%08x",
                      go.loadId, name.c_str(), go.fingerprint);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            warnIfNoPortraits(name);
        } else if (edges[i].suppressed) {
            // Forward the swallowed manual load for host arbitration.
            coop::LoadReqPacket rq;
            memset(&rq, 0, sizeof(rq));
            rq.type    = (coop::u8)coop::PKT_LOAD_REQ;
            rq.ownerId = g_net.localId();
            rq.reqId   = ++g_loadReqId;
            strncpy(rq.name, name.c_str(), sizeof(rq.name) - 1);
            g_net.queueLoadReq(rq);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "[load] REQ->host id=%u name='%s'",
                      rq.reqId, name.c_str());
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        } else if (g_gameStarted) {
            // The join's own coordinated (bypass-once) load went through the
            // engine - arm the stalled-signal backstop for it too.
            g_loadPumpArmTick = GetTickCount();
        }
    }

    if (g_cfg.isHost) {
        // Join load requests -> the ONE authoritative load (the loadSave
        // below re-enters the detour, whose edge broadcasts the GO above).
        std::deque<coop::InboundLoadReq> reqs;
        g_inbound.drainLoadReqs(reqs);
        for (std::deque<coop::InboundLoadReq>::iterator it = reqs.begin();
             it != reqs.end(); ++it) {
            char name[sizeof(it->pkt.name) + 1];
            memcpy(name, it->pkt.name, sizeof(it->pkt.name));
            name[sizeof(it->pkt.name)] = '\0';
            if (!name[0]) continue;
            if (coop::savexfer::folderFingerprint(name) == 0) {
                char b[144];
                _snprintf(b, sizeof(b) - 1,
                          "[load] REQ from join id=%u name='%s' REFUSED (no such save here)",
                          it->pkt.reqId, name);
                b[sizeof(b) - 1] = '\0'; coopErr(b);
                continue;
            }
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "[load] REQ from join id=%u name='%s' -> loading",
                      it->pkt.reqId, name);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            if (!coop::engine::loadSave(name))
                coopErr("[load] join-requested load FAILED to issue");
        }

        // NACKs: the join can't load our save - stream it the folder once
        // OUR OWN reload has completed (never mid-swap).
        std::deque<coop::InboundLoadNack> nacks;
        g_inbound.drainLoadNacks(nacks);
        for (std::deque<coop::InboundLoadNack>::iterator it = nacks.begin();
             it != nacks.end(); ++it) {
            char name[sizeof(it->pkt.name) + 1];
            memcpy(name, it->pkt.name, sizeof(it->pkt.name));
            name[sizeof(it->pkt.name)] = '\0';
            char b[176];
            if (it->pkt.loadId != g_loadIdOut) {
                _snprintf(b, sizeof(b) - 1,
                          "[load] stale NACK id=%u (current %u) ignored",
                          it->pkt.loadId, g_loadIdOut);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                continue;
            }
            _snprintf(b, sizeof(b) - 1,
                      "[load] NACK id=%u name='%s' joinFp=%08x -> transfer after reload",
                      it->pkt.loadId, name, it->pkt.fingerprint);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            g_loadXferPending = name;
        }
        if (!g_loadXferPending.empty() && coop::engine::gameplayLive(gw) &&
            !coop::savexfer::sending()) {
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "[load] starting fallback transfer name='%s'",
                      g_loadXferPending.c_str());
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            coop::savexfer::beginSend(g_net, g_net.localId(), g_loadXferPending);
            g_loadXferPending.clear();
        }
        // The chunk pump normally lives in driveSaveSync; keep the fallback
        // transfer moving even when saveSync is gated off.
        if (!g_cfg.saveSync && coop::savexfer::sending())
            coop::savexfer::tickSend(g_net, g_net.localId());
    } else {
        // Test-only (KENSHICOOP_FORCE_STREAM=1, join): force the missing/diverged
        // NACK branch even when our on-disk fingerprint MATCHES the host's, so a
        // single-machine run (where both installs share %LOCALAPPDATA%\kenshi\save
        // and would otherwise MATCH + load from disk) still exercises the REAL
        // folder-transfer + post-transfer load path. Default OFF; read once.
        static int s_forceStream = -1;
        if (s_forceStream < 0) {
            const char* e = getenv("KENSHICOOP_FORCE_STREAM");
            s_forceStream = (e && e[0] == '1') ? 1 : 0;
            if (s_forceStream)
                coopLog("[load] FORCE-STREAM armed (test): join NACKs matching "
                        "saves to exercise the transfer");
        }

        // LOAD_GOs: verify our on-disk copy and follow the host.
        std::deque<coop::InboundLoadGo> gos;
        g_inbound.drainLoadGos(gos);
        // A GO that found the character editor open is held for a moment: the
        // engine refuses a load while edit mode is on, and closing it only takes
        // effect on a later frame - loading in the same tick was silently dropped
        // (the join stayed on the old world and the host waited out its timeout).
        static std::deque<coop::InboundLoadGo> s_heldGos;
        static DWORD s_heldTick = 0;
        if (!s_heldGos.empty()) {
            if (GetTickCount() - s_heldTick >= 2000) {
                gos.insert(gos.begin(), s_heldGos.begin(), s_heldGos.end());
                s_heldGos.clear();
            } else {
                s_heldGos.insert(s_heldGos.end(), gos.begin(), gos.end());
                gos.clear();
            }
        }
        for (std::deque<coop::InboundLoadGo>::iterator it = gos.begin();
             it != gos.end(); ++it) {
            if (it->pkt.loadId <= g_loadIdSeen) continue; // stale/duplicate
            if (closeEditorForLoad()) {
                s_heldGos.insert(s_heldGos.end(), it, gos.end());
                s_heldTick = GetTickCount();
                break;
            }
            g_loadIdSeen = it->pkt.loadId;
            char name[sizeof(it->pkt.name) + 1];
            memcpy(name, it->pkt.name, sizeof(it->pkt.name));
            name[sizeof(it->pkt.name)] = '\0';
            if (!name[0]) continue;
            coop::u32 fp = coop::savexfer::folderFingerprint(name);
            char b[192];
            if (!s_forceStream && fp != 0 && fp == it->pkt.fingerprint) {
                // Our copy is loadable as-is, so load it - ALWAYS, including
                // when this save is the one already live here. A host
                // mid-session load and a connect-push arrive as the SAME
                // packet (nothing on the wire says which), so the old "already
                // in this save, skip the hitch" arm swallowed the real reload:
                // the host swapped worlds while this side kept running the
                // pre-load one, holding its characters' damage, position and
                // inventory, and never reached sessionResetForWorldReload().
                // The cost of reloading unconditionally is one load screen when
                // two clients connect already sitting on the same save.
                _snprintf(b, sizeof(b) - 1,
                          "[load] GO id=%u name='%s' fp=%08x MATCH -> loading",
                          it->pkt.loadId, name, fp);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                warnIfNoPortraits(name);
                g_loadAfterCommit.clear();
                coop::engine::setLoadBypassOnce();
                if (!coop::engine::loadSave(name))
                    coopErr("[load] coordinated load FAILED to issue");
            } else {
                _snprintf(b, sizeof(b) - 1,
                          "[load] GO id=%u name='%s' hostFp=%08x localFp=%08x %s -> NACK (transfer)",
                          it->pkt.loadId, name, it->pkt.fingerprint, fp,
                          fp == 0 ? "MISSING" : "DIVERGED");
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                coop::LoadNackPacket nk;
                memset(&nk, 0, sizeof(nk));
                nk.type        = (coop::u8)coop::PKT_LOAD_NACK;
                nk.ownerId     = g_net.localId();
                nk.loadId      = it->pkt.loadId;
                nk.fingerprint = fp;
                strncpy(nk.name, name, sizeof(nk.name) - 1);
                g_net.queueLoadNack(nk);
                g_loadAfterCommit = name;
                g_loadCommitBase  = coop::savexfer::commitSeq();
            }
        }

        // The transfer's receive half (BEGIN/FILE/DONE -> stage/verify/
        // commit/ACK) normally lives in driveSaveSync; run it here when
        // saveSync is gated off so the fallback transfer still lands.
        if (!g_cfg.saveSync)
            pumpSaveReceive();

        // Pending latch: load once the transfer for OUR save committed.
        if (!g_loadAfterCommit.empty() &&
            coop::savexfer::commitSeq() > g_loadCommitBase) {
            if (coop::savexfer::lastCommitResult() == 1 &&
                _stricmp(coop::savexfer::lastCommitName().c_str(),
                         g_loadAfterCommit.c_str()) == 0) {
                char b[144];
                _snprintf(b, sizeof(b) - 1,
                          "[load] transfer committed -> loading '%s'",
                          g_loadAfterCommit.c_str());
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                warnIfNoPortraits(g_loadAfterCommit);
                coop::engine::setLoadBypassOnce();
                if (!coop::engine::loadSave(g_loadAfterCommit))
                    coopErr("[load] post-transfer load FAILED to issue");
                g_loadAfterCommit.clear();
            } else {
                // Some other/failed commit landed; re-base and keep waiting.
                g_loadCommitBase = coop::savexfer::commitSeq();
            }
        }
    }
}

// Co-op session panel (F2) + status banner. Interactive sessions only - the
// unattended harness (scenario / self-exit timer) never touches the panel, and
// keeping the GUI stack out of those runs avoids perturbing the scenario
// oracles. Both calls are SEH-guarded internally and touch only GUI + input, so
// they are safe wherever the GUI stack is up - neither needs a world, which is
// why this takes no GameWorld*. Driven from BOTH the in-game mainLoop_hook and
// the title-screen titleUpdate_hook so a join can go ONLINE (and copy/paste
// Steam IDs) straight from the main menu, and so the banner reports status there
// too.
void coopPanelDrive() {
    if (!(g_cfg.scenario.empty() && g_cfg.testSeconds == 0)) return;
    coop::engine::CoopPanelState ps;
    ps.selfSteamId  = (unsigned long long)coop::steamp2p::selfId();
    ps.peerSteamId  = g_cfg.steamPeer;
    ps.running      = g_net.isRunning();
    ps.peerPresent  = g_peerPresent;
    ps.isHost       = g_cfg.isHost;
    ps.transportSel = (g_cfg.transport == "steam") ? 0 : 1;
    // The last net error rides on the status line (panel AND the top-left
    // banner), so a session that is failing says why without opening a log.
    std::string lastErr = coop::NetLink::lastError();
    std::string detail;
    int ostate;
    if (g_peerPresent) {
        detail = g_cfg.isHost ? "Hosting - friends connected" : "Connected to host";
        ostate = 2;
        // Link health: ping (rounded to 10 ms so the panel does not rebuild on
        // every jitter) and reliable-packet loss when there is any. A weak link
        // turns the banner yellow - it explains rubber-banding before anyone
        // has to open a log.
        unsigned int rtt  = ((g_net.linkRttMs() + 5) / 10) * 10;
        unsigned int loss = g_net.linkLossPermille();
        char hb[64];
        if (loss >= 10)
            _snprintf(hb, sizeof(hb) - 1, "  |  ping %u ms, loss %u%%", rtt, (loss + 5) / 10);
        else
            _snprintf(hb, sizeof(hb) - 1, "  |  ping %u ms", rtt);
        hb[sizeof(hb) - 1] = '\0';
        detail += hb;
        if (rtt > 250 || loss > 50) { detail += "  (weak link)"; ostate = 1; }
    } else if (g_net.isRunning()) {
        detail = g_cfg.isHost ? "Hosting - waiting for peer..." : "Connecting...";
        if (!lastErr.empty()) detail += " (" + lastErr + ")";
        ostate = 1;
    } else {
        char kb[96];
        _snprintf(kb, sizeof(kb) - 1, "Offline - press F%d to host or join friends",
                  g_cfg.panelKeyVk - 0x70 + 1);
        kb[sizeof(kb) - 1] = '\0';
        detail = lastErr.empty() ? std::string(kb) : ("Offline - " + lastErr);
        ostate = 0;
    }
    // Join: an action the host's settings refused just now says why.
    if (g_lookEditing) {
        detail = "Customize your character - close the editor when done";
        ostate = 1;
    }
    if (!g_uiNotice.empty() && GetTickCount() - g_uiNoticeTick < UI_NOTICE_MS) {
        detail = g_uiNotice;
        ostate = 1;
    }
    if (!g_modWarn.empty() && GetTickCount() - g_modWarnTick < MODWARN_BANNER_MS) {
        detail = g_modWarn.rbegin()->second;
        ostate = 1;
    }
    if (g_cfg.isHost && !g_readyWait.empty()) {
        char wb[96];
        _snprintf(wb, sizeof(wb) - 1, "Paused - waiting for %u player(s) to finish loading",
                  (unsigned)g_readyWait.size());
        wb[sizeof(wb) - 1] = '\0';
        detail = wb;
        ostate = 1;
    }
    if (g_permNoticeTick != 0 && GetTickCount() - g_permNoticeTick < PERM_NOTICE_MS) {
        if (g_permNoticeKind == 4)
            detail = "You can only take items from your own characters";
        else if (g_permNoticeKind == 5)
            detail = "You can only control your own characters";
        else if (!g_cfg.isHost)
            detail = std::string("The host has turned off ") +
                     (g_permNoticeKind == 1 ? "building" :
                      g_permNoticeKind == 2 ? "storage containers" : "buying") +
                     " for joins";
        ostate = 1;
    }
    ps.detail = detail.c_str();

    // Join save-transfer status for the panel: while a join streams the host's
    // world at the menu (no leader -> no screen overlay), show live progress on
    // the F2 panel. The percent is whole-number so the panel only rebuilds ~100x
    // over a transfer, not every chunk. Null when not streaming.
    std::string transfer;
    if (!g_cfg.isHost && g_net.isRunning() && !g_gameStarted) {
        if (coop::savexfer::receiving()) {
            unsigned __int64 got = coop::savexfer::recvBytes();
            unsigned __int64 tot = coop::savexfer::recvTotalBytes();
            int pct = (tot > 0) ? (int)((got * 100) / tot) : 0;
            if (pct > 100) pct = 100;
            char tb[96];
            _snprintf(tb, sizeof(tb) - 1,
                      "Streaming host world... %d%% (%.1f/%.1f MB)", pct,
                      (double)got / (1024.0 * 1024.0),
                      (double)tot / (1024.0 * 1024.0));
            tb[sizeof(tb) - 1] = '\0';
            transfer = tb;
        } else if (!g_loadAfterCommit.empty()) {
            // NACK sent (host baking/streaming) or committed + about to load.
            transfer = "Preparing host world...";
        }
    }
    ps.transferDetail = transfer.empty() ? (const char*)0 : transfer.c_str();

    // Outbound Steam invite for the panel's host rows (the panel shows them only
    // to an offline HOST on STEAM, and only once the invite layer is ready).
    static coop::engine::CoopInviteApi s_invite = {
        &coop::steaminvite::ready,        &coop::steaminvite::beginInvite,
        &coop::steaminvite::inviteFriend, &coop::steaminvite::pickerActive,
        &coop::steaminvite::friendCount,  &coop::steaminvite::friendId,
        &coop::steaminvite::friendName,   &coop::steaminvite::friendState,
        &coop::steaminvite::status,
        &coop::steaminvite::hostLobby,    &coop::steaminvite::setBrowsing,
        &coop::steaminvite::refreshGameList, &coop::steaminvite::gameCount,
        &coop::steaminvite::gameName,     &coop::steaminvite::joinGame
    };
    ps.invite = &s_invite;
    ps.panelVk = g_cfg.panelKeyVk;
    ps.lastError = lastErr.c_str();
    ps.permBuild = g_permBuild; ps.permContainers = g_permContainers; ps.permBuy = g_permBuy;
    ps.permTake = g_permTake;
    ps.onPermToggle = &onPermToggle;
    // Simple view: hosting needs a loaded world; players by Steam name.
    ps.inGame = g_gameStarted;
    std::string players;
    if (g_peerPresent || g_net.isRunning()) {
        std::set<coop::u32> ids;
        ids.insert(0);
        if (g_cfg.isHost) ids.insert(g_peers.begin(), g_peers.end());
        else for (std::map<coop::u32, std::string>::const_iterator it = g_names.begin();
                  it != g_names.end(); ++it) ids.insert(it->first);
        if (g_cfg.isHost || g_peerPresent) {
            for (std::set<coop::u32>::const_iterator it = ids.begin(); it != ids.end(); ++it) {
                std::map<coop::u32, std::string>::const_iterator nm = g_names.find(*it);
                char fb[24];
                _snprintf(fb, sizeof(fb) - 1, "Player %u", (unsigned)*it);
                fb[sizeof(fb) - 1] = '\0';
                if (!players.empty()) players += ", ";
                players += (nm != g_names.end() && !nm->second.empty()) ? nm->second : std::string(fb);
                if (*it == 0) players += " (host)";
            }
        }
    }
    ps.players = players.c_str();
    loadPrefs();
    ps.autoHost = g_autoHost;
    ps.onAutoHostToggle = &onAutoHostToggle;
    loadPrefs();
    ps.newGameMoney = NG_MONEY_TEXT[g_ngMoney];
    ps.newGameStats = NG_STATS_TEXT[g_ngStats];
    ps.onNewGameCycle = &onNewGameCycle;
    ps.onEditLook  = (g_peerPresent && realSession()) ? &onEditLookRequest : 0;
    ps.lookEditing = g_lookEditing;
    ps.onResync = (g_peerPresent && realSession() && g_cfg.saveSync) ? &onResyncRequest : 0;
    ps.onReport = &onReportRequest;
    ps.tradeCount = 0;
    for (size_t ti = 0; ti < g_tradeIn.size() && ti < 4; ++ti) {
        char tb[160];
        _snprintf(tb, sizeof(tb) - 1, "%s wants %s x%d", playerName(g_tradeIn[ti].req.ownerId).c_str(),
                  g_tradeIn[ti].req.name, (int)g_tradeIn[ti].req.qty);
        tb[sizeof(tb) - 1] = '\0';
        g_tradeText[ti] = tb;
        ps.tradeText[ti] = g_tradeText[ti].c_str();
        ++ps.tradeCount;
    }
    ps.onTradeAnswer = g_peerPresent ? &onTradeAnswer : 0;
    initPingKey();
    ps.onPing  = g_peerPresent ? &onPingRequest : 0;
    ps.pingKey = g_pingKeyName;
    // Only the host pauses, and only on purpose (Space / panel button);
    // trade and dialogue windows no longer stop the game for everyone.
    coop::engine::setMultiplayerPauseRules(g_net.isRunning());
    coop::engine::notePauseKey();
    {
        float m = 0.0f; bool p = false;
        ps.gamePaused = g_lastGw && coop::engine::readGameSpeed(g_lastGw, &m, &p) && p;
    }
    ps.onPauseToggle = g_cfg.isHost ? &coop::engine::requestPauseToggle : 0;
    // Always host: once per loaded game, while offline and not on a scenario.
    if (!g_gameStarted) g_autoHostDone = false;
    if (g_autoHost && g_gameStarted && !g_autoHostDone && !g_net.isRunning()) {
        g_autoHostDone = true;
        coopLog("[coop-ui] always host: opening this game to friends");
        coopUiConnect(true, true, 0);
        coop::steaminvite::hostLobby();
    }

    // Pump Steam callbacks: an inbound "Join Game" (a friend inviting US) fires
    // coopUiConnect, and the host's lobby/friend list advance here.
    coop::steaminvite::tick();

    coop::engine::coopPanelTick(&ps, &coopUiConnect, &coopUiDisconnect);
    coop::engine::coopOverlayTick(detail.c_str(), ostate, g_net.isRunning());
}

// Main-thread tick hook: the one safe point where we touch game state.
// ---------------------------------------------------------------------------
// TickPipeline stages (Phase 3). mainLoop_hook drives these in a fixed order
// every frame; each stage was lifted VERBATIM out of the old monolithic hook,
// so per-tick behavior is unchanged - the hook now reads as an explicit
// sequence. All stages share the file-static plugin globals (g_cfg, g_repl,
// g_net, g_inbound, ...) and are defined here, just before the hook that uses
// them, so no forward declarations are needed.
// ---------------------------------------------------------------------------

// Protocol 32 world-swap edge detection. Runs FIRST so the reload edge (and,
// under load-sync, the session reset) lands before any sync code touches
// pointers from the torn-down world this tick.
void tickWorldSwapEdge(GameWorld* gw) {
    if (g_gameStarted) {
        bool live = coop::engine::gameplayLive(gw);
        if (!live && g_swapStartTick == 0) {
            g_swapStartTick = GetTickCount();
            g_swapHookTicks = 0;
            coopLog("[load] WORLD-SWAP begin (gameplay went non-live)");
        } else if (!live) {
            ++g_swapHookTicks; // probe evidence: does the hook tick during the load screen?
        } else if (g_swapStartTick != 0) {
            DWORD swapMs = GetTickCount() - g_swapStartTick;
            g_swapStartTick = 0;
            char b[160];
            if (swapMs < SWAP_MIN_MS) {
                _snprintf(b, sizeof(b) - 1,
                          "[load] WORLD-FLICKER ms=%lu (ignored, below reload threshold)",
                          (unsigned long)swapMs);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
            } else {
                _snprintf(b, sizeof(b) - 1,
                          "[load] WORLD-RELOAD swapMs=%lu hookTicksDuringSwap=%u",
                          (unsigned long)swapMs, (unsigned)g_swapHookTicks);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                // Session reset (protocol 32): centralized order (repl maps then
                // inbound queues). Peer presence + suppression levers survive.
                if (g_cfg.loadSync)
                    sessionResetForWorldReload();
                sendLoadReady();
            }
        }
    }
}

// Test-scene setup: host spawns the controlled scene ONCE, a few seconds after
// gameplay starts, then leaves the game running so the user can arrange the pose
// (e.g. seat the character) and SAVE. Baking it into a save gives the chair/NPC
// save-stable hands that resolve on both clients.
void tickSetupScene(GameWorld* gw) {
    if (!g_setupDone && !g_cfg.setupScene.empty() && g_cfg.isHost && gw &&
        g_gameStarted && (GetTickCount() - g_gameStartTick) >= SETUP_DELAY_MS) {
        g_setupDone = true;
        if (g_cfg.setupScene == "craft") {
            // Crafting/gathering (Stage 3a): spawn a work fixture + an NPC and force
            // the NPC to work it, so the host streams the work task. Both clients
            // load the baked save, so the fixture has save-stable hands on both.
            coop::engine::setupCraftScene(gw);
        } else if (g_cfg.setupScene == "down") {
            // Body-state (Stage 2) BAKE: spawn a non-squad world NPC and ragdoll it,
            // so the user can SAVE a 'down1' that both clients then load.
            coop::engine::setupDownScene(gw);
        } else if (g_cfg.setupScene == "downhold") {
            // Body-state VALIDATION: the subject is already baked into the save - do
            // NOT spawn a duplicate. The periodic re-arm below keeps it down.
            coopLog("SETUP(downhold): no spawn - re-arm keeps baked down bodies down");
        } else if (g_cfg.setupScene == "duel") {
            // Combat (Stage 3c) BAKE: spawn two PEACEFUL non-squad NPCs (same faction)
            // so the user can SAVE a neutral 'duel1' that both clients load. The fight
            // is triggered live later (combat_order) so the join sees the transition.
            bool ok = coop::engine::setupDuelScene(gw);
            coopLog(ok ? "SETUP(duel): peaceful duelists spawned - SAVE 'duel1' now"
                       : "SETUP(duel): duelist spawn FAILED");
            if (ok && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 8000; // let the duelists settle/ground
        } else if (g_cfg.setupScene == "squad") {
            // Bidirectional presence (Phase 3.5) BAKE: build a SECOND player squad tab
            // so host (tab 0) and join (tab 1) each own a tab. User SAVEs e.g. 'squad1'.
            bool ok = coop::engine::setupSquadScene(gw);
            coopLog(ok ? "SETUP(squad): second squad tab built - SAVE your two-tab save now"
                       : "SETUP(squad): squad-tab build FAILED");
        } else if (g_cfg.setupScene == "splitfar") {
            // Far-apart desync fixture BAKE ('splitfar1'): park the non-leader
            // squad tab at a MEASURED remote point and save, so split_far opens
            // with the two tabs already in two different regions instead of
            // spiralling to whatever it happens to find.
            //
            // The target must be somewhere BOTH clients see population, which
            // means BAKED population. The spot where the bug was first caught
            // (-42161,1555,-2655: join enumerated 8, host 0) is exactly the
            // wrong choice - those 8 bodies existed only in the join's process,
            // so a bake, which writes the HOST's world, records an empty field
            // and the scenario has nothing to judge. Verified: baking there
            // yielded sep=9839 with popMover=0 on both sides.
            // This target held 17 NPCs on BOTH clients for a full window
            // (run 20260802_105738, hop 2), so the crowd is in the save.
            // Override with KENSHICOOP_SPLITFAR_AT="x,y,z".
            float sx = -50446.0f, sy = 841.0f, sz = -2652.0f;
            const char* at = getenv("KENSHICOOP_SPLITFAR_AT");
            if (at && *at) sscanf(at, "%f,%f,%f", &sx, &sy, &sz);
            unsigned int nm = coop::engine::setupSplitFarScene(gw, sx, sy, sz);
            char sb[176];
            _snprintf(sb, sizeof(sb) - 1,
                      "SETUP(splitfar): relocated %u member(s) to %.0f,%.0f,%.0f - SAVE 'splitfar1' now",
                      nm, sx, sy, sz);
            sb[sizeof(sb) - 1] = '\0'; coopLog(sb);
            // Longer than the other bakes: the destination zone has to stream in
            // and the relocated bodies have to ground before the save is written.
            if (nm > 0 && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 20000;
        } else if (g_cfg.setupScene == "bedcage") {
            // Bed+cage occupancy (protocol 19) BAKE: spawn a bed and a prison
            // cage near the leader so both clients load save-stable furniture
            // hands. With KENSHICOOP_BAKESAVE set the save is written
            // automatically a few seconds later (no manual menu round-trip).
            bool ok = coop::engine::setupBedCageScene(gw);
            coopLog(ok ? "SETUP(bedcage): bed + cage spawned - SAVE 'bedcage1' now"
                       : "SETUP(bedcage): spawn FAILED");
            if (ok && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 8000; // let physics/grounding settle
        } else if (g_cfg.setupScene == "pole") {
            // Prisoner-pole occupancy (protocol 19 kind=4) BAKE: spawn one
            // standing prisoner POLE in front of the leader so both clients load
            // a save-stable pole hand. The pole_put controlled test then KOs a PC
            // and setPrisonMode's it onto the pole (visibly a body ON A POLE, not
            // in a cage). With KENSHICOOP_BAKESAVE the save writes automatically.
            bool ok = coop::engine::setupPoleScene(gw);
            coopLog(ok ? "SETUP(pole): prisoner pole spawned - SAVE 'pole1' now"
                       : "SETUP(pole): spawn FAILED (no pole template? see candidates)");
            if (ok && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 8000; // let physics/grounding settle
        } else if (g_cfg.setupScene == "buffpc") {
            // Stat buff BAKE (single client): raise EVERY player-squad PC to 120 in
            // all stats, then leave the game running so the user can SAVE manually
            // (or auto-bake if KENSHICOOP_BAKESAVE is set). No coop peer required.
            unsigned int nb = coop::engine::buffAllPlayerStats(gw, 120.0f);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SETUP(buffpc): buffed %u PC(s) to 120 in every stat - SAVE now", nb);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            if (nb > 0 && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 4000; // let the recalc settle
        } else if (g_cfg.setupScene == "inventory") {
            // Inventory (Phase 4a) BAKE: spawn a save-stable storage container in front
            // of the leader + seed it with items so both clients load an identical
            // container with a resolvable hand. User SAVEs e.g. 'inv1'.
            unsigned int ch[5] = { 0, 0, 0, 0, 0 };
            bool ok = coop::engine::setupInventoryScene(gw, ch);
            if (ok) { char b[160]; _snprintf(b, sizeof(b) - 1,
                "SETUP(inventory): container hand=%u,%u,%u,%u,%u - SAVE 'inv1' now",
                ch[0], ch[1], ch[2], ch[3], ch[4]); b[sizeof(b) - 1] = '\0'; coopLog(b); }
            else coopLog("SETUP(inventory): container prep FAILED");
        } else {
            RootObject* seat = 0;
            bool ok = coop::engine::spawnSeatInFront(gw, 7.0f, 0.0f, &seat);
            if (ok && seat) {
                unsigned int h[5];
                if (coop::engine::readObjectHand(seat, h))
                    { char b[160]; _snprintf(b, sizeof(b)-1,
                        "SETUP: spawned seat hand=%u,%u,%u,%u,%u",
                        h[3], h[4], h[0], h[1], h[2]); b[sizeof(b)-1]='\0'; coopLog(b); }
                else coopLog("SETUP: spawned seat (hand unread)");
            } else {
                coopLog("SETUP: seat spawn FAILED (no seat template or createBuilding faulted)");
            }
            if (g_cfg.setupScene == "npc") {
                Character* npc = coop::engine::spawnNpcInFront(gw, 2.5f, 1.0f);
                coopLog(npc ? "SETUP: spawned world NPC" : "SETUP: NPC spawn FAILED");
            }
        }
        coopLog("SETUP: scene ready - arrange the pose and SAVE the game now");
    }
}

// Deferred auto-bake: write the fixture save once the armed settle window
// elapses. One-shot; the self-exit timer (KENSHICOOP_TEST_SECONDS) then ends
// the bake run.
void tickAutoBake() {
    if (g_bakeSaveTick != 0 && GetTickCount() >= g_bakeSaveTick) {
        g_bakeSaveTick = 0;
        bool ok = coop::engine::saveGameAs(g_cfg.bakeSave);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SETUP: auto-bake save '%s' %s",
                  g_cfg.bakeSave.c_str(), ok ? "ISSUED" : "FAILED");
        b[sizeof(b) - 1] = '\0'; coopLog(b);
    }
}

// Host re-arm + inventory-owner registration. Baked work goals / ragdoll state
// do not survive save/load and drift over time, so re-issue them on an interval;
// and (re)resolve the owned container so publishInventories streams its contents.
void tickHostRearm(GameWorld* gw) {
    // Craft re-arm (host): a baked work goal does not persist across save/load, and a
    // world worker's own AI can drift off-station. Re-issue the work goal on an
    // interval so the host keeps streaming the work task throughout the scene. The
    // call is throttled and no-ops when the worker is already on task, so it never
    // thrashes the worker's pathing. Only the 'craft' scene arms this.
    if (g_cfg.setupScene == "craft" && g_cfg.isHost && gw && g_gameStarted &&
        (GetTickCount() - g_lastCraftRearmTick) >= CRAFT_REARM_MS) {
        g_lastCraftRearmTick = GetTickCount();
        coop::engine::rearmCraftScene(gw);
    }

    // Down re-arm (host): a healthy ragdolled body recovers and stands back up, and
    // ragdoll state does not survive save/load. Re-knock the nearby non-squad bodies
    // on an interval so they stay on the ground throughout the scene. Both the bake
    // ('down') and the spawn-free validation ('downhold') arm this.
    if ((g_cfg.setupScene == "down" || g_cfg.setupScene == "downhold") &&
        g_cfg.isHost && gw && g_gameStarted &&
        (GetTickCount() - g_lastCraftRearmTick) >= CRAFT_REARM_MS) {
        g_lastCraftRearmTick = GetTickCount();
        coop::engine::rearmDownScene(gw);
    }

    // Inventory owner registration (host, Phase 4a): when inventory sync is active,
    // (re)resolve the baked storage container nearest the leader (else the leader's own
    // inventory) and register it as the owned container so publishInventories streams
    // its contents. Re-resolved on an interval because the container hand only resolves
    // once the world block has loaded. The join never registers (it reconciles).
    if (g_cfg.invSync && g_cfg.isHost && gw && g_gameStarted) {
        static DWORD lastInvReg = 0;
        if (GetTickCount() - lastInvReg >= (DWORD)CRAFT_REARM_MS) {
            lastInvReg = GetTickCount();
            unsigned int ch[5];
            if (coop::engine::pickInventoryContainer(gw, ch))
                g_repl.setOwnedContainerHand(ch);
        }
    }
}

// Scenario completion hold (harness): once a verdict is logged, keep driving the
// synced bodies on screen for the capture window, then self-exit cleanly.
void tickScenarioHold() {
#ifdef KENSHICOOP_HARNESS
    if (g_scenario && g_scenarioDoneTick != 0) {
        if (GetTickCount() - g_scenarioDoneTick >= SCENARIO_HOLD_MS) {
            coop::logClose();
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
#endif
}

// Phase 6: build the per-tick channel call environment (SyncContext) from the
// long-lived globals. A cheap POD rebuilt per call, so localId() is read at the
// same point it always was (byte-identical to the old loose-arg calls).
static coop::SyncContext replCtx(GameWorld* gw) {
    coop::SyncContext c;
    c.gw = gw; c.net = &g_net; c.in = &g_inbound;
    c.localId = g_net.localId(); c.isHost = g_cfg.isHost;
    return c;
}

// Replication publish (pre-engine). BIDIRECTIONAL presence: both clients stream
// their OWNED squad subset (partitioned by hand-rank) and drive the peer's; the
// host additionally streams world NPCs. Ingest received targets BEFORE the engine
// tick so apply targets are current. worldLive is sampled ONCE pre-engine by the
// caller (see the comment there); every channel is gated by its own Config knob.
void tickReplicatePublish(GameWorld* gw, bool worldLive) {
    if (worldLive) {
        KC_PERF(S_PUB_INGEST, g_repl.ingest(g_inbound));
        // Phase 4a: drain received container-contents snapshots into the per-container
        // cache (reconciled after the engine tick by applyInventories).
        KC_PERF(S_PUB_INGEST, g_repl.ingestInv(g_inbound));
        // Both clients latch reliable transition events (KO/death/revive) for the bodies
        // they drive, before apply (a side can emit an event for its own owned body and
        // the peer that drives that body must honour it).
        KC_PERF(S_PUB_INGEST, g_repl.applyEvents(gw, g_inbound));
    }
    if (worldLive) {
        KC_PERF(S_PUB_OWNED, g_repl.publishOwned(gw, g_net, g_net.localId()));
        // Phase W2: BOTH clients watch their OWNED characters for a WEAPON drop and author a
        // reliable conservation intent so the peer relocates its own copy of that weapon (a
        // weapon can't be rebuilt via the W1 proxy path). Bidirectional; gated on worldSync.
        // Ordered BEFORE publishInventories: the gear census is what tells the inventory
        // publisher a drop is still being adjudicated (wdPendingDrop), and running it first
        // means that signal is current rather than one tick stale. Otherwise a bag snapshot
        // can be authored ahead of the intent that explains it, and the peer's reconcile
        // destroys the copy the intent was going to relocate.
        if (g_cfg.worldSync)
            KC_PERF(S_PUB_WEAPONS, g_repl.detectAndPublishWeaponDrops(gw, g_net, g_net.localId()));
        // Phase W3 convergence: prune ground-gear tracks whose object is gone (a stale
        // Item* is how a re-home silently no-ops, leaving the author's copy on the ground
        // as a duplicate) and finish any re-home the peer's pickup could not complete yet.
        // Ordered before publishInventories so a completed re-home ships in THIS tick's
        // snapshot; it only ever touches PEER-owned containers, so it cannot feed the
        // owned-character census above and loop.
        if (g_cfg.worldSync)
            KC_PERF(S_PUB_WEAPONS, g_repl.reconcileGroundGear(gw));
        // Both clients stream the contents of every squad member they OWN (host tab 0,
        // join tab 1) on content-change - bidirectional, disjoint by the same tab
        // partition as positional sync. Gated on invSync so ordinary co-op sessions add
        // no inventory traffic; the peer reconciles via applyInventories (skips own).
        if (g_cfg.invSync)
            KC_PERF(S_PUB_INV, g_repl.publishInventories(gw, g_net, g_net.localId()));
        // Protocol 37: BOTH clients diff every tracked container (own + received)
        // against its baseline to catch a completed cross-owner UI drag - the one
        // inventory write the single-writer snapshots cannot represent - and author
        // a reliable PKT_INV_XFER so the peer relocates its own copy (conservation).
        // RETIRED by the trade veto (blockXfer): a refused drag can never complete,
        // so there is nothing to detect/replicate - Config forces xferSync off when
        // blockXfer is on, making this (and applyTransfers below) a no-op. The
        // xferLatch_/xferDefer_ reconcile-race machinery then stays dormant (never
        // populated). KENSHICOOP_BLOCK_XFER=0 restores this replicate-the-trade path.
        if (g_cfg.xferSync)
            KC_PERF(S_PUB_XFER, g_repl.detectAndPublishTransfers(gw, g_net, g_net.localId()));
        // Phase W1 (bidirectional): BOTH clients stream the free ground items they
        // author in their interest sphere - owner-scoped netId spaces, peer items
        // filtered by the proxy echo guard - so a join-side drop of materials/food
        // finally appears on the host. Proxies reconcile after the engine tick
        // (applyWorldItems, also both sides now).
        if (g_cfg.worldSync)
            KC_PERF(S_PUB_WORLD, g_repl.publishWorldItems(gw, g_net, g_net.localId()));
        // Phase 2 (player combat + medical): owner-authoritative vitals sync for
        // player-squad members, both directions. publishMedical streams OUR
        // members' medical model (change-gated, reliable); applyMedical writes
        // received snapshots onto the peer copies we drive AND forwards any
        // local first aid administered on those copies back to their owner;
        // applyTreatments lands forwarded first aid on the bodies we own.
        // Ordered after publishOwned (they use the ownHands_ set it refreshes).
        if (g_cfg.medSync) {
            KC_PERF(S_MEDICAL, g_repl.publishMedical(gw, g_net, g_net.localId()));
            KC_PERF(S_MEDICAL, g_repl.applyMedical(gw, g_inbound, g_net, g_net.localId()));
            KC_PERF(S_MEDICAL, g_repl.applyTreatments(gw, g_inbound));
            // Join-dealt authoritative damage (protocol 45): the JOIN forwards the
            // damage its guarded melee would have dealt to driven world-NPC copies;
            // the HOST applies it to the real body (blood + a frontal flesh wound),
            // which the vitals stream then mirrors back. Decouples the join PC's
            // damage from its disrupted (position-driven) copy animation.
            if (g_cfg.isHost)
                KC_PERF(S_MEDICAL, g_repl.applyCombatHits(gw, g_inbound));
            else
                KC_PERF(S_MEDICAL, g_repl.publishCombatHits(gw, g_net, g_net.localId()));
        }
        // Character stats sync (protocol 17): owner-authoritative CharStats
        // stream for player-squad members, both directions. publishStats
        // streams OUR members' stats (change-gated, reliable); applyStats
        // writes received snapshots onto the peer copies we drive. Ordered
        // after publishOwned (they use the ownHands_ set it refreshes).
        if (g_cfg.statsSync) {
            KC_PERF(S_STATS, g_repl.publishStats(gw, g_net, g_net.localId()));
            KC_PERF(S_STATS, g_repl.applyStats(gw, g_inbound));
        }
        // Shared money pool (protocol 52): Kenshi keeps ONE player wallet, so
        // both players spend from one pool with the host as authority. Publish
        // samples the wallet and reports any local movement (join: a signed
        // delta, host: the authoritative total); apply folds deltas / adopts the
        // total. Ordered after publishOwned so isHostRole() reflects this tick's
        // ownership partition.
        if (g_cfg.moneySync) {
            KC_PERF(S_MONEY, g_repl.publishMoneyPool(replCtx(gw)));
            KC_PERF(S_MONEY, g_repl.applyMoneyPool(replCtx(gw)));
        }
        // Recruitment sync (protocol 23): drain the recruit detour's edge queue
        // into reliable EVT_RECRUIT events (subject = old hand, actor = new
        // hand) and pin recruited hands to their recruiter's ownership. The
        // receive half (re-key) lives in applyEvents above. Ordered after
        // publishOwned so this tick's recruits pin BEFORE next tick's census.
    if (g_cfg.recruitSync)
        KC_PERF(S_SQUAD, g_repl.publishRecruits(gw, g_net, g_net.localId()));

    // Squad management sync (protocol 35): poll the roster's pointer->hand
    // baseline (~2 Hz; a squad-tab move re-containers the body but the
    // Character* survives) and author reliable EVT_SQUAD_MOVE re-key edges,
    // pinning moved hands to the mover's ownership. The receive half (shared
    // EVT_RECRUIT re-key path) lives in applyEvents above.
    if (g_cfg.squadSync)
        KC_PERF(S_SQUAD, g_repl.publishSquadMoves(gw, g_net, g_net.localId()));

    // Change-gated sampled channels (Phase 6c): faction relations (protocol 24),
    // baked doors (26), placed buildings (27), placed-building doors (28),
    // production machines (33), and research (38). Each is a self-contained
    // publish/apply pair sharing the ChangeGate policy + SyncContext; the
    // channel-descriptor registry inside driveSampledChannels owns their
    // per-channel enable, direction (symmetric detector vs host-authoritative),
    // and cadence order, so adding a sampled channel is one table row there -
    // not an edit to this tick. Ordered after recruit/squad (ownership pins)
    // and before stealth/speed/time, exactly as the explicit blocks were.
    KC_PERF(S_SAMPLED, g_repl.driveSampledChannels(replCtx(gw)));
        // Stealth sync (protocol 20): the HOST is the world-detection authority
        // - it streams each DRIVEN sneaker's whoSeesMeSneaking back to the
        // sneaker's owner; every client replays received snapshots onto the
        // bodies it OWNS (the indicators render on the owner's screen). The
        // posture half lives inside applyTargets (continuous BODY_SNEAK apply).
        if (g_cfg.stealthSync) {
            if (g_cfg.isHost)
                KC_PERF(S_STEALTH, g_repl.publishStealth(gw, g_net, g_net.localId()));
            KC_PERF(S_STEALTH, g_repl.applyStealthFeedback(gw, g_inbound));
        }
        // Consensus game-speed sync: detect local speed clicks as REQUESTS,
        // host arbitrates effective = min(requests) (capped at 1x while either
        // player squad fights) and broadcasts; the join applies the SET. Runs
        // after publishOwned (the combat flag samples the ownHands_ set).
        if (g_cfg.speedSync)
            KC_PERF(S_SPEED, g_repl.syncSpeed(gw, g_inbound, g_net, g_net.localId(), g_cfg.isHost));
        // Phase 6 (6a evidence spike): env-gated ([shackledbg]) per-character
        // shackle/lock trace. No-op unless KENSHICOOP_DEBUG_SHACKLE=1, so it is
        // free to leave in the tick for manual-session characterization.
        coop::engine::shackleDbgTick(gw, g_cfg.isHost);
        // Game-clock sync (protocol 25): the host broadcasts its absolute
        // in-game clock ~1 Hz; the join measures the offset and SLEWS - a
        // multiplier the speed layer's quiet writes fold in on top of the
        // arbitrated consensus effective. AFTER syncSpeed so a slew change
        // applies against this tick's consensus state.
        if (g_cfg.timeSync)
            KC_PERF(S_TIME, g_repl.syncTime(gw, g_inbound, g_net, g_net.localId(), g_cfg.isHost));
        // Runtime-spawn proxy replication (protocol 21): the join asks about
        // streamed hands it couldn't resolve last tick (host RUNTIME spawns)
        // and mints local proxy bodies from the host's replies; the host
        // answers requests. BEFORE the engine tick so a proxy bound this
        // frame is driven by this frame's applyTargets.
        if (g_cfg.spawnSync)
            KC_PERF(S_SPAWN, g_repl.syncSpawns(gw, g_inbound, g_net, g_net.localId(), g_cfg.isHost));
    }
}

// Coordinated save + load (protocols 31/32) run under g_gameStarted, NOT
// worldLive: they must keep pumping DURING a world swap (the join drains the
// host's LOAD_GO and issues its own load; the host's fallback transfer and the
// deferred-signal state advance) and they only touch net packets, file IO and
// the load/save ENTRY points - never a cached world pointer the swap invalidates.
void tickCoordinatedSaveLoad(GameWorld* gw) {
    if (g_gameStarted) {
        // Coordinated save + session resume (protocol 31): host-arbitrated
        // save edges, folder-quiescence completion, paced in-band folder
        // transfer, staged+verified commit on the join.
        if (g_cfg.saveSync)
            driveSaveSync();
        // Coordinated load (protocol 32): host-arbitrated load edges,
        // fingerprint-verified join follow, SaveXfer fallback on divergence.
        if (g_cfg.loadSync)
            driveLoadSync(gw);
    }
}

// Scenario onStart (harness) fires once, BEFORE the engine tick, so a host-issued
// move order takes effect this frame.
//
// PEER-READY ARMING: the scenario clock does NOT start at gameplay start - it
// starts when this client first receives a peer's owned-entity batch
// (Inbound::sawRemoteEntity). On the HOST that flips true exactly when the JOIN
// is loaded + streaming, so every scripted host action (orders, kills, item adds
// - all timed off ctx.elapsedMs) happens with the join watching, instead of
// racing its load screen. On the JOIN the host's stream is already arriving by
// the time it reaches gameplay, so it arms ~immediately - which also aligns both
// clients' scenario clocks to roughly the same wall moment. Fallback: arm anyway
// after scenarioArmTimeoutMs of gameplay (peer never connected / host-only
// diagnostics); 0 = arm immediately (spike runs).
void tickScenarioStart(GameWorld* gw) {
#ifdef KENSHICOOP_HARNESS
    if (g_scenario && g_gameStarted && gw && !g_scenarioStarted) {
        bool  peerReady = g_inbound.sawRemoteEntity();
        DWORD waitedMs  = GetTickCount() - g_gameStartTick;
        bool  fallback  = (g_cfg.scenarioArmTimeoutMs == 0) ||
                          (waitedMs >= (DWORD)g_cfg.scenarioArmTimeoutMs);
        // Pre-arm phase: every tick until arming, let the scenario pin/hold its
        // subjects while the freshly-loaded world is still in its baked pose
        // (waiting a join-load before pinning loses wandering NPCs).
        {
            coop::ScenarioContext pctx;
            pctx.gw = gw; pctx.isHost = g_cfg.isHost; pctx.localId = g_net.localId();
            pctx.elapsedMs = waitedMs; pctx.tick = g_scenarioTick;
            pctx.peerReady = peerReady;
            pctx.pickMintedProxy = &coopScenarioPickMintedProxy;
            g_scenario->onGameplay(pctx);
        }
        if (peerReady || fallback) {
            g_scenarioStarted   = true;
            g_scenarioStartTick = GetTickCount();
            coop::ScenarioContext ctx;
            ctx.gw = gw; ctx.isHost = g_cfg.isHost; ctx.localId = g_net.localId();
            ctx.elapsedMs = 0; ctx.tick = g_scenarioTick;
            ctx.peerReady = peerReady;
            ctx.pickMintedProxy = &coopScenarioPickMintedProxy;
            char m[200];
            _snprintf(m, sizeof(m) - 1, "SCENARIO arm trigger=%s waitedMs=%lu",
                      peerReady ? "peer-ready" : "timeout", (unsigned long)waitedMs);
            m[sizeof(m) - 1] = '\0';
            coopLog(m);
            _snprintf(m, sizeof(m) - 1, "SCENARIO %s start", g_scenario->name());
            m[sizeof(m) - 1] = '\0';
            coopLog(m);
            g_scenario->onStart(ctx);
        }
    }
#endif // KENSHICOOP_HARNESS
}

// Replication apply (post-engine) so our transform is the last word the renderer
// samples (the local AI re-decides at the start of the next tick). Both clients
// drive the PEER's owned bodies; applyTargets skips our OWN owned hands. Re-sample
// live: the engine tick may have STARTED a swap (gameplay just went non-live), in
// which case the caches are now stale - skip apply this tick too (the reset runs
// next tick's top on the reload edge).
void trackMove(GameWorld* gw);   // KENSHICOOP_TRACK_MOVE route recorder (below)

// While a player is in the character editor their squad cannot move or fight
// back, so it is frozen and melee-proof (the away guards) until the editor
// closes. Vitals are owner-authoritative, so guarding on the editing client is
// what counts. Rebuilt every tick after applyTargets, like the away guards.
void applyLookGuard(GameWorld* gw) {
    static bool s_logged = false;
    if (!g_lookEditing || !gw) { s_logged = false; return; }
    coop::EntityState sq[64];
    unsigned int n = coop::engine::captureSquad(gw, /*leaderOnly*/ false, sq, 64);
    unsigned int guarded = 0;
    for (unsigned int i = 0; i < n; ++i) {
        if (sq[i].hContainer != g_lookHand[1] || sq[i].hContainerSerial != g_lookHand[2]) continue;
        Character* c = coop::engine::resolveCharByHand(sq[i].hIndex, sq[i].hSerial, sq[i].hType,
                                                       sq[i].hContainer, sq[i].hContainerSerial);
        if (!c) continue;
        coop::engine::addAiSuspend(c);
        coop::engine::addDamageGuard(c);
        ++guarded;
    }
    if (!s_logged) {
        s_logged = true;
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[look] guarding %u character(s) while the editor is open", guarded);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
}

void tickReplicateApply(GameWorld* gw, bool worldLive) {
    if (worldLive && coop::engine::gameplayLive(gw)) {
        KC_PERF(S_APPLY_TARGETS, g_repl.applyTargets(gw));
        applyAwayGuards(gw); // after applyTargets rebuilt the suspend/guard sets
        applyLookGuard(gw);
        // Phase W2: relocate our own copy of any DROPPED weapon to the ground BEFORE the
        // inventory reconcile runs, so the conservation move beats the (debounced) removal
        // reconcile that would otherwise DESTROY the weapon we cannot refabricate.
        if (g_cfg.worldSync) {
            KC_PERF(S_APPLY_WEAPONS, g_repl.applyWeaponDrops(gw, g_inbound));
            // Phase W3: re-home tracked ground copies into the picking character's bag, also
            // before the inventory reconcile (which can't refabricate a weapon into the proxy).
            KC_PERF(S_APPLY_WEAPONS, g_repl.applyWeaponPickups(gw, g_inbound));
        }
        // Protocol 37: relocate our copy of any cross-owner TRADED item between the
        // two containers BEFORE the inventory reconcile, so the conservation move
        // beats the stale-snapshot dupe/wipe (and traded gear survives - no
        // fabrication on this path).
        if (g_cfg.xferSync) {
            KC_PERF(S_APPLY_XFER, g_repl.applyTransfers(gw, g_inbound, g_net, g_net.localId()));
            // Protocol 50: settle our own outstanding intents on the receiver's
            // verdict. Before the reconcile, because a rejected transfer works
            // by DROPPING our latch and letting applyInventories restore the
            // owner's version - one tick later and the reconcile would run once
            // more while still defending a move that was refused.
            KC_PERF(S_APPLY_XFER, g_repl.applyXferAcks(gw, g_inbound, g_net.localId()));
        }
        // Phase 4a: reconcile any peer-owned container we received a fresh snapshot
        // for (the join applies the host's container; the host skips its own).
        KC_PERF(S_APPLY_INV, g_repl.applyInventories(gw));
        // Phase W1 (bidirectional): BOTH clients spawn/update/cull local proxies for
        // the peer's streamed ground items, keyed (ownerId, netId) so the per-sender
        // netId spaces never collide.
        if (g_cfg.worldSync) {
            KC_PERF(S_APPLY_WORLD, g_repl.applyWorldItems(gw, g_inbound));
            // Protocol 47: a peer consumed a proxy it held for one of OUR ground items,
            // so destroy our real copy. Runs AFTER applyWorldItems so a snapshot and the
            // claim that retires it in the same batch resolve in authored order.
            KC_PERF(S_APPLY_WORLD, g_repl.applyWorldClaims(gw, g_inbound, g_net.localId()));
        }
        // Host-authoritative world: only the JOIN hides/freezes any local NPC the
        // host isn't streaming (so the join can't run a divergent copy). The host IS
        // the world authority, so it never suppresses.
        // NPC existence census (protocol 36): the host broadcasts its 1 Hz
        // wide-radius hand list; the join drains it and lets the wide-radius
        // pass in enforceHostAuthority cull local-only ghosts at render range.
        // Under presence authority both halves run on BOTH sides: each client
        // authors the cells it stands in and judges its local copies of the
        // other's. With cellAuth off the two conditions collapse to today's
        // exclusive split, which is what makes the fail-open A/B meaningful.
        if (!g_cfg.isHost || g_cfg.cellAuth) {
            KC_PERF(S_CENSUS, g_repl.applyNpcCensus(gw, g_inbound));
            KC_PERF(S_CENSUS, g_repl.enforceHostAuthority(gw, g_net.localId()));
        }
        if (g_cfg.isHost || g_cfg.cellAuth) {
            KC_PERF(S_CENSUS, g_repl.publishNpcCensus(gw, g_net, g_net.localId()));
        }
        // Camera-anchored interest (protocol 43): both sides publish their
        // LOCAL camera to the engine's anchor store, ship its center to the
        // peer at ~1 Hz, and fold a fresh peer hint into interestCenters.
        // Runs even with CAM_INTEREST off (the engine-side knob makes
        // interestCenters ignore the anchors) so an A/B toggle needs no
        // session restart logic.
        KC_PERF(S_CAM_CELL, g_repl.syncCamHint(gw, g_inbound, g_net, g_net.localId()));
        KC_PERF(S_CAM_CELL, g_repl.syncCellClaims(gw, g_inbound, g_net, g_net.localId()));
        trackMove(gw);
    }
}

// KENSHICOOP_TRACK_MOVE=1: log one line per player squad TAB per second - where
// its first member is and whether it is moving. Log-only; gates nothing.
//
// It exists because the cell claims are far too sparse to reconstruct a walked
// route. A claim fires when a tab crosses a 4608 u cell boundary, so a 99,600 u
// crossing left 29 points, and run_apart's attempt to walk between consecutive
// ones wedged the pair at -48980,-5670: a straight line between two cell-crossing
// points is not a path, because the human who produced them walked AROUND
// whatever sits between. A 1 Hz track is ~500 points over the same crossing,
// which is a route, and it also shows plainly whether a single distant click
// routes across the map at all - the question run_apart is currently guessing at.
void trackMove(GameWorld* gw) {
    if (!g_cfg.trackMove || !gw) return;
    static DWORD lastTick = 0;
    const DWORD now = GetTickCount();
    if (lastTick != 0 && (now - lastTick) < 1000) return;
    lastTick = now;

    const unsigned int MAXB = 32;
    coop::EntityState sq[MAXB];
    unsigned int n = coop::engine::captureSquad(gw, false, sq, MAXB);
    // One line per tab, keyed on the container hand: every squad member would be
    // six lines a second of the same information.
    unsigned int seenC[8], seenS[8];
    unsigned int nSeen = 0;
    for (unsigned int i = 0; i < n && nSeen < 8; ++i) {
        bool dup = false;
        for (unsigned int k = 0; k < nSeen; ++k)
            if (seenC[k] == (unsigned int)sq[i].hContainer &&
                seenS[k] == (unsigned int)sq[i].hContainerSerial)
                dup = true;
        if (dup) continue;
        seenC[nSeen] = (unsigned int)sq[i].hContainer;
        seenS[nSeen] = (unsigned int)sq[i].hContainerSerial;
        ++nSeen;
        int cx = 0, cz = 0;
        int haveCell = coop::engine::cellAt(gw, sq[i].x, sq[i].z, &cx, &cz) ? 1 : 0;
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "[track] tab=%u,%u pos=%.0f,%.0f,%.0f moving=%u speed=%.1f "
                  "cell=%d(%d,%d)",
                  (unsigned)sq[i].hContainer, (unsigned)sq[i].hContainerSerial,
                  sq[i].x, sq[i].y, sq[i].z, (unsigned)sq[i].cMoving, sq[i].cSpeed,
                  haveCell, cx, cz);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }
}

// Coordinated load (protocol 32): deferred-signal backstop. The engine usually
// consumes the LOADGAME signal on its own within ~0.5 s (load_probe run 2), but
// run 1 saw it sit forever - if the swap hasn't begun after the grace window,
// pump SaveManager::execute() once from this end-of-tick context (the engine tick
// above is done with the world).
void tickLoadPumpBackstop(GameWorld* gw) {
    if (g_loadPumpArmTick != 0 && g_cfg.loadSync) {
        if (g_swapStartTick != 0) {
            g_loadPumpArmTick = 0; // the swap started on its own
        } else if (GetTickCount() - g_loadPumpArmTick >= LOAD_PUMP_GRACE_MS) {
            g_loadPumpArmTick = 0;
            int delay = -1;
            int sig = coop::engine::saveMgrSignal(&delay);
            if (sig == 2 /*LOADGAME*/) {
                bool ok = coop::engine::saveMgrExecute();
                int sigAfter = coop::engine::saveMgrSignal(0);
                char b[144];
                _snprintf(b, sizeof(b) - 1,
                          "[load] EXEC pumped (deferred LOADGAME stalled) ok=%d sigAfter=%d",
                          ok ? 1 : 0, sigAfter);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                // If execute() swapped the world SYNCHRONOUSLY (signal consumed
                // and we're still live), the reload-edge detector never saw a
                // non-live frame, so it won't run the session reset - force it
                // here. The caches from the OLD world are dangling right now.
                if (ok && sigAfter != 2 && g_swapStartTick == 0 &&
                    coop::engine::gameplayLive(gw)) {
                    coopLog("[load] synchronous execute swap - forcing session reset");
                    sessionResetForWorldReload();
                }
            }
        }
    } else if (g_loadPumpArmTick != 0) {
        g_loadPumpArmTick = 0;
    }
}

// Scenario onTick (harness) AFTER apply, so a join's RECV line reflects the
// applied pos; on completion it emits the smoothness summary + verdict and begins
// the capture hold.
void tickScenarioTick(GameWorld* gw) {
#ifdef KENSHICOOP_HARNESS
    if (g_scenario && g_gameStarted && gw && g_scenarioStarted && g_scenarioDoneTick == 0) {
        coop::ScenarioContext ctx;
        ctx.gw = gw; ctx.isHost = g_cfg.isHost; ctx.localId = g_net.localId();
        ctx.elapsedMs = GetTickCount() - g_scenarioStartTick;
        ctx.tick = ++g_scenarioTick;
        ctx.peerReady = g_inbound.sawRemoteEntity();
        ctx.pickMintedProxy = &coopScenarioPickMintedProxy;
        if (g_scenario->onTick(ctx)) {
            // Stage 2: the receiver emits its interpolation smoothness summary
            // alongside the verdict so the runner can assert per-frame gliding.
            if (!g_cfg.isHost) g_repl.logSmoothSummary();
            bool ok = g_scenario->passed();
            char m[48];
            _snprintf(m, sizeof(m) - 1, "SCENARIO RESULT %s", ok ? "PASS" : "FAIL");
            m[sizeof(m) - 1] = '\0';
            coopLog(m);
            g_scenarioDoneTick = GetTickCount(); // begin the capture hold
        }
    }
#endif // KENSHICOOP_HARNESS
}

void mainLoop_hook(GameWorld* gw, float dt) {
    const unsigned __int64 perfHookT0 = g_perfWindowMs ? perfNow() : 0;
    ++g_tick;
    g_lastGw = gw; // cache for the argument-less F2 UI callbacks
    coop::engine::noteMainThread();
    coop::engine::probeThread(46, "mainLoop_hook");

#ifdef KENSHICOOP_HARNESS
    // TEST-ONLY (KENSHICOOP_TEST_CRASH=<seconds>): fault on purpose, to prove the
    // crash-dump filter actually produces a dump with our frames in it. A crash
    // handler nobody has ever seen fire is a coin flip, and the run it would waste
    // is a human reproducing a rare bug. Deliberately NOT wrapped in SEH.
    {
        static int crashAfter = -1;
        if (crashAfter < 0) {
            const char* e = getenv("KENSHICOOP_TEST_CRASH");
            crashAfter = e ? atoi(e) : 0;
        }
        if (crashAfter > 0 && g_gameStarted &&
            (GetTickCount() - g_gameStartTick) >= (DWORD)(crashAfter * 1000)) {
            coopLog("[crash] TEST-CRASH: faulting on purpose now");
            volatile int* p = (int*)0x80; // same shape as the 2026-08-03 fault
            *p = 1;
        }
    }
#endif

    KC_PERF(S_PANEL, coopPanelDrive());

    // Protocol 32 world-swap edge detection + session reset. Runs FIRST so the
    // reload edge lands before any sync code touches pointers from the torn-down
    // world this tick.
    KC_PERF(S_WORLD_SWAP, tickWorldSwapEdge(gw));

    if (!g_gameStarted && coop::engine::gameplayLive(gw)) {
        g_gameStarted   = true;
        g_gameStartTick = GetTickCount();
        coopLog("KenshiCoop: gameplay started");
        sendLoadReady();
        // Coordinated load (protocol 32): the title-screen auto-load fired the
        // load detour BEFORE gameplay - discard its queued edge here, or the
        // first driveLoadSync tick (g_gameStarted now true) would mistake it
        // for a mid-session host load and broadcast a spurious PKT_LOAD_GO.
        {
            coop::engine::LoadEdge stale[8];
            unsigned int n = coop::engine::drainLoadEdges(stale, 8);
            g_firstWorldLoaded = n > 0; // no load edge = the player started a NEW game
            if (n > 0) {
                char b[112];
                _snprintf(b, sizeof(b) - 1,
                          "[load] %u pre-gameplay load edge(s) discarded (title-screen auto-load)",
                          n);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
            }
        }
        // Speed-intent capture (vote/effective decoupling): detour the engine's
        // speed setters so every USER action (button, keyboard pause, simulated
        // click) registers as a vote, while our own quiet applies stay invisible.
        // Installed at gameplay start so the seed reads the save's live speed.
        if (coop::engine::installSpeedIntentHooks(gw))
            coopLog("[speed] intent hooks installed (setGameSpeed/userPause/togglePause)");
        else
            coopLog("[speed] FAILED to install intent hooks (vote capture degraded)");
        // "Multiplayer+ (Wanderer x2)" start: give both wanderers the stat line the start
        // promises. Raise-only, so it is a FLOOR at 50 - it re-runs on every load
        // of such a world and is a no-op once the characters have trained past it.
        //
        // Gated on having no peer: buffAllPlayerStats walks EVERY squad tab, which
        // on a connected join includes the host's bodies, and writing those fights
        // the owner-authoritative stats channel (protocol 17). Solo at gameplay
        // start the write is purely local, and the join receives it as part of the
        // host's world through the connect-push below.
        //
        // The lobby's "Starting stats" setting, when set, replaces the 50.
        bool ngStatsSet = !g_firstWorldLoaded && g_ngStats > 0;
        if (!g_peerPresent && !ngStatsSet &&
            coop::engine::playerSquadHasTemplate(gw, WPX2_MARKER_SID)) {
            unsigned int nb = coop::engine::buffAllPlayerStats(gw, WPX2_STAT_LEVEL);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "[start] Multiplayer+ (Wanderer x2): raised %u PC(s) to %d in every stat",
                      nb, (int)WPX2_STAT_LEVEL);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        }
        // New game settings (lobby panel): only a world that did not come from a
        // save, and only where it is ours to write (no peer yet: a join gets the
        // host's world, and the host's world reaches the join on connect).
        if (!g_firstWorldLoaded && !g_peerPresent) {
            loadPrefs();
            char b[160];
            _snprintf(b, sizeof(b) - 1, "[start] new game: money=%s stats=%s",
                      NG_MONEY_TEXT[g_ngMoney], NG_STATS_TEXT[g_ngStats]);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            if (g_ngMoney > 0) {
                bool ok = coop::engine::writePlayerWallet(gw, NG_MONEY[g_ngMoney]);
                _snprintf(b, sizeof(b) - 1, "[start] wallet set to %d cats (%s)",
                          NG_MONEY[g_ngMoney], ok ? "ok" : "FAILED");
                b[sizeof(b) - 1] = '\0'; coopLog(b);
            }
            if (g_ngStats > 0) {
                // Raise-only (a floor): a new game's characters start below every
                // preset, and the Multiplayer+ 50 is skipped above when this is set.
                unsigned int nb = coop::engine::buffAllPlayerStats(gw, (float)NG_STATS[g_ngStats]);
                _snprintf(b, sizeof(b) - 1, "[start] raised %u PC(s) to %d in every stat",
                          nb, NG_STATS[g_ngStats]);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
            }
        }
        // Push-save-on-connect ordering: if a peer connected while we were still
        // at the menu / loading, its connect edge could not bake a save (no live
        // world yet). Now that gameplay is live, arm the connect-push so the
        // waiting join gets pulled into this world.
        if (g_cfg.isHost && g_cfg.saveSync && g_peerPresent)
            armConnectPush();
    }

    // Manual-validation helper (host only): KENSHICOOP_AUTORECRUIT=N seconds -
    // ONCE, N s after gameplay settles, programmatically recruit the nearest
    // non-player world NPC. probeRecruit -> recruitNpc -> g_recruitFn is the
    // SAME PlayerInterface::recruit the dialog "join me" outcome hits, so
    // recruit_hook authors the edge and the whole recruit-sync path runs. Lets
    // us exercise recruit on any populated save (camp/squad1) with no dialog-
    // hireable NPC. OFF by default (0 = no-op, zero cost).
    if (g_cfg.isHost && g_gameStarted) {
        static int  autoRecruitS    = -1;
        static bool autoRecruitDone = false;
        if (autoRecruitS < 0) {
            const char* e = std::getenv("KENSHICOOP_AUTORECRUIT");
            autoRecruitS = e ? std::atoi(e) : 0;
        }
        if (autoRecruitS > 0 && !autoRecruitDone &&
            (GetTickCount() - g_gameStartTick) >= (DWORD)autoRecruitS * 1000u) {
            autoRecruitDone = true;
            unsigned int hb[5], ha[5];
            // Wide reach (600u): a caged/imprisoned player squad (the camp save)
            // sits well beyond the scenario's 60u nearest-NPC probe from the
            // guards/prisoners it should recruit.
            int res = coop::engine::probeRecruit(gw, false, hb, ha, 600.0f);
            char b[192];
            _snprintf(b, sizeof(b) - 1,
                "[recruit] AUTORECRUIT res=%d before=%u,%u,%u,%u,%u "
                "after=%u,%u,%u,%u,%u", res, hb[0], hb[1], hb[2], hb[3], hb[4],
                ha[0], ha[1], ha[2], ha[3], ha[4]);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        }
    }

    // Test-runner self-exit: quit cleanly after the configured duration so
    // unattended runs terminate on their own and flush their logs. Also a hard
    // backstop for a scenario that never reports completion.
    if (g_cfg.testSeconds > 0 && g_gameStarted &&
        (GetTickCount() - g_gameStartTick) >= (DWORD)g_cfg.testSeconds * 1000u) {
        coopLog("KenshiCoop: test duration elapsed; exiting");
        coop::logClose();
        // TerminateProcess (not ExitProcess): we're inside the live game loop
        // with GPU/audio/net threads running; orderly teardown deadlocks on the
        // loader lock. Our log is already flushed/closed above.
        TerminateProcess(GetCurrentProcess(), 0);
    }

    KC_PERF(S_NET_EVENTS, processNetEvents(gw));
    // Control lock: in a real session nobody orders another player's characters.
    coop::engine::setControlLock(g_peerPresent && realSession());
    if (gw && g_peerPresent && realSession()) coop::engine::enforceOwnSelection(gw);
    KC_PERF(S_NET_EVENTS, tickPermissions());
    KC_PERF(S_NET_EVENTS, tickReady());
    KC_PERF(S_NET_EVENTS, tickModCheck());
    KC_PERF(S_NET_EVENTS, tickLook(gw));
    KC_PERF(S_NET_EVENTS, tickResync());
    KC_PERF(S_NET_EVENTS, tickTrade(gw));
    KC_PERF(S_NET_EVENTS, tickShop());
    KC_PERF(S_NET_EVENTS, tickPing());
    KC_PERF(S_NET_EVENTS, tickRoster(gw));

    // Coordinated load (protocol 32): the JOIN never loads locally while a
    // peer is connected under the gate - the host arbitrates; a manual load
    // press forwards as PKT_LOAD_REQ. Evaluated per tick (not just on the
    // connect edge) so the title-screen auto-load - which precedes gameplay -
    // is never swallowed.
    {
        bool want = !g_cfg.isHost && g_cfg.loadSync && g_peerPresent && g_gameStarted;
        if (want != g_loadSuppressOn) {
            g_loadSuppressOn = want;
            coop::engine::setLoadSuppress(want);
            coopLog(want ? "[load] JOIN load suppression ON (host arbitrates loads)"
                         : "[load] JOIN load suppression OFF");
        }
    }

    // Test-scene setup: host spawns the controlled scene ONCE, a few seconds
    // after gameplay starts, then leaves the game running so the user can arrange
    // the pose and SAVE (baking save-stable hands that resolve on both clients).
    KC_PERF(S_HARNESS, tickSetupScene(gw));

    // Deferred auto-bake: write the fixture save once the armed settle window
    // elapses (one-shot); the self-exit timer then ends the bake run.
    KC_PERF(S_HARNESS, tickAutoBake());

    // Host re-arm + inventory-owner registration: keep craft/down scenes on task
    // across save/load drift and (re)resolve the owned container for invSync.
    KC_PERF(S_HARNESS, tickHostRearm(gw));

    // Scenario completion hold: once a verdict is logged, keep driving the synced
    // bodies on screen for the capture window, then self-exit cleanly. (harness)
    KC_PERF(S_HARNESS, tickScenarioHold());

    // Coordinated load (protocol 32): while a world swap is in progress the
    // OLD world is being torn down under us - every cached Character*/
    // RootObject* is dangling, and mainLoop_hook keeps ticking throughout
    // (~500 ticks across a ~4 s load). Touching those pointers mid-swap is a
    // pure-virtual-call crash (a virtual on a half-destroyed object). Skip
    // ALL replication until the reload edge (gameplay live again) runs the
    // session reset at the top of the hook, which clears the stale caches -
    // the next live tick then resumes replication with a fresh world.
    // Sampled ONCE (pre-engine): if the engine tick below completes the
    // reload, the reset still runs first NEXT tick, so we stay hands-off this
    // tick either way. Before first gameplay this is simply false (no caches
    // yet). driveLoadSync itself is NOT gated on this - it only touches net
    // packets + file IO + loadSave, never cached world pointers.
    const bool worldLive = g_gameStarted && coop::engine::gameplayLive(gw);

    // Build capture runs inside an engine factory call, not on our tick, so it
    // needs worldLive handed to it: a world load creates every building in the
    // save through that same factory and none of them are player placements.
    coop::engine::setBuildCaptureArmed(worldLive);

    // Replication publish (pre-engine, worldLive-gated): ingest received targets,
    // then stream every owned channel so applied state is current this tick.
    tickReplicatePublish(gw, worldLive); // sections timed inside

    // Coordinated save + load (protocols 31/32): run under g_gameStarted (NOT
    // worldLive) so they keep pumping DURING a world swap - they only touch net
    // packets, file IO and the load/save ENTRY points, never a cached world ptr.
    KC_PERF(S_SAVE_LOAD, tickCoordinatedSaveLoad(gw));

    // Scenario onStart (harness): pre-arm hold + peer-ready/timeout arm, fired
    // once BEFORE the engine tick so a host-issued order takes effect this frame.
    KC_PERF(S_HARNESS, tickScenarioStart(gw));

    KC_PERF(S_ENGINE, g_mainLoop_orig(gw, dt)); // run the engine (incl. local AI)

    // Replication apply (post-engine): our transform is the last word the renderer
    // samples. Re-checks gameplayLive so a swap STARTED by this engine tick skips
    // apply (its caches are now stale; the reset runs next tick's reload edge).
    tickReplicateApply(gw, worldLive); // sections timed inside

    // Coordinated load deferred-signal backstop: if the LOADGAME signal stalls
    // past the grace window, pump SaveManager::execute() once from end-of-tick.
    KC_PERF(S_SAVE_LOAD, tickLoadPumpBackstop(gw));

    // Scenario onTick (harness) AFTER apply, so a join's RECV line reflects the
    // applied pos; latches the verdict + begins the capture hold on completion.
    KC_PERF(S_HARNESS, tickScenarioTick(gw));

    perfEndTick(perfHookT0);
}

// Title-screen update hook: a safe main-thread point every frame the menu is up.
// Wait g_cfg.autoLoadDelayMs after the first title frame AND until the save
// subsystem reports ready, then issue the deferred load once.
// SEH-guarded so a fault in the (title-screen-untested) GUI stack can never
// abort titleUpdate_hook before the bootstrap pump runs. No C++ unwind objects
// live in titleUpdate_hook, but coopPanelDrive uses std::string internally, so
// the guarded call lives in its own function (C2712).
void coopPanelDriveSeh() {
    __try { coopPanelDrive(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true;
            coopErr("[coop-ui] panel tick FAULTED at title screen (guarded)"); }
    }
}

void titleUpdate_hook(TitleScreen* self) {
    g_titleUpdate_orig(self);

    // Bring-up trace: log once on the first title tick, then only when the
    // online state flips (the moment the user toggles ONLINE via F2), so the log
    // shows the menu hook is live and pinpoints the connect edge without spam.
    {
        static int s_lastRunning = -1;
        int running = g_net.isRunning() ? 1 : 0;
        if (running != s_lastRunning) {
            s_lastRunning = running;
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "[boot] title-tick running=%d host=%d started=%d saveEmpty=%d",
                      running, g_cfg.isHost ? 1 : 0, g_gameStarted ? 1 : 0,
                      g_cfg.save.empty() ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        }
    }

    // Push-save-on-connect (join, main menu). A join that has gone ONLINE must
    // drain the connect edge, receive the host's pushed save, and load it while
    // still at the title screen - work that normally only runs in-game from
    // mainLoop_hook (gated on g_gameStarted). Pump the join half FIRST (before
    // the panel) so a GUI fault can never block it. gw is null: processNetEvents
    // only touches it under a gw&& guard, and the JOIN branches of driveSaveSync/
    // driveLoadSync never deref it. The load path is gated on savesReady():
    // before then the host's LOAD_GO simply waits in the inbound queue (no NACK
    // -> no stream yet), and the save-receiver half still commits chunks to disk.
    if (g_net.isRunning() && !g_cfg.isHost && !g_gameStarted) {
        processNetEvents(0);
        if (g_cfg.saveSync) driveSaveSync();
        if (g_cfg.loadSync && coop::engine::savesReady()) driveLoadSync(0);
        // F2 panel + status banner while the join waits at the menu (guarded; the
        // host's world is the destination, so we skip the config auto-load for a
        // join session).
        coopPanelDriveSeh();
        return;
    }

    // Co-op panel (F2) at the main menu for the HOST / offline case: lets a user
    // toggle ONLINE and paste a Steam ID before any save is loaded. Guarded
    // because the title-screen GUI stack is otherwise unexercised.
    coopPanelDriveSeh();

    if (g_autoLoadDone || g_cfg.save.empty()) return;

    DWORD now = GetTickCount();
    if (g_titleFirstTick == 0) { g_titleFirstTick = now; return; }
    if ((now - g_titleFirstTick) < g_cfg.autoLoadDelayMs) return;
    if (!coop::engine::savesReady()) return;

    if (coop::engine::loadSave(g_cfg.save)) {
        g_autoLoadDone = true;
        char m[128];
        _snprintf(m, sizeof(m) - 1,
                  "KenshiCoop: auto-load issued for save '%s' (after %lu ms settle)",
                  g_cfg.save.c_str(), (unsigned long)(now - g_titleFirstTick));
        m[sizeof(m) - 1] = '\0';
        coopLog(m);
    }
}

void startNetworking() {
    // Debug WAN simulation: when configured, hold/drop inbound entity batches so the
    // loopback harness exercises the real-latency path (interp + local enforcement)
    // instead of the ~0 ms same-frame delivery we'd otherwise validate against.
    if (g_cfg.netSimDelayMs || g_cfg.netSimJitterMs || g_cfg.netSimLossPct) {
        g_net.setNetSim(g_cfg.netSimDelayMs, g_cfg.netSimJitterMs, g_cfg.netSimLossPct);
        char b[128];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: NET SIM on - delay=%ums jitter=+/-%ums loss=%u%%",
                  g_cfg.netSimDelayMs, g_cfg.netSimJitterMs, g_cfg.netSimLossPct);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Steam reachability spike (channel 1): independent of the transport, so a
    // UDP session can still prove Steam P2P punch-vs-relay against a peer.
    if (g_cfg.steamPing != 0) {
        if (coop::steamp2p::init()) coop::steamp2p::setPingPeer(g_cfg.steamPing);
        else coopErr("[steam] KENSHICOOP_STEAM_PING set but Steam init failed");
    }

    // Steam P2P transport: connect by SteamID (NAT punch + Valve relay) with the
    // ENet protocol unchanged. Requires the partner's steamid64; falls back to
    // UDP loudly when Steam is unavailable so a misconfigured session still
    // behaves like the stock build instead of silently doing nothing.
    // A HOST needs no peer id (protocol 57): it admits every player whose Steam
    // session is accepted, so several joins can connect; a pasted id only
    // pre-accepts that one. A JOIN needs the host's id.
    if (g_cfg.transport == "steam") {
        if (!g_cfg.isHost && g_cfg.steamPeer == 0) {
            coopErr("[steam] KENSHICOOP_TRANSPORT=steam requires KENSHICOOP_STEAM_PEER=<partner steamid64>; falling back to UDP");
        } else if (!coop::steamp2p::init()) {
            coopErr("[steam] init failed (Steam not running / offline?); falling back to UDP");
        } else {
            coop::steamp2p::setPeer(g_cfg.steamPeer);
            coop::steamp2p::setOpenHost(g_cfg.isHost);
            g_net.setSteamTransport(g_cfg.steamPeer, g_cfg.isHost);
            coopLog("[steam] transport=steam armed (connect by SteamID; no port forwarding)");
        }
    }

    // Our Steam name labels our squad tab on every client (protocol 57); the
    // friends interface comes with the invite layer (idempotent).
    if (coop::steamp2p::init()) coop::steaminvite::init(&coopUiConnect);
    g_net.setLocalName(coop::steaminvite::selfName());
    {
        coop::u32 mh = 0; coop::u16 mc = 0;
        readModList(&mh, &mc);
        g_net.setLocalMods(mh, mc);
        char b[80];
        _snprintf(b, sizeof(b) - 1, "[mods] %u enabled mod(s), list hash %08x", (unsigned)mc, (unsigned)mh);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
    bool ok;
    if (g_cfg.isHost) {
        coopLog("KenshiCoop: starting as HOST");
        ok = g_net.startHost(g_cfg.port, &g_inbound);
    } else {
        coopLog("KenshiCoop: starting as CLIENT");
        ok = g_net.startClient(g_cfg.ip, g_cfg.port, &g_inbound);
    }
    if (!ok) coopErr("KenshiCoop: networking failed to start");
}

// In-game panel handlers. coopUiConnect tears down any live session, re-arms the
// config from the panel's choices, and restarts via the shared startNetworking()
// path (NetLink cleanly supports stop() then start again; Steam is re-armed and
// the Replicator/Inbound session state is reset for a clean handshake).
void coopUiConnect(bool isHost, bool useSteam, unsigned long long peerId) {
    if (g_net.isRunning()) g_net.stop();
    coop::steamp2p::shutdown();
    // World is live here (reconnect from within a running game): despawn minted
    // proxies before clearing maps so a re-connect leaves no orphaned duplicates.
    sessionResetForUi();

    g_cfg.isHost    = isHost;
    g_cfg.transport = useSteam ? "steam" : "udp";
    coop::logSetTag(isHost ? "HOST" : "JOIN"); // the startup tag guessed the role
    // Re-read the UDP endpoint (ip/port) from coop_config.json so editing it then
    // hitting Connect works without restarting the game. (It also picks up a
    // steamPeer if one is set for advanced/back-compat use.)
    coop::reloadPeerFromFile(g_cfg);
    // A Steam ID pasted in the F2 panel this session wins over the config: the
    // normal flow is Copy my Steam ID -> friend Pastes it -> Connect, with no file
    // editing. peerId is 0 when nothing was pasted, so the config value stands.
    if (peerId != 0) g_cfg.steamPeer = peerId;
    // Host streams world NPCs; join drives. Under presence authority both do,
    // each for the cells it claims.
    g_repl.setStreamNpcs(isHost || g_cfg.cellAuth);
    // Ownership ranks must follow the role chosen in the panel. Only an explicit
    // KENSHICOOP_OWN_SQUAD override is preserved; otherwise recompute the default
    // (host owns {0}, join owns {1}). Without this, a session launched as HOST
    // that switches to JOIN keeps rank {0} and wrongly claims the host's player
    // squad, so that unit never moves on the client (unowned NPCs still sync).
    coop::resolveOwnRanks(g_cfg.ownRanks, isHost, g_cfg.ownRanksFromEnv);
    g_repl.setOwnRanks(g_cfg.ownRanks);

    std::string ranks;
    for (std::set<unsigned int>::const_iterator it = g_cfg.ownRanks.begin();
         it != g_cfg.ownRanks.end(); ++it) {
        char n[16]; _snprintf(n, sizeof(n) - 1, "%u", *it); n[sizeof(n) - 1] = '\0';
        if (!ranks.empty()) ranks += ",";
        ranks += n;
    }
    char b[176];
    _snprintf(b, sizeof(b) - 1,
              "[coop-ui] connect: role=%s transport=%s peer=%llu ownRanks={%s} src=%s",
              isHost ? "HOST" : "JOIN", g_cfg.transport.c_str(),
              (unsigned long long)g_cfg.steamPeer, ranks.c_str(),
              g_cfg.ownRanksFromEnv ? "env" : "role");
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
    startNetworking();
}

void coopUiDisconnect() {
    coopLog("[coop-ui] disconnect");
    if (g_net.isRunning()) g_net.stop();
    coop::steaminvite::reset(); // leave any Steam lobby
    coop::steamp2p::shutdown();
    // World stays live on a manual disconnect: despawn minted proxies before
    // clearing maps so nothing lingers as a duplicate or gets baked into a save.
    sessionResetForUi();
}

} // namespace

// RE_Kenshi resolves the entry by its C++-mangled name (?startPlugin@@YAXXZ), so
// this must NOT be extern "C".
// ---------------------------------------------------------------------------
// PluginHooks (Phase 3): startPlugin's install wall, split into named phases.
// Each was lifted VERBATIM from startPlugin, so load-time behavior + ordering
// are unchanged; startPlugin now reads as loadConfig -> banner -> hook -> wire.
// ---------------------------------------------------------------------------

// Startup log roster: load banner, fake-skew notice, build stamp, role line,
// and the effective (resolved) config summary. Answerable from the log alone.
void logStartupBanner() {
    coopLog("KenshiCoop loaded! (clean rebuild)");
    if (g_cfg.fakeClockSkewMs != 0) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "KenshiCoop: FAKE clock skew injected: %+ld ms",
                  g_cfg.fakeClockSkewMs);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
    // Build stamp: changes every compile, so the test runner can confirm a fresh
    // DLL is actually deployed (anti-stale guard) rather than an old cached copy.
    {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "KenshiCoop: build %s %s", __DATE__, __TIME__);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
    {
        char b[128];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: role=%s proto=v%u port=%d save='%s'",
                  g_cfg.isHost ? "HOST" : "JOIN", (unsigned)coop::PROTOCOL_VERSION,
                  g_cfg.port, g_cfg.save.c_str());
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
    // Effective (resolved) config roster: which sync channels are actually on/off
    // for THIS run + the key tuning knobs. Answerable from the log alone.
    coopLog(coop::describeConfig(g_cfg).c_str());
}

// Main-thread tick hook. GameWorld exposes both the virtual and the non-virtual
// _NV_mainLoop_GPUSensitiveStuff (same RVA); GetRealAddress only works on the
// _NV_ variant. Returns false (caller aborts startup) if the hook can't install.
bool installMainLoopHook() {
    if (KenshiLib::SUCCESS !=
        KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            &mainLoop_hook, &g_mainLoop_orig)) {
        coopErr("KenshiCoop: could not install main-loop hook!");
        return false;
    }
    return true;
}

// Resolve engine symbols and apply every replicator configuration knob (stream
// scope, ownership ranks, interp/drive/combat tuning, audit rows, and the
// channel enables that are pure config - the detour-backed channels are wired in
// installEngineDetours).
void configureReplicator() {
    coop::engine::resolve();

    // Stage 4: the host streams nearby world NPCs (host-authoritative) in addition
    // to its squad; the join resolves each by hand and drives it like a squad body.
    if (g_cfg.isHost || g_cfg.cellAuth) g_repl.setStreamNpcs(true);

    // Bidirectional presence: this client owns (controls + streams) a disjoint set of
    // the shared squad chosen by save-stable hand-rank; it drives the peer's owned
    // members from their stream. Default leader-first: host {0}, join {1}.
    g_repl.setOwnRanks(g_cfg.ownRanks);
    {
        std::string ranks;
        for (std::set<unsigned int>::const_iterator it = g_cfg.ownRanks.begin();
             it != g_cfg.ownRanks.end(); ++it) {
            char num[16]; _snprintf(num, sizeof(num) - 1, "%u", *it); num[sizeof(num)-1] = '\0';
            if (!ranks.empty()) ranks += ",";
            ranks += num;
        }
        std::string m = "KenshiCoop: ownership ranks = {" + ranks + "} (bidirectional presence)";
        coopLog(m.c_str());
    }

    // Protocol 36 movement-smoothness knobs (KENSHICOOP_INTERP_* / _CATCHUP_K /
    // _SNAP_DIST): live-tune the interp window and the walk-drive gains for WAN
    // A/B runs. Defaults reproduce the historical constants exactly.
    {
        coop::InterpConfig ic;
        ic.minDelayMs  = g_cfg.interpMinDelayMs;
        ic.maxDelayMs  = g_cfg.interpMaxDelayMs;
        ic.maxExtrapMs = g_cfg.interpMaxExtrapMs;
        ic.staleMs     = g_cfg.interpStaleMs;
        ic.snapDistSq  = g_cfg.interpSnapDist * g_cfg.interpSnapDist;
        ic.cadenceDelayK     = g_cfg.interpCadenceK;
        ic.maxCadenceDelayMs = g_cfg.interpMaxCadenceDelayMs;
        g_repl.setInterpConfig(ic);
        g_repl.setDriveTuning(g_cfg.catchupK, g_cfg.snapDist, g_cfg.snapSeconds);
        g_repl.setCombatTuning(g_cfg.combatSoftDist, g_cfg.combatSnapDist,
                               g_cfg.combatBigSnapDist, g_cfg.combatSlideMax,
                               g_cfg.combatConvergeMs);
        g_repl.setSendStamp(g_cfg.sendStamp);
        g_repl.setCensusRadius(g_cfg.censusRadius);
        g_repl.setSpawnMintRadius(g_cfg.spawnMintRadius);
        g_repl.setAdoptRadius(g_cfg.adoptRadius);
        g_repl.setCensusParkDist(g_cfg.censusParkDist);
        g_repl.setCensusWalkDist(g_cfg.censusWalkDist);
        g_repl.setCensusFreezeAi(g_cfg.censusFreezeAi);
        g_repl.setAttentionRadius(g_cfg.attentionRadius);
        g_repl.setCellAuth(g_cfg.cellAuth);
        g_repl.setCellCollapse(g_cfg.cellCollapse);
        g_repl.setSpeedCombatCap(g_cfg.speedCombatCap);
        g_repl.setStarveHold(g_cfg.starveHoldMs);
        // Camera-anchored interest (protocol 43): engine-side master enable
        // for the camera anchors in interestCenters.
        coop::engine::setCamInterest(g_cfg.camInterest);
        // travel_parity needs the 5 s SCENARIO WORLD/WNPC worldstate rows on
        // both sides; npc_sync feeds the same rows to the anti-zombie oracle
        // (Phase 2 mid-band tier: a populated town run has moving census-band
        // NPCs, which the hop corridor mostly lacks). Every other scenario
        // stays quiet (log volume).
        // run_apart joins them because the population question it exists to ask
        // - who holds a body while the two squads are 146,000 u apart and one
        // camera is on the other's town - can only be answered by cross-matching
        // the two sides' enumerated sets, hand by hand, with the authority class
        // each side assigned. The aggregate counters cannot: a creature visible
        // on one client and absent on the other leaves every bucket balanced.
        g_repl.setAuditRows(g_cfg.scenario == "travel_parity" ||
                            g_cfg.scenario == "split_far" ||
                            g_cfg.scenario == "run_apart" ||
                            g_cfg.scenario == "npc_sync" ||
                            g_cfg.scenario == "world_parity" ||
                            g_cfg.scenario == "escape_cohesion" ||
                            g_cfg.scenario == "jail_probe" ||
                            g_cfg.scenario == "jail_soak" ||
                            g_cfg.jailProbe);  // manual -JailProbe: no scenario name
        char b[260];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: interp delay=%u-%ums extrap=%ums stale=%ums snap=%.0fu "
                  "drive catchupK=%.2f snapDist=%.1fu snapSec=%.2f sendStamp=%d "
                  "census=%.0fu mint=%.0fu park=%.0fu walk=%.0fu starveHold=%ums",
                  g_cfg.interpMinDelayMs, g_cfg.interpMaxDelayMs,
                  g_cfg.interpMaxExtrapMs, g_cfg.interpStaleMs, g_cfg.interpSnapDist,
                  g_cfg.catchupK, g_cfg.snapDist, g_cfg.snapSeconds,
                  g_cfg.sendStamp ? 1 : 0,
                  g_cfg.censusRadius, g_cfg.spawnMintRadius, g_cfg.censusParkDist,
                  g_cfg.censusWalkDist, g_cfg.starveHoldMs);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Carried-body sync (protocol 18, default ON): reliable pickup/drop edges +
    // self-healing carried state. KENSHICOOP_CARRY_SYNC=0 is the A/B escape hatch.
    g_repl.setCarrySync(g_cfg.carrySync);

    // Furniture occupancy sync (protocol 19, default ON): reliable bed/cage
    // enter/exit edges + self-healing occupancy state. KENSHICOOP_FURN_SYNC=0
    // is the A/B escape hatch.
    g_repl.setFurnSync(g_cfg.furnSync);
    g_repl.setChainSync(g_cfg.chainSync);
    g_repl.setStealthSync(g_cfg.stealthSync);

    // Prone posture sync (protocol 53, default ON): driven copies are posed with
    // the owner's exact ProneState so an injured crawler is not walk-driven
    // upright. KENSHICOOP_PRONE_SYNC=0 is the A/B escape hatch.
    g_repl.setProneSync(g_cfg.proneSync);

    // Shared money pool (protocol 52, default ON): one host-authoritative purse
    // both players spend from. KENSHICOOP_MONEY_SYNC=0 is the A/B escape hatch
    // (and the shop_probe baseline).
    g_repl.setMoneySync(g_cfg.moneySync);

    // Runtime-spawn proxy replication (protocol 21, default ON): the join mints
    // local proxy bodies for host runtime spawns it cannot resolve by hand.
    // KENSHICOOP_SPAWN_SYNC=0 is the A/B escape hatch (spawn_probe forces off).
    g_repl.setSpawnSync(g_cfg.spawnSync);

    // AI-gating probe (join side): recruit diverged NPCs to test the inhabit lever.
    if (!g_cfg.isHost && g_cfg.probeRecruit) g_repl.setProbeRecruit(true);
}

// Install every engine detour + set the detour-backed channel enables (AI
// suspend, task-select spike, jail probes, damage guard, shop, trade veto, item
// drop, recruit/squad/faction/build/dismantle, coordinated save/load).
void installEngineDetours() {
    // AI-suspend (BOTH roles, DEFAULT ON): detour Character::periodicUpdate so
    // any body a client DRIVES from the peer's stream stops self-tasking (decision
    // layer off) while still animating. Faction is untouched - we hold the body's
    // current action instead of letting the AI re-decide and wander/thrash. This
    // is the universal QUIETING layer (doctrine 15 amendment); per-class APPLY
    // levers sit on top. KENSHICOOP_AI_SUSPEND=0 disables (A/B escape hatch).
    //
    // Phase 1b: enabled on the HOST too (was join-only). The host drives nothing
    // in the classic host-authoritative single-direction case (it owns every
    // NPC), so this is a no-op there. But once a recruit is TRANSFERRED into the
    // join's tab, control flips to the join and the HOST must now drive that
    // peer-owned squad member - without suspend it self-follows the host's leader
    // and shows the same slow-walk/snap artifact the join had. applyTargets only
    // ever suspends bodies we drive (peer-owned), so a host-owned NPC/leader is
    // never quieted by this.
    if (g_cfg.aiSuspend) {
        if (coop::engine::installAiSuspendHook()) {
            g_repl.setAiSuspend(true);
            coopLog("[ai] periodicUpdate detour installed; AI-suspend ON (default)");
        } else {
            coopLog("[ai] FAILED to install periodicUpdate detour; AI-suspend disabled");
        }
    }
    // Task-selection observation spike (KENSHICOOP_TASK_SPIKE, off by default):
    // passive detour on CharBody::setCurrentAction to prove the selection seam is
    // hookable in isolation and log the chosen task tuple per body. No behavior
    // change - a diagnostic for the "stream selection, not motion" direction.
    if (g_cfg.taskSelectSpike) {
        if (coop::engine::installTaskSelectSpikeHook()) {
            coop::engine::setTaskSelectSpike(true);
            coopLog("[spike] setCurrentAction detour installed; task-select observation ON (KENSHICOOP_TASK_SPIKE)");
        } else {
            coopLog("[spike] FAILED to install setCurrentAction detour (seam unresolved)");
        }
    }
    // Jail put-to-work desync spike: correlated [jail] STATE traces (read-only).
    g_repl.setJailProbe(g_cfg.jailProbe);
    if (g_cfg.jailProbe)
        coopLog("[jail] KENSHICOOP_JAIL_PROBE=1: captive-state [jail] STATE tracing ON");
    // Jail put-to-work observation spike (Phase A): host runs the captive
    // unopposed and logs its trajectory ([jail] OBSERVE) to classify intent.
    g_repl.setJailObserve(g_cfg.jailObserve);
    if (g_cfg.jailObserve)
        coopLog("[jail] KENSHICOOP_JAIL_OBSERVE=1: captive drive/suspend/self-heal OFF; [jail] OBSERVE tracing ON");
    // Step-2 experiment: sitter no-detach A/B (manual runs only; off by default).
    if (!g_cfg.isHost && g_cfg.noDetach) {
        g_repl.setNoDetach(true);
        coopLog("[quiet] KENSHICOOP_NO_DETACH=1: sitter detachFromTownAI SKIPPED (experiment)");
    }

    // Divergence-gated authority (join side, DEFAULT ON since the step-4 A/B):
    // trust world NPCs whose local AI sustainedly agrees with the host; drive
    // only divergence. KENSHICOOP_GATE_AUTHORITY=0 disables (A/B escape hatch).
    if (!g_cfg.isHost && g_cfg.gateAuthority) {
        g_repl.setGateAuthority(true);
        coopLog("[trust] divergence-gated authority ON (default; KENSHICOOP_GATE_AUTHORITY=0 disables)");
    }

    // Damage guard (BOTH sides, DEFAULT ON): locally-simulated melee hits on
    // driven bodies are suppressed (HIT_MISSED) so cosmetic fights cannot diverge
    // the local-only medical model. The guard set is "every body this client
    // drives": the peer's world-NPC copies on the join, the peer's SQUAD-member
    // copies on the host (host-side extension 2026-07-06 - phase-1 player_combat
    // measured the host's copy of a join victim bleeding 40+ blood while the
    // join's guarded copies stayed at 0). KENSHICOOP_DAMAGE_GUARD=0 disables.
    if (g_cfg.damageGuard) {
        if (coop::engine::installDamageGuardHook()) {
            g_repl.setDamageGuard(true);
            // Join-dealt authoritative damage report (protocol 45): only the JOIN
            // reports (the HOST owns + simulates world NPCs, so its own swings land
            // natively). Enabling report mode on the join makes the guard accumulate
            // the damage its player-squad melee WOULD have dealt to driven world-NPC
            // copies; publishCombatHits forwards it and the host wounds the real body.
            g_repl.setReportCombat(!g_cfg.isHost);
            coopLog(g_cfg.isHost
                ? "[dmg] hitByMeleeAttack detour installed; damage guard ON (host, driven peer-squad bodies)"
                : "[dmg] hitByMeleeAttack detour installed; damage guard ON + combat-hit report ON (join)");
        } else {
            coopLog("[dmg] FAILED to install hitByMeleeAttack detour; damage guard disabled");
        }
    }

    // Purchase observability (protocol 52 groundwork): log every real trade-UI
    // purchase ("[shop] BUY-LOCAL"), the field evidence for the vendor-stock
    // mirror design. Always installed: it also enforces the host's "join may
    // buy" switch (protocol 56) and marks a purchase in progress, which the
    // storage switch in the drag detours below must not mistake for a take
    // out of a shop's storage.
    if (coop::engine::installShopHook())
        coopLog("[shop] buyItem detour installed; purchase logging ON");
    else
        coopLog("[shop] FAILED to install buyItem detour; purchase logging off");

    // Cross-owner trade veto (KENSHICOOP_BLOCK_XFER, default ON in real sessions):
    // refuse a UI inventory drag whose source + destination squad characters are
    // owned by different clients (item stays in the source bag) so ground drops
    // are the only cross-client transfer path. Retires Protocol 37 (Config forces
    // xferSync off when this is on). The classifier is always registered (cheap).
    // The detours are ALWAYS installed: the host's "join may use storage" switch
    // (protocol 56) is enforced in them, and the role can change in the panel at
    // runtime. With the veto off and storage allowed they only test two flags.
    coop::engine::setInvOwnerClassifier(&coopInvOwnerClass);
    coop::engine::setBlockXfer(g_cfg.blockXfer);
    if (coop::engine::installXferBlockHook())
        coopLog(g_cfg.blockXfer
            ? "[xfer] drag detours installed; cross-owner trade veto ON (drop to transfer)"
            : "[xfer] drag detours installed; veto off, storage permission enforced here");
    else
        coopLog("[xfer] FAILED to install drag detours; trade veto and storage permission degraded");

    // Phase W1b: query-free ground-drop capture (town reliability). Detour
    // Inventory::dropItem so every drop is discovered without the spatial sphere
    // query that fails in towns; publishWorldItems seeds its tracker from these
    // edges and culls by handle liveness. Rides the world-sync gate (the channel
    // it feeds), so real sessions and the world_item_* / drop scenarios get it.
    if (g_cfg.worldSync) {
        if (coop::engine::installItemDropHook())
            coopLog("[wi] dropItem detour installed; query-free drop capture ON");
        else
            coopLog("[wi] FAILED to install dropItem detour; drop capture falls back to the spatial scan");
    }

    // Recruitment sync (protocol 23, default ON): detour PlayerInterface::
    // recruit so every successful local recruit (dialog or programmatic)
    // authors a reliable EVT_RECRUIT; the peer re-keys its local copy of the
    // recruited body to the new stream key. KENSHICOOP_RECRUIT_SYNC=0 is the
    // A/B escape hatch (recruit_probe forces it off to keep the unsynced
    // baseline measurable).
    g_repl.setRecruitSync(g_cfg.recruitSync);
    // Squad management sync (protocol 35, default ON): pointer-diff move
    // edges -> EVT_SQUAD_MOVE re-keys + the container-rank latch (mid-session
    // tabs append instead of reshuffling ownership). KENSHICOOP_SQUAD_SYNC=0
    // is the A/B escape hatch (squad_probe forces it off to keep the unsynced
    // baseline measurable).
    g_repl.setSquadSync(g_cfg.squadSync);
    g_repl.setFactionSync(g_cfg.factionSync);
    g_repl.setTimeSync(g_cfg.timeSync);
    g_repl.setTimeBrake(g_cfg.timeBrake);
    g_repl.setDoorSync(g_cfg.doorSync);
    g_repl.setBuildSync(g_cfg.buildSync);
    g_repl.setBdoorSync(g_cfg.bdoorSync);
    g_repl.setHungerSync(g_cfg.hungerSync);
    g_repl.setProdSync(g_cfg.prodSync);
    g_repl.setResearchSync(g_cfg.researchSync);
    g_repl.setDeedSync(g_cfg.deedSync);
    g_repl.setFixtureSync(g_cfg.fixtureSync);
    // Protocol 34: the HOST authors every storage/machine container near the
    // interest centers (the ~1 Hz census inside publishInventories); the join
    // reconciles via the translated key. Host-only flag - the join must never
    // census-author (host-authoritative world containers). Layered on invSync
    // (publishInventories is the carrier and is gated on it below).
    g_repl.setStoreSync(g_cfg.storeSync && g_cfg.isHost);
    // v63 trader stock authority: OFF by default until a field session shows
    // where shop stock lives (the trade UI's ShopTrader inventory is lazy and
    // runtime-keyed; the trader character's is save-stable). Authoring the wrong
    // container could empty a join's shop. KENSHICOOP_TRADER_SYNC=1 turns it on.
    {
        const char* ts = getenv("KENSHICOOP_TRADER_SYNC");
        g_repl.setTraderSync(g_cfg.isHost && ts && ts[0] == '1');
    }
    if (g_cfg.recruitSync) {
        if (coop::engine::installRecruitHook())
            coopLog("[recruit] recruit detour installed; recruitment sync ON");
        else
            coopLog("[recruit] FAILED to install recruit detour; recruitment sync degraded (no local-edge detection)");
    }

    // Build-placement observability (protocol 27): detour RootObjectFactory::
    // createBuilding so every REAL build-mode commit logs its template +
    // transform + runtime hand ("[build] LOCAL-PLACE") and queues an edge for
    // the sync layer. Installed for the sync AND for build_probe (which
    // measures the unsynced baseline but wants the placement evidence).
    if (g_cfg.buildSync || g_cfg.scenario == "build_probe") {
        if (coop::engine::installBuildHook())
            coopLog("[build] createBuilding capture installed; placement logging ON");
        else
            coopLog("[build] FAILED to install placement capture; placement sync OFF");
    }

    // Dismantle observability (protocol 28): detour Building::
    // notifyConstructionDismantling so every UI dismantle logs its hand
    // ("[build] LOCAL-DISMANTLE") and queues a removal edge for the sync
    // layer. Installed for the sync AND for bdoor_probe (unsynced baseline,
    // but the edge evidence is the finding).
    if (g_cfg.bdoorSync || g_cfg.scenario == "bdoor_probe") {
        if (coop::engine::installDismantleHook())
            coopLog("[build] dismantle detour installed; removal logging ON");
        else
            coopLog("[build] FAILED to install dismantle detour; removal logging degraded");
    }

    // Faction-relation observability (protocol 24): detour both affectRelations
    // overloads so every REAL relation mutation logs its cause ("[fac] AFFECT")
    // and records a delta for the sync layer. Installed for the sync AND for
    // faction_probe (which measures the unsynced baseline but needs the
    // cause-attribution evidence).
    if (g_cfg.factionSync || g_cfg.scenario == "faction_probe") {
        if (coop::engine::installFactionHooks())
            coopLog("[fac] affectRelations detours installed; relation logging ON");
        else
            coopLog("[fac] FAILED to install affectRelations detours; relation logging degraded");
    }

    // Coordinated save (protocol 31): detour SaveManager::save so every local
    // save (menu, quicksave, autosave timer, programmatic bake) logs its
    // "[save] LOCAL-SAVE" edge and queues a SaveEdge - the trigger for the
    // host-authoritative save transfer (and the join's REQ forwarding).
    // save_probe installs it too but measures the raw behaviour alone
    // (saveSync forced off - no coordination on top).
    if (g_cfg.saveSync || g_cfg.scenario == "save_probe") {
        if (coop::engine::installSaveHook())
            coopLog(g_cfg.saveSync
                ? "[save] save detour installed; coordinated save ON"
                : "[save] save detour installed; save-edge logging ON");
        else
            coopLog("[save] FAILED to install save detour; coordinated save degraded");
    }
    // Rolling backups: the join keeps each save a transfer replaces (SaveXfer),
    // and every local write copies the folder it is about to overwrite first.
    coop::savexfer::setBackupKeep(g_cfg.saveBackups);
    g_permBuild      = g_cfg.joinCanBuild;
    g_permContainers = g_cfg.joinCanUseContainers;
    g_permBuy        = g_cfg.joinCanBuy;
    coop::engine::setPreSaveCallback(&backupBeforeSave);
    {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[save] rolling backups: keep %d per save (saveBackups)",
                  g_cfg.saveBackups);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Coordinated load (protocol 32): detour SaveManager::load so every local
    // load (menu, title screen, programmatic) logs its "[load] LOCAL-LOAD"
    // edge and queues a LoadEdge - the trigger for the host-authoritative
    // load broadcast (and the join's REQ forwarding / suppression).
    // load_probe installs it too but measures the raw world swap alone
    // (loadSync forced off - no coordination on top).
    if (g_cfg.loadSync || g_cfg.scenario == "load_probe") {
        if (coop::engine::installLoadHook())
            coopLog(g_cfg.loadSync
                ? "[load] load detour installed; coordinated load ON"
                : "[load] load detour installed; load-edge logging ON");
        else
            coopLog("[load] FAILED to install load detour; coordinated load degraded");
    }
}

// Title-screen hook. It drives THREE things at the main menu: the config
// auto-load (only when a save is set), the F2 co-op panel (go ONLINE / paste
// a Steam ID before loading), and the push-save-on-connect bootstrap (a join
// with NO save receives + loads the host's world). So install it whenever a
// save is configured OR this is an interactive session - NOT only when a save
// is present, which was the old behavior that left a join-from-menu with no
// title hook (the panel and bootstrap never ran). The scenario/test harness
// always sets a save, so its behavior is unchanged. titleUpdate_hook itself
// self-gates each of the three concerns.
void installTitleHook() {
    bool interactive = g_cfg.scenario.empty() && g_cfg.testSeconds == 0;
    if (!g_cfg.save.empty() || interactive) {
        if (KenshiLib::SUCCESS !=
            KenshiLib::AddHook(
                KenshiLib::GetRealAddress(&TitleScreen::_NV_update),
                &titleUpdate_hook, &g_titleUpdate_orig)) {
            coopErr("KenshiCoop: could not install title-screen hook (F2 panel + bootstrap + auto-load disabled)");
        } else if (!g_cfg.save.empty()) {
            std::string m = "KenshiCoop: title hook armed (auto-load save '" + g_cfg.save + "')";
            coopLog(m.c_str());
        } else {
            coopLog("KenshiCoop: title hook armed (F2 panel + push-on-connect at menu; no auto-load save)");
        }
    }
}

__declspec(dllexport) void startPlugin() {
    coop::loadConfig(g_cfg);
    // The fake clock skew must be armed BEFORE the first log line so every
    // timestamp in this run (and every time-sync packet) shares the skewed clock.
    coop::logSetFakeSkewMs(g_cfg.fakeClockSkewMs);
    coop::logInit(g_cfg.logPath.c_str(), g_cfg.isHost ? "HOST" : "JOIN");
    // The thread guards must hold from the first world load on, not from the
    // first main-loop tick (a join's first load runs before that).
    coop::engine::noteMainThread();
    {
        char b[64];
        _snprintf(b, sizeof(b) - 1, "[thread] startPlugin on thread %lu", (unsigned long)GetCurrentThreadId());
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Fault-time crash dumps, installed before anything else can fault. RE_Kenshi's
    // own dump is written from a late filter and carries no usable frames, which is
    // where the 2026-08-03 crash investigation dead-ended; ours chains to RE_Kenshi's
    // filter afterwards, so its emergency save is unaffected. See core/CrashDump.h.
    {
        std::string dir = g_cfg.logPath;
        size_t cut = dir.find_last_of("\\/");
        dir = (cut == std::string::npos) ? std::string(".") : dir.substr(0, cut);
        coop::crashdump::install(dir.c_str(), g_cfg.isHost ? "host" : "join");
    }

    logStartupBanner();

    // Per-section tick cost (core/TickProfiler.h). Armed before the hook goes in
    // so the first window covers the first tick.
    {
        LARGE_INTEGER f;
        g_perfFreq     = QueryPerformanceFrequency(&f) ? (unsigned __int64)f.QuadPart : 0;
        g_perfWindowMs = g_perfFreq ? coop::perf::windowMsFromEnv() : 0;
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[perf] tick profiler %s (window=%lums, KENSHICOOP_PERF_MS)",
                  g_perfWindowMs ? "on" : "off", g_perfWindowMs);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Hook the main-thread tick FIRST (early-return on failure), then wire the
    // replicator, engine detours and UI hooks as named install phases.
    if (!installMainLoopHook())
        return;

    configureReplicator();

    installEngineDetours();

    installTitleHook();

    if (g_cfg.testSeconds > 0) {
        char b[96];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: test mode - self-exit %d s after gameplay starts",
                  g_cfg.testSeconds);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Scenario harness: build the selected scenario (both host & join run it; it
    // branches on host/join internally). Unknown names are logged and ignored.
    // Harness/Debug only - the shipped Release DLL has no scenario runner.
#ifdef KENSHICOOP_HARNESS
    if (!g_cfg.scenario.empty()) {
        g_scenario = coop::makeScenario(g_cfg.scenario);
        if (g_scenario) {
            std::string m = "KenshiCoop: scenario armed '" + g_cfg.scenario + "'";
            coopLog(m.c_str());
        } else {
            std::string m = "KenshiCoop: unknown scenario '" + g_cfg.scenario + "'";
            coopErr(m.c_str());
        }
    }
#endif

    // Session start policy (in-game panel, 2026-07-14): the unattended harness
    // (a scenario or a self-exit timer) ALWAYS auto-starts - it never touches the
    // F2 panel. An interactive install DEFERS the session to the panel's Connect
    // unless KENSHICOOP_AUTOCONNECT=1 restores load-time auto-start from the
    // env/config role+transport+peer (the legacy launcher behaviour).
    bool autoStart = g_cfg.autoConnect || !g_cfg.scenario.empty() || g_cfg.testSeconds > 0;

    // Bring Steam up now (when the panel may be used, or Steam is configured) so
    // the panel can show THIS player's SteamID for the two-code exchange before
    // any connection is made. init() is idempotent; startNetworking re-uses it.
    // When Steam is up, also arm the invite layer so this player can RECEIVE a
    // Steam "Join Game"/invite (GameLobbyJoinRequested) even before touching the
    // panel - the joiner never has to open it or type an ID.
    if (!autoStart || g_cfg.transport == "steam" || g_cfg.steamPing != 0) {
        if (coop::steamp2p::init())
            coop::steaminvite::init(&coopUiConnect);
    }

    if (autoStart) {
        startNetworking();
    } else {
        char b[160];
        _snprintf(b, sizeof(b) - 1, "KenshiCoop: session DEFERRED - press F%d in-game to pick "
                  "role/transport and go ONLINE (panelKey)", g_cfg.panelKeyVk - 0x70 + 1);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
}
