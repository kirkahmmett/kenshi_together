// EngineUi.h - narrow PUBLIC engine surface: the in-game co-op session panel +
// status overlay. Carved out of Engine.h (Phase 5a domain split, 2026-07-19) so
// the UI root (Plugin.cpp) includes only what it needs and the sync/replication
// consumers stop transitively seeing the panel API.
//
// Like Engine.h this is a PUBLIC header: it declares only the SEH-guarded engine
// facade and must NEVER pull in a <kenshi/...> internal header - those live in
// the adapter (EngineInternal.h). Forward declarations only.

#ifndef KENSHICOOP_ENGINE_UI_H
#define KENSHICOOP_ENGINE_UI_H

namespace coop {
namespace engine {

// ---- In-game co-op session panel ---------------------------------------------
// A native DatapanelGUI opened with F2 that lets the player pick role + transport
// (buttons/checkboxes - the only reliably interactive DatapanelGUI controls;
// MyGUI comboboxes/editboxes have no usable RVAs and don't receive keyboard focus
// during gameplay) and Connect/Disconnect. The friend's Steam ID is entered by
// clipboard: "Copy my Steam ID" puts the player's own id on the clipboard to
// share, and "Paste friend's Steam ID" reads the friend's id back in (per-session,
// never written to disk). The UDP endpoint (ip/port) still comes from
// coop_config.json. The GUI layer stays session-agnostic: live status is passed IN
// via *st and the user's actions are handed BACK through the callbacks (the plugin
// root owns the session/config wiring). Main-thread only; SEH-guarded.

// Outbound Steam invite, as function pointers so this GUI layer stays free of the
// Steam/net code (the plugin root fills it from net/SteamInvite). The host opens
// a lobby + friend list with begin(), then invite(id) sends a Steam lobby invite;
// the friend accepts from Steam and both sides connect - no Steam IDs swapped.
// friendState: 0 offline, 1 online, 2 playing Kenshi (the list is sorted by it).
struct CoopInviteApi {
    bool               (*ready)();
    void               (*begin)();
    void               (*invite)(unsigned long long friendId);
    bool               (*pickerActive)();
    int                (*friendCount)();
    unsigned long long (*friendId)(int i);
    const char*        (*friendName)(int i);
    int                (*friendState)(int i);
    const char*        (*status)();   // one-line invite status ("" when idle)
    // Simple lobby flow (protocol 57): host opens a friends-only lobby once it is
    // already hosting; joins browse friends' open games and join one directly.
    void               (*hostLobby)();
    void               (*setBrowsing)(bool on);
    void               (*refreshGames)();
    int                (*gameCount)();
    const char*        (*gameName)(int i);
    void               (*joinGame)(int i);
};

struct CoopPanelState {
    unsigned long long selfSteamId; // steamp2p::selfId (0 = Steam not up)
    unsigned long long peerSteamId; // config steamPeer fallback (0 = unset; pasted id wins)
    bool               running;     // net thread up
    bool               peerPresent; // peer connected
    bool               isHost;      // current armed role (seeds the Host toggle)
    int                transportSel;// current armed transport (0 steam, 1 udp)
    const char*        detail;      // one-line status string for the panel/overlay
    // Join-side save-transfer status (null when not streaming): byte-level
    // progress the one-line detail above has no room for, shown on the F2 panel
    // while a join receives the host's world (e.g. "Streaming host world... 42%
    // (3.1/7.4 MB)"). Set by coopPanelDrive, rendered in dbgVal.
    const char*        transferDetail;
    // Outbound Steam invite (null = Steam invite layer not up). Offered only
    // while the panel is armed as HOST over STEAM and not yet connected.
    const CoopInviteApi* invite;
    // Virtual-key code that toggles the panel (Config::panelKeyVk; 0 = F2).
    int                panelVk;
    // Last connection error worth showing the player (null/"" = none), e.g. a
    // protocol mismatch. Shown on the panel's status line while offline.
    const char*        lastError;
    // Host settings for the join (protocol 56): current values, shown as three
    // switches while the panel is armed as HOST; a press calls onPermToggle
    // (0 build, 1 storage containers, 2 buying). Null callback = rows hidden.
    bool               permBuild, permContainers, permBuy;
    bool               permTake;   // v58: players may take from each other (onPermToggle(3))
    void               (*onPermToggle)(int which);
    // Simple view: in a loaded game (hosting needs a world), and the session's
    // players as one display line ("Ali (host), Veli"; null/"" = unknown).
    bool               inGame;
    const char*        players;
    // "Always host my games": when on, the plugin opens every loaded game to
    // friends by itself. The panel shows it as a switch.
    bool               autoHost;
    void               (*onAutoHostToggle)();
    // Host: game paused for everyone, and the button that toggles it.
    bool               gamePaused;
    void               (*onPauseToggle)();
    // New game settings (lobby view): what a NEW game starts with, as display
    // text ("Start default", "50,000 cats"); a press calls onNewGameCycle
    // (0 money, 1 stats) to step to the next preset. Null callback = rows hidden.
    const char*        newGameMoney;
    const char*        newGameStats;
    void               (*onNewGameCycle)(int which);
    // Character editor (v60): in a session, a button that opens Kenshi's editor
    // on your own character (null = hidden); lookEditing while it is open.
    void               (*onEditLook)();
    bool               lookEditing;
    // "Resync" (in a session: everyone reloads the host's live world) and
    // "Save a problem report" (logs to a Desktop folder). Null = hidden.
    void               (*onResync)();
    void               (*onReport)();
    // Consent trade (v62): requests waiting for OUR answer, as display lines;
    // a press calls onTradeAnswer(index, accept). Null callback = hidden.
    int                tradeCount;
    const char*        tradeText[4];
    void               (*onTradeAnswer)(int index, bool accept);
    // Shared pings (v64): "Ping where I look" (null = hidden); pingKey names the hotkey.
    void               (*onPing)();
    const char*        pingKey;
};
// The panel's role/transport selections at the moment Connect is hit. peerId is the
// Steam ID pasted in-panel this session (0 if none), and overrides the config
// steamPeer in coopUiConnect; the UDP endpoint is re-read from the config there.
typedef void (*CoopConnectFn)(bool isHost, bool useSteam, unsigned long long peerId);
typedef void (*CoopDisconnectFn)();
void coopPanelTick(const CoopPanelState* st, CoopConnectFn onConnect,
                   CoopDisconnectFn onDisconnect);

// Persistent co-op connection-status banner: a single screen-space label fixed 10
// px in from the top-left corner (a createFloatingLabel MyGUI::Window on the
// spike-48 screenshot-proven "Info" layer) whose caption shows the live session
// status, colored by state (0 = offline/red, 1 = waiting/yellow, 2 =
// connected/green). Needs no player character, so it also shows at the title
// screen; updated in place when the text/state changes and re-minted if the GUI
// destroyed the widget (world load). Pass show=false to remove it. Main-thread
// only; SEH-guarded.
void coopOverlayTick(const char* text, int state, bool show);

// Rebuild the main bar's squad tabs (their captions are cached) - call after
// renaming a squad.
void refreshSquadTabs();

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_UI_H
