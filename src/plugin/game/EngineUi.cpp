// EngineUi.cpp - the in-game co-op UI plane: the debug marker HUD labels, the
// F2 co-op session panel (native DatapanelGUI), and the persistent status
// overlay. Split out of EngineEntity.cpp (Phase 5e code motion, 2026-07-19) so
// the entity capture/resolve/apply TU stays focused on sync and the UI/render
// surface lives with its public header (EngineUi.h).
//
// Owner state: section-private statics / anon-namespace SEH shims only (marker
// colour + create/update/destroy shims, panel widget pointers, overlay label).
// Must NOT: define g_* engine pointers (EngineInternal.cpp owns them), install
// hooks, or change any log string - "[coop-ui] ..." phrasing is API consumed by
// the harness. The public marker* declarations stay in Engine.h (the Replicator
// uses them for KENSHICOOP_DEBUG_MARKERS); only their definitions moved here.

#include "EngineInternal.h"

// In-game co-op session panel: the native DatapanelGUI window + its interactive
// rows and Win32 key capture. EngineInternal.h already pulls Globals.h (::gui),
// ForgottenGUI.h and MyGUI_Button.h; these add the panel row types, the
// free-function delegate factory, and GetAsyncKeyState/VK_* for the F2 toggle +
// digit entry.
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <kenshi/gui/MainBarGUI.h>
#include <mygui/MyGUI_Delegate.h> // MyGUI::newDelegate + CDelegate* (free-fn callbacks)
#include <windows.h>

#include "../core/SteamId.h" // parseSteamId64 (paste button) + maskSteamId64 (id rows)

namespace coop {
namespace engine {

// ---- Debug marker HUD labels (KENSHICOOP_DEBUG_MARKERS, spike-47 substrate) --
// ForgottenGUI::createScreenLabel + ScreenLabel::setTracking pin a colored text
// label to a character; the engine's own per-frame projection keeps it on the
// body (spike 47 render proof). The Replicator uses these to make join-side
// authority states self-explaining on screen: who is host-driven, who is
// hidden, who is a local-only ghost. C2712 split: the outer fns build the
// std::string/Colour/Vector3 (unwindable), POD-only inner fns hold the SEH.

namespace {

void markerColour(int colorId, MyGUI::Colour* col) {
    switch (colorId) {
    case 0:  *col = MyGUI::Colour(0.30f, 1.00f, 0.30f, 1.0f); break; // driven
    case 1:  *col = MyGUI::Colour(1.00f, 0.25f, 0.25f, 1.0f); break; // hidden
    case 2:  *col = MyGUI::Colour(1.00f, 0.90f, 0.25f, 1.0f); break; // local-only
    default: *col = MyGUI::Colour(0.80f, 0.80f, 0.80f, 1.0f); break;
    }
}

ScreenLabel* markerCreateSeh(ForgottenGUI* g, Character* c,
                             const std::string* text, const MyGUI::Colour* col,
                             const Ogre::Vector3* off) {
    __try {
        ScreenLabel* l = g->createScreenLabel(*text, *col, ScreenLabel::LS_SMALL,
                                              ScreenLabel::RS_STOPPED);
        if (l) {
            l->_NV_setRisingSpeed(ScreenLabel::RS_STOPPED);
            l->_NV_setTracking(c->handle, *off);
        }
        return l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool markerUpdateSeh(ScreenLabel* l, const std::string* text,
                     const MyGUI::Colour* col) {
    __try {
        l->_NV_setCaption(*text);
        l->_NV_setColor(*col);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Is this label one the GUI still owns? ForgottenGUI keeps the authoritative
// registry of live ScreenLabels (plus deferred add/remove queues), and that
// registry - not our cached pointer - is the only ground truth available: the
// GUI destroys labels on its own schedule and notifies nobody, so a handle held
// across ticks dangles silently. Measured 2026-08-03: the join minted three
// proxies in one burst, debugMark found stale map entries sitting at RECYCLED
// Character* addresses, and setCaption on the previous occupants' dead labels
// faulted twice inside ScreenLabel::setCaption (kenshi+0x6e451e) moments before
// the process died. Same lesson as the world-item proxy hands - never
// dereference a cached engine pointer without re-asking the engine.
//
// Deliberately unlocked. guiScreenLabelsMutex guards these lektors, but taking
// an engine shared_mutex from inside a detour is its own class of hazard, and a
// torn read here is harmless BECAUSE we test for an exact pointer match: garbage
// answers "not present", which costs one discarded marker, whereas a false
// "alive" would need the torn word to equal the very pointer we are asking
// about. The safe direction is the likely one.
bool labelListHas(const lektor<ScreenLabelInterface*>* v, const void* l) {
    __try {
        ScreenLabelInterface* const* p = v->stuff;
        unsigned int n = v->count;
        if (!p || n > 8192u) return false;   // bound a torn/garbage count
        for (unsigned int i = 0; i < n; ++i)
            if ((const void*)p[i] == l) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool markerDestroySeh(ForgottenGUI* g, ScreenLabel* l) {
    __try {
        g->destroy(l);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

void* markerCreate(Character* c, const char* text, int colorId) {
    if (!c || !text) return 0;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return 0;
    std::string t(text);
    MyGUI::Colour col;
    markerColour(colorId, &col);
    Ogre::Vector3 off(0.0f, 2.2f, 0.0f); // head height (spike 47)
    return markerCreateSeh(g, c, &t, &col, &off);
}

bool markerAlive(void* label) {
    if (!label) return false;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return false;
    // Queued for removal counts as dead: the GUI has already decided, and we
    // would only be racing its next flush.
    if (labelListHas(&g->guiScreenLabelsToRemove, label)) return false;
    // A freshly created label sits in the add queue until the GUI flushes it, so
    // both lists are "alive" or markerCreate's own handle would look dead.
    return labelListHas(&g->guiScreenLabels, label) ||
           labelListHas(&g->guiScreenLabelsToAdd, label);
}

bool markerUpdate(void* label, const char* text, int colorId) {
    if (!label || !text || !markerAlive(label)) return false;
    std::string t(text);
    MyGUI::Colour col;
    markerColour(colorId, &col);
    return markerUpdateSeh((ScreenLabel*)label, &t, &col);
}

void markerDestroy(void* label) {
    if (!label) return;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return;
    // Handing the GUI a label it has already destroyed is the same
    // use-after-free as updating one; prune paths reach here with handles whose
    // Character went away, which is exactly when the GUI has cleaned up too.
    if (!markerAlive(label)) return;
    markerDestroySeh(g, (ScreenLabel*)label);
}

// ---- In-game co-op session panel (config-driven, spike-50 DatapanelGUI stack) -
// A native DatapanelGUI window toggled with F2. The player picks role + transport
// (toggle BUTTONS - the only DatapanelGUI control with a callable RVA callback;
// MyGUI comboboxes/editboxes have no reachable getters and never receive keyboard
// focus during gameplay) and connects/leaves via a bound checkbox. The friend code
// (peer SteamID) + UDP endpoint come from coop_config.json and are shown READ-ONLY;
// a "Copy my Steam ID" button puts the player's own id on the clipboard to share.
// The GUI layer is session-agnostic: live status arrives via *st; the user's
// actions leave via the onConnect/onDisconnect callbacks (the plugin root owns the
// net/session/config wiring).
//
// SEH discipline (spike 47/48): the mutation calls take std::string by const-ref
// or PODs, so they all sit inside one __try, provided NO std::string temporary is
// constructed in that frame. The one exception is createDatapanel's BY-VALUE
// std::string 'layer' arg (an unwindable temporary => C2712), so the window is
// created in the outer, non-SEH function; ::gui is verified non-null first and the
// createScreenLabel/createFloatingLabel factory family is render-proven (46-48).

namespace {

// Write a UTF-8/ANSI string to the Windows clipboard (CF_TEXT). Mirror of the
// paste-read: OpenClipboard -> EmptyClipboard -> GlobalAlloc+copy -> SetClipboardData
// -> CloseClipboard. Used by the "Copy my Steam ID" button. Win32 only (no MyGUI).
bool clipboardSetText(const char* text) {
    if (!text) return false;
    size_t n = strlen(text);
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n + 1);
        if (h) {
            char* dst = (char*)GlobalLock(h);
            if (dst) {
                memcpy(dst, text, n);
                dst[n] = '\0';
                GlobalUnlock(h);
                if (SetClipboardData(CF_TEXT, h)) ok = true; // clipboard now owns h
            }
            if (!ok) GlobalFree(h); // ownership not transferred on failure
        }
    }
    CloseClipboard();
    return ok;
}

// Read text from the Windows clipboard into out. Prefers CF_UNICODETEXT (what the
// Steam overlay / browsers usually publish) and falls back to CF_TEXT, converting
// either to a narrow std::string (the SteamID parse keeps only ASCII digits, so a
// lossy WideCharToMultiByte is fine here). Used by the "Paste friend's Steam ID"
// button. Win32 only (no MyGUI). Returns true iff some text was retrieved.
bool clipboardGetText(std::string& out) {
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    HANDLE hw = GetClipboardData(CF_UNICODETEXT);
    if (hw) {
        const wchar_t* src = (const wchar_t*)GlobalLock(hw);
        if (src) {
            int need = WideCharToMultiByte(CP_UTF8, 0, src, -1, 0, 0, 0, 0);
            if (need > 0) {
                std::string tmp((size_t)need, '\0');
                if (WideCharToMultiByte(CP_UTF8, 0, src, -1, &tmp[0], need, 0, 0) > 0) {
                    if (!tmp.empty() && tmp[tmp.size() - 1] == '\0') tmp.resize(tmp.size() - 1);
                    out = tmp;
                    ok = true;
                }
            }
            GlobalUnlock(hw);
        }
    }
    if (!ok) {
        HANDLE ha = GetClipboardData(CF_TEXT);
        if (ha) {
            const char* src = (const char*)GlobalLock(ha);
            if (src) { out = src; ok = true; GlobalUnlock(ha); }
        }
    }
    CloseClipboard();
    return ok;
}

struct CoopPanelUi {
    DatapanelGUI* panel;
    bool          open, built;
    bool          hostFlag;      // true = HOST role armed
    bool          steamFlag;     // true = Steam transport armed (else UDP)
    bool          connectedFlag; // desired connection state (Online/Offline toggle)
    bool          lastConnected; // last observed st->running (external-change sync)
    bool          lastChkVal;    // last toggle value (connect/disconnect edge)
    bool          needsRebuild;
    bool          advanced;      // the old role/transport/Steam-ID view
    bool          hostSettings;  // host view: permission switches expanded
    bool          toggleRequest; // a button asked to open/close the panel
    bool          f2Down;        // F2 held last tick (rising-edge toggle)
    std::string   lastStatus;    // last status text shown (refresh gate)
    std::string   lastTransfer;  // last save-transfer line shown (refresh gate)
    CoopPanelUi()
        : panel(0), open(false), built(false), hostFlag(true), steamFlag(true),
          connectedFlag(false), lastConnected(false), lastChkVal(false),
          needsRebuild(false), advanced(false), hostSettings(false),
          toggleRequest(false), f2Down(false) {}
};

CoopPanelUi             g_panel;
DataPanelLine_Button*   g_roleBtn      = 0;
DataPanelLine_Button*   g_transBtn     = 0;
DataPanelLine_Button*   g_connBtn      = 0; // Online/Offline toggle (replaces the checkbox)
DataPanelLine_Button*   g_copyIdBtn    = 0;
DataPanelLine_Button*   g_pasteIdBtn   = 0; // "Paste friend's Steam ID" from clipboard
DataPanelLine*          g_debugLine    = 0; // white connection-status debug row
DataPanelLine*          g_peerLine     = 0; // white "Friend's Steam ID" row
DataPanelLine*          g_selfLine     = 0; // white "Your Steam ID" row
std::string             g_selfIdStr;   // self SteamID as digits (set each tick; "" = none)

// Friend's SteamID pasted in-panel this session (0 = none). Per-session by
// design: it lives only in memory, so relaunching Kenshi clears it and the
// friend's id is re-pasted (nothing is written to disk). Passed to onConnect,
// where it overrides the (usually empty) config steamPeer.
unsigned long long      g_pastedPeer   = 0;
bool                    g_pasteFailed  = false; // last paste wasn't a valid Steam ID

// Outbound Steam invite rows (HOST + STEAM + offline only). The friend list is
// BUTTONS, one per friend: the in-panel picker this replaces was a MyGUI list,
// and MyGUI lists/editboxes never take focus during gameplay - buttons are the
// one DatapanelGUI control proven to click (see the panel note above).
const int               INVITE_ROWS_MAX = 8;
const CoopInviteApi*    g_inviteApi    = 0; // set each tick from CoopPanelState
DataPanelLine_Button*   g_inviteBtn    = 0; // "Invite a Steam friend"
DataPanelLine*          g_inviteLine   = 0; // white invite-status row
DataPanelLine_Button*   g_friendBtns[INVITE_ROWS_MAX];
unsigned long long      g_friendBtnIds[INVITE_ROWS_MAX];
std::string             g_lastInviteSig;    // rebuild gate for the invite rows

// Host settings for the join (protocol 56): three switches, HOST role only.
DataPanelLine_Button*   g_permBtns[3]  = { 0, 0, 0 };
void                  (*g_permToggleFn)(int) = 0;
int                     g_lastPermSig  = -1; // rebuild gate (bit per switch)

// Simple view (protocol 57): Host for friends / Join a friend / Leave, with the
// old role/transport/Steam-ID rows one click away under Advanced.
const int               GAME_ROWS_MAX  = 8;
CoopConnectFn           g_onConnectFn    = 0; // set each tick (button callbacks)
CoopDisconnectFn        g_onDisconnectFn = 0;
DataPanelLine_Button*   g_hostBtn      = 0;
DataPanelLine_Button*   g_refreshBtn   = 0;
DataPanelLine_Button*   g_advBtn       = 0; // simple -> advanced
DataPanelLine_Button*   g_simpleBtn    = 0; // advanced -> simple
DataPanelLine_Button*   g_leaveBtn     = 0;
DataPanelLine_Button*   g_gameBtns[GAME_ROWS_MAX];
std::string             g_lastSimpleSig;
DataPanelLine_Button*   g_hostSetBtn   = 0; // host view: show/hide permission switches
DataPanelLine_Button*   g_autoHostBtn  = 0; // "Always host my games"
void                  (*g_autoHostToggleFn)() = 0;
// Main-menu launcher: a small window by Kenshi's own menu, so nobody has to
// know about F2. "Multiplayer" opens the panel; with exactly one friend
// hosting, a second button joins them in one click.
DatapanelGUI*           g_launch       = 0;
DataPanelLine_Button*   g_launchBtn    = 0;
DataPanelLine_Button*   g_quickJoinBtn = 0;
std::string             g_lastLaunchSig;

// Button callbacks (free functions - MyGUI::newDelegate wraps them without any
// raw-MyGUI link). A press flips the armed flag and requests a rebuild so the
// caption reflects the new choice on the next tick.
void onRoleBtn(DataPanelLine*) {
    g_panel.hostFlag = !g_panel.hostFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.hostFlag ? "[coop-ui] role -> Host" : "[coop-ui] role -> Join");
}
void onTransBtn(DataPanelLine*) {
    g_panel.steamFlag = !g_panel.steamFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.steamFlag ? "[coop-ui] transport -> Steam" : "[coop-ui] transport -> UDP");
}
// Online/Offline toggle: flip the desired connection state. The connect/disconnect
// edge (connectedFlag vs lastChkVal) is handled in coopPanelTick, same as before.
void onConnBtn(DataPanelLine*) {
    g_panel.connectedFlag = !g_panel.connectedFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.connectedFlag ? "[coop-ui] connection -> ONLINE"
                                        : "[coop-ui] connection -> OFFLINE");
}
// Copy the player's own SteamID to the clipboard so they can paste it to a friend
// (who pastes it into their panel via "Paste friend's Steam ID").
void onCopyIdBtn(DataPanelLine*) {
    if (g_selfIdStr.empty()) {
        coop::logLine("[coop-ui] copy Steam ID: none (Steam not running)");
        return;
    }
    bool ok = clipboardSetText(g_selfIdStr.c_str());
    char b[64];
    _snprintf(b, sizeof(b) - 1, "[coop-ui] copied Steam ID to clipboard: %s",
              ok ? "ok" : "FAILED");
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}
// Paste the friend's SteamID from the clipboard: read text, extract + validate a
// SteamID64, and store it as the session peer (used on the next Connect). No
// typing, no config edit. Rejects arbitrary clipboard junk (g_pasteFailed drives
// the peer-row hint).
void onPasteIdBtn(DataPanelLine*) {
    std::string clip;
    unsigned long long id = 0;
    if (clipboardGetText(clip) && coop::parseSteamId64(clip, id)) {
        g_pastedPeer  = id;
        g_pasteFailed = false;
        char b[64];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] paste friend id=%llu ok=1", id);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    } else {
        g_pasteFailed = true;
        coop::logLine("[coop-ui] paste friend id=0 ok=0 (clipboard not a Steam ID)");
    }
    g_panel.needsRebuild = true;
}
// One callback for the three permission switches: the line says which.
void onPermBtn(DataPanelLine* line) {
    for (int i = 0; i < 3; ++i) {
        if (g_permBtns[i] && (DataPanelLine*)g_permBtns[i] == line) {
            if (g_permToggleFn) g_permToggleFn(i);
            g_panel.needsRebuild = true;
            return;
        }
    }
}
// Open the host's lobby + friend list.
void onInviteBtn(DataPanelLine*) {
    if (!g_inviteApi || !g_inviteApi->begin) return;
    g_inviteApi->begin();
    coop::logLine("[coop-ui] invite: friend list opened");
    g_panel.needsRebuild = true;
}
// Simple view actions.
void onLaunchBtn(DataPanelLine*) {
    g_panel.toggleRequest = true;
    coop::logLine("[coop-ui] Multiplayer button");
}
void onQuickJoinBtn(DataPanelLine*) {
    if (!g_inviteApi || !g_inviteApi->joinGame) return;
    g_panel.hostFlag = false; g_panel.steamFlag = true;
    coop::logLine("[coop-ui] quick join");
    g_inviteApi->joinGame(0);
}
void onHostSetBtn(DataPanelLine*) {
    g_panel.hostSettings = !g_panel.hostSettings;
    g_panel.needsRebuild = true;
}
void onAutoHostBtn(DataPanelLine*) {
    if (g_autoHostToggleFn) g_autoHostToggleFn();
    g_panel.needsRebuild = true;
}
void onHostBtn(DataPanelLine*) {
    g_panel.hostFlag = true; g_panel.steamFlag = true;
    g_panel.connectedFlag = true; g_panel.lastChkVal = true; // no second connect edge
    g_panel.needsRebuild = true;
    coop::logLine("[coop-ui] Host for friends");
    if (g_onConnectFn) g_onConnectFn(true, true, 0);
    if (g_inviteApi && g_inviteApi->hostLobby) g_inviteApi->hostLobby();
}
void onGameBtn(DataPanelLine* line) {
    if (!g_inviteApi || !g_inviteApi->joinGame) return;
    for (int i = 0; i < GAME_ROWS_MAX; ++i) {
        if (g_gameBtns[i] && (DataPanelLine*)g_gameBtns[i] == line) {
            g_panel.hostFlag = false; g_panel.steamFlag = true;
            char b[64];
            _snprintf(b, sizeof(b) - 1, "[coop-ui] Join a friend: row %d", i);
            b[sizeof(b) - 1] = '\0';
            coop::logLine(b);
            g_inviteApi->joinGame(i); // connects once Steam lets us into the lobby
            g_panel.needsRebuild = true;
            return;
        }
    }
}
void onRefreshBtn(DataPanelLine*) {
    if (g_inviteApi && g_inviteApi->refreshGames) g_inviteApi->refreshGames();
    g_panel.needsRebuild = true;
}
void onAdvBtn(DataPanelLine*) {
    g_panel.advanced = !g_panel.advanced;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.advanced ? "[coop-ui] advanced view" : "[coop-ui] simple view");
}
void onLeaveBtn(DataPanelLine*) {
    g_panel.connectedFlag = false; g_panel.lastChkVal = false;
    g_panel.needsRebuild = true;
    coop::logLine("[coop-ui] Leave / stop hosting");
    if (g_onDisconnectFn) g_onDisconnectFn();
}

// One callback for every friend row: the line pointer says which friend.
void onFriendBtn(DataPanelLine* line) {
    if (!g_inviteApi || !g_inviteApi->invite) return;
    for (int i = 0; i < INVITE_ROWS_MAX; ++i) {
        if (g_friendBtns[i] && (DataPanelLine*)g_friendBtns[i] == line && g_friendBtnIds[i]) {
            g_inviteApi->invite(g_friendBtnIds[i]);
            char b[80];
            _snprintf(b, sizeof(b) - 1, "[coop-ui] invite: sent to row %d", i);
            b[sizeof(b) - 1] = '\0';
            coop::logLine(b);
            g_panel.needsRebuild = true;
            return;
        }
    }
}

// Simple view rows: a flat list built in one SEH frame (no std::string made in
// it). kind 0 = text line, 1 = button, 2 = spacer. out receives the created row.
struct SimpleRow {
    int                 kind;
    const std::string*  key;
    const std::string*  val;   // text value / button caption
    DataPanelLine**     out;
};
const int SIMPLE_ROWS_MAX = 40;

void panelBuildSimpleSeh(DatapanelGUI* p, const std::string* title,
                         const SimpleRow* rows, int n, const std::string* empty) {
    __try {
        p->_NV_clear();
        p->setCaption(*title);
        for (int i = 0; i < n; ++i) {
            DataPanelLine* made = 0;
            if (rows[i].kind == 0)
                made = p->setLine(*rows[i].key, *rows[i].val, *empty, 0, false, true);
            else if (rows[i].kind == 1)
                made = (DataPanelLine*)p->setLineButton(*rows[i].key, *rows[i].val, 0);
            else
                p->addSpace(0, 0.35f);
            if (rows[i].out) *rows[i].out = made;
        }
        p->_NV_update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// POD-only pointer bundle so the row-build SEH frame constructs no std::string.
struct PanelStrings {
    const std::string *title, *roleKey, *roleCap, *transKey, *transCap;
    const std::string *connKey, *connCap;
    const std::string *dbgKey, *dbgVal;
    const std::string *peerKey, *peerVal, *pasteKey, *pasteCap;
    const std::string *selfKey, *selfVal, *copyKey, *copyCap;
    const std::string *empty;
    // Outbound invite (all null/0 when not offered).
    const std::string *inviteKey, *inviteCap;       // "Invite a Steam friend" button
    const std::string *invStatKey, *invStatVal;     // status row
    int                friendRows;                  // 0..INVITE_ROWS_MAX
    const std::string *friendKey[INVITE_ROWS_MAX];
    const std::string *friendCap[INVITE_ROWS_MAX];
    // Host settings for the join (null = rows hidden).
    const std::string *permKey[3];
    const std::string *permCap[3];
    // "Simple view" button at the top of the advanced view.
    const std::string *backKey, *backCap;
};

void panelBuildSeh(DatapanelGUI* p, const PanelStrings* s) {
    __try {
        p->_NV_clear();
        p->setCaption(*s->title);
        g_simpleBtn = 0;
        if (s->backKey) {
            g_simpleBtn = p->setLineButton(*s->backKey, *s->backCap, 0);
            p->addSpace(0, 0.35f);
        }
        g_roleBtn  = p->setLineButton(*s->roleKey,  *s->roleCap,  0);
        g_transBtn = p->setLineButton(*s->transKey, *s->transCap, 0);
        g_connBtn  = p->setLineButton(*s->connKey,  *s->connCap,  0);
        p->addSpace(0, 0.35f);
        // Host settings for the join: three switches, directly under the
        // session toggles because they are part of how the host runs it.
        for (int i = 0; i < 3; ++i) g_permBtns[i] = 0;
        if (s->permKey[0]) {
            for (int i = 0; i < 3; ++i)
                g_permBtns[i] = p->setLineButton(*s->permKey[i], *s->permCap[i], 0);
            p->addSpace(0, 0.35f);
        }
        // Outbound Steam invite: the button opens the list, each friend row
        // invites that friend. Above the ID rows because it replaces them.
        g_inviteBtn = 0; g_inviteLine = 0;
        for (int i = 0; i < INVITE_ROWS_MAX; ++i) g_friendBtns[i] = 0;
        if (s->inviteKey) {
            g_inviteBtn = p->setLineButton(*s->inviteKey, *s->inviteCap, 0);
            if (s->invStatKey)
                g_inviteLine = p->setLine(*s->invStatKey, *s->invStatVal, *s->empty, 0, false, true);
            for (int i = 0; i < s->friendRows && i < INVITE_ROWS_MAX; ++i)
                g_friendBtns[i] = p->setLineButton(*s->friendKey[i], *s->friendCap[i], 0);
            p->addSpace(0, 0.35f);
        }
        // Connection-status debug line (coloured white below, outside SEH).
        g_debugLine = p->setLine(*s->dbgKey, *s->dbgVal, *s->empty, 0, false, true);
        p->addSpace(0, 0.35f);
        // Friend's SteamID: pasted in-panel (Copy on their side -> Paste here).
        g_peerLine = p->setLine(*s->peerKey, *s->peerVal, *s->empty, 0, false, true);
        g_pasteIdBtn = p->setLineButton(*s->pasteKey, *s->pasteCap, 0);
        p->addSpace(0, 0.35f);
        g_selfLine = p->setLine(*s->selfKey, *s->selfVal, *s->empty, 0, false, true);
        g_copyIdBtn = p->setLineButton(*s->copyKey, *s->copyCap, 0);
        p->_NV_update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Colour a line's key + value TextBoxes for readability. Runs AFTER
// panelBuildSeh's _NV_update (the w1/w2 widgets exist by then). MyGUI::Colour is a
// trivial 4-float struct (no destructor), so it may live in the SEH frame.
// yellow=true tints the value column amber - used for the join's live
// "Streaming host world..." transfer line so it reads as in-progress activity.
void dbgColourSeh(DataPanelLine* line, bool yellow) {
    if (!line) return;
    __try {
        MyGUI::Colour white(1.0f, 1.0f, 1.0f, 1.0f);
        MyGUI::Colour amber(1.0f, 0.82f, 0.20f, 1.0f);
        if (line->w1) line->w1->setTextColour(white);
        if (line->w2) line->w2->setTextColour(yellow ? amber : white);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Arm a freshly-minted panel: register it for ForgottenGUI's per-frame refresh
// AND make it visible. createDatapanel returns a built-but-hidden window; without
// this pair the F2 toggle logs open/close yet nothing ever draws (the render bug
// in the reconstruction). PODs only, so the whole thing sits in one SEH frame.
bool uiPanelArmSeh(ForgottenGUI* g, DatapanelGUI* p) {
    if (!g || !p) return false;
    __try {
        g->addDatapanelToUpdateList(p);
        p->_NV_show(true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void panelDestroySeh(ForgottenGUI* g, DatapanelGUI* p) {
    if (!g || !p) return;
    // Pull it off the refresh list BEFORE destroying so ForgottenGUI never
    // dereferences the freed panel on the next frame.
    __try {
        g->removeDatapanelFromUpdateList(p);
        g->destroy(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace

void coopPanelTick(const CoopPanelState* st, CoopConnectFn onConnect,
                   CoopDisconnectFn onDisconnect) {
    if (!st) return;
    g_inviteApi = st->invite;
    g_permToggleFn = st->onPermToggle;
    g_onConnectFn = onConnect;
    g_onDisconnectFn = onDisconnect;
    g_autoHostToggleFn = st->onAutoHostToggle;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    { static void* s_last = (void*)-1;
      if ((void*)g != s_last) { s_last = (void*)g;
          char b[64]; _snprintf(b, sizeof(b) - 1, "[coop-ui] gui ptr=%p", (void*)g);
          b[sizeof(b) - 1] = '\0'; coop::logLine(b); } }
    if (!g) return;

    // Cache the self id as a string for the Copy button (used by onCopyIdBtn).
    if (st->selfSteamId) {
        char b[32];
        _snprintf(b, sizeof(b) - 1, "%llu", (unsigned long long)st->selfSteamId);
        b[sizeof(b) - 1] = '\0';
        g_selfIdStr = b;
    } else {
        g_selfIdStr.clear();
    }

    // Panel key (F2 unless configured) rising edge toggles the panel open/closed.
    const int panelVk = (st->panelVk >= VK_F1 && st->panelVk <= VK_F12) ? st->panelVk : VK_F2;
    // GetAsyncKeyState is system-wide: only count the key while OUR window is
    // in front, or F2 in another program (or another Kenshi) toggles this panel.
    DWORD fgPid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    bool f2 = fgPid == GetCurrentProcessId() && (GetAsyncKeyState(panelVk) & 0x8000) != 0;
    // Test aid: KENSHICOOP_PANEL_OPEN=1 opens the panel once at start, so a
    // screenshot needs no key press (and no stolen focus).
    { static int s_autoOpen = -1;
      if (s_autoOpen < 0) { char e[4]; s_autoOpen = (GetEnvironmentVariableA("KENSHICOOP_PANEL_OPEN", e, sizeof(e)) > 0 && e[0] == '1') ? 1 : 0;
                            if (s_autoOpen) g_panel.toggleRequest = true; } }
    const bool toggle = (f2 && !g_panel.f2Down) || g_panel.toggleRequest;
    g_panel.toggleRequest = false;
    if (toggle) {
        if (!g_panel.open) {
            g_panel.hostFlag      = st->isHost;
            g_panel.steamFlag     = (st->transportSel == 0);
            g_panel.connectedFlag = st->running;
            g_panel.lastConnected = st->running;
            g_panel.lastChkVal    = st->running;
            g_panel.open = true;
            g_panel.needsRebuild = true;
            coop::logLine("[coop-ui] panel opened");
        } else {
            panelDestroySeh(g, g_panel.panel);
            g_panel.panel = 0; g_panel.built = false;
            g_roleBtn = 0; g_transBtn = 0; g_connBtn = 0; g_copyIdBtn = 0;
            g_pasteIdBtn = 0;
            g_debugLine = 0; g_peerLine = 0; g_selfLine = 0;
            g_inviteBtn = 0; g_inviteLine = 0;
            for (int i = 0; i < INVITE_ROWS_MAX; ++i) g_friendBtns[i] = 0;
            g_lastInviteSig.clear();
            for (int i = 0; i < 3; ++i) g_permBtns[i] = 0;
            g_lastPermSig = -1;
            g_hostBtn = 0; g_refreshBtn = 0; g_advBtn = 0; g_simpleBtn = 0; g_leaveBtn = 0;
            g_hostSetBtn = 0; g_autoHostBtn = 0;
            for (int i = 0; i < GAME_ROWS_MAX; ++i) g_gameBtns[i] = 0;
            g_lastSimpleSig.clear();
            g_panel.open = false;
            coop::logLine("[coop-ui] panel closed");
        }
    }
    g_panel.f2Down = f2;

    // ---- Main-menu launcher -------------------------------------------------
    const bool wantLauncher = !st->inGame && !st->running && !g_panel.open;
    // "Join a friend" list refresh runs only while something shows it.
    if (g_inviteApi && g_inviteApi->setBrowsing)
        g_inviteApi->setBrowsing(((g_panel.open && !g_panel.advanced) || wantLauncher) &&
                                 !st->running);
    if (wantLauncher) {
        if (!g_launch) {
            std::string layer = "Info";
            g_launch = g->createDatapanel(0.05f, 0.40f, 0.22f, 0.13f, false, layer, true); // (top, left, w, h): above the logo
            g_lastLaunchSig.clear();
            if (g_launch && !uiPanelArmSeh(g, g_launch)) g_launch = 0;
        }
        const bool apiOk = g_inviteApi && g_inviteApi->ready && g_inviteApi->ready();
        int games = (apiOk && g_inviteApi->gameCount) ? g_inviteApi->gameCount() : 0;
        std::string one = (games == 1) ? std::string(g_inviteApi->gameName(0)) : std::string();
        char sb[32];
        _snprintf(sb, sizeof(sb) - 1, "%d|", games);
        sb[sizeof(sb) - 1] = '\0';
        std::string sig = std::string(sb) + one;
        if (g_launch && sig != g_lastLaunchSig) {
            char cap[48];
            _snprintf(cap, sizeof(cap) - 1, "Multiplayer    (F%d)", panelVk - VK_F1 + 1);
            cap[sizeof(cap) - 1] = '\0';
            std::string title = "KenshiCoop", empty = "";
            std::string k0 = "mp", v0 = cap, k1 = "qj", v1, k2 = "Hosting", v2;
            SimpleRow rows[3];
            int n = 0;
            g_launchBtn = 0; g_quickJoinBtn = 0;
            rows[n].kind = 1; rows[n].key = &k0; rows[n].val = &v0;
            rows[n].out = (DataPanelLine**)&g_launchBtn; ++n;
            if (games == 1) {
                v1 = "Join  " + one + "'s game";
                rows[n].kind = 1; rows[n].key = &k1; rows[n].val = &v1;
                rows[n].out = (DataPanelLine**)&g_quickJoinBtn; ++n;
            } else if (games > 1) {
                char gb[64];
                _snprintf(gb, sizeof(gb) - 1, "%d friends are hosting - open Multiplayer", games);
                gb[sizeof(gb) - 1] = '\0';
                v2 = gb;
                rows[n].kind = 0; rows[n].key = &k2; rows[n].val = &v2; rows[n].out = 0; ++n;
            }
            panelBuildSimpleSeh(g_launch, &title, rows, n, &empty);
            if (g_launchBtn)    g_launchBtn->callback    = MyGUI::newDelegate(&onLaunchBtn);
            if (g_quickJoinBtn) g_quickJoinBtn->callback = MyGUI::newDelegate(&onQuickJoinBtn);
            g_lastLaunchSig = sig;
        }
    } else if (g_launch) {
        panelDestroySeh(g, g_launch);
        g_launch = 0; g_launchBtn = 0; g_quickJoinBtn = 0;
        g_lastLaunchSig.clear();
    }

    if (!g_panel.open) return;

    // Keep the Online/Offline toggle honest when the session state changes
    // underneath us (a peer-driven connect, a failed connect that stopped, etc):
    // resync the desired flag to the real state and rebuild so the button caption
    // + debug line reflect it.
    if (st->running != g_panel.lastConnected) {
        g_panel.lastConnected = st->running;
        g_panel.connectedFlag = st->running;
        g_panel.lastChkVal    = st->running;
        g_panel.needsRebuild = true;
    }

    std::string detail = st->detail ? std::string(st->detail) : std::string();
    if (detail != g_panel.lastStatus) g_panel.needsRebuild = true;

    // Join save-transfer line (throttled to whole-percent by the caller): rebuild
    // when it changes so the "Streaming host world... NN%" line advances live.
    std::string transfer = st->transferDetail ? std::string(st->transferDetail)
                                               : std::string();
    if (transfer != g_panel.lastTransfer) g_panel.needsRebuild = true;

    // Outbound invite: offered to an offline HOST on STEAM. The rows rebuild when
    // the invite status or the visible friend list (who, and online/in-Kenshi)
    // changes - the invite layer refreshes the list every ~3 s while it is open.
    const bool offerInvite = g_inviteApi && g_inviteApi->ready && g_inviteApi->ready() &&
                             g_panel.hostFlag && g_panel.steamFlag && !st->running;
    std::string inviteSig;
    if (offerInvite) {
        inviteSig = g_inviteApi->status ? g_inviteApi->status() : "";
        if (g_inviteApi->pickerActive && g_inviteApi->pickerActive()) {
            inviteSig += "|p";
            int n = g_inviteApi->friendCount ? g_inviteApi->friendCount() : 0;
            for (int i = 0; i < n && i < INVITE_ROWS_MAX; ++i) {
                char b[40];
                _snprintf(b, sizeof(b) - 1, "|%llu:%d", g_inviteApi->friendId(i),
                          g_inviteApi->friendState(i));
                b[sizeof(b) - 1] = '\0';
                inviteSig += b;
            }
        }
    }
    if (inviteSig != g_lastInviteSig) g_panel.needsRebuild = true;

    // Host settings rows: shown while the panel is armed as HOST.
    const bool offerPerms = g_permToggleFn && g_panel.hostFlag;
    const int permSig = offerPerms ? ((st->permBuild ? 1 : 0) | (st->permContainers ? 2 : 0) |
                                      (st->permBuy ? 4 : 0) | 8) : 0;
    if (permSig != g_lastPermSig) g_panel.needsRebuild = true;

    // Create the window once (outside SEH - see the header note on C2712).
    // Layer MUST be "Info": spike 48 proved createFloatingLabel renders non-null
    // there. "Windows" is not a visible MyGUI layer here - the panel is minted
    // and armed but attaches to nothing, so F2 logs open/close yet nothing draws.
    if (!g_panel.panel) {
        std::string layer = "Info";
        // Right of centre: clear of RE_Kenshi's own menu on the left.
        g_panel.panel = g->createDatapanel(0.18f, 0.56f, 0.30f, 0.50f, false, layer, true); // (top, left, w, h)
        g_panel.built = false;
        if (!g_panel.panel) {
            coop::logErrLine("[coop-ui] createDatapanel FAILED");
        } else if (!uiPanelArmSeh(g, g_panel.panel)) {
            coop::logErrLine("[coop-ui] panel arm (update-list/show) FAILED");
        }
    }

    // ---- Simple view ------------------------------------------------------
    if (g_panel.panel && !g_panel.advanced) {
        const bool apiOk = g_inviteApi && g_inviteApi->ready && g_inviteApi->ready();
        std::string players = st->players ? std::string(st->players) : std::string();
        std::string invStat = (apiOk && g_inviteApi->status) ? g_inviteApi->status() : "";
        // Signature of everything shown: rebuild only when it changes.
        std::string sig = std::string(st->running ? "R" : "r") + (st->isHost ? "H" : "h") +
                          (st->inGame ? "G" : "g") + (st->peerPresent ? "P" : "p") + "|" +
                          detail + "|" + transfer + "|" + players + "|" + invStat;
        int games = (apiOk && g_inviteApi->gameCount) ? g_inviteApi->gameCount() : 0;
        for (int i = 0; i < games && i < GAME_ROWS_MAX; ++i) sig += std::string("|g") + g_inviteApi->gameName(i);
        const bool hostOnline = st->running && st->isHost;
        int friendsN = 0;
        if (hostOnline && apiOk && g_inviteApi->pickerActive && g_inviteApi->pickerActive()) {
            int n = g_inviteApi->friendCount ? g_inviteApi->friendCount() : 0;
            for (int i = 0; i < n && friendsN < INVITE_ROWS_MAX; ++i) {
                if (g_inviteApi->friendState(i) <= 0) break;
                char b[40];
                _snprintf(b, sizeof(b) - 1, "|f%llu:%d", g_inviteApi->friendId(i),
                          g_inviteApi->friendState(i));
                b[sizeof(b) - 1] = '\0';
                sig += b;
                ++friendsN;
            }
        }
        if (hostOnline) sig += (st->permBuild ? "B" : "b") + std::string(st->permContainers ? "C" : "c") +
                               (st->permBuy ? "Y" : "y") + (g_panel.hostSettings ? "S" : "s");
        sig += st->autoHost ? "A" : "a";
        if (g_panel.needsRebuild || !g_panel.built || sig != g_lastSimpleSig) {
            char titleBuf[64];
            _snprintf(titleBuf, sizeof(titleBuf) - 1, "Co-op    -    F%d to close",
                      panelVk - VK_F1 + 1);
            titleBuf[sizeof(titleBuf) - 1] = '\0';
            std::string title = titleBuf;
            std::string empty = "";
            // Every string the rows point at lives in these arrays for the build.
            std::string keys[SIMPLE_ROWS_MAX], vals[SIMPLE_ROWS_MAX];
            SimpleRow rows[SIMPLE_ROWS_MAX];
            int n = 0;
            DataPanelLine* statusLine = 0;
            DataPanelLine* infoLines[4] = { 0, 0, 0, 0 };
            int infoN = 0;
            g_hostBtn = 0; g_refreshBtn = 0; g_advBtn = 0; g_leaveBtn = 0;
            for (int i = 0; i < GAME_ROWS_MAX; ++i) g_gameBtns[i] = 0;
            for (int i = 0; i < INVITE_ROWS_MAX; ++i) { g_friendBtns[i] = 0; g_friendBtnIds[i] = 0; }
            for (int i = 0; i < 3; ++i) g_permBtns[i] = 0;
            g_inviteBtn = 0; g_hostSetBtn = 0; g_autoHostBtn = 0;
#define ROW(k, kk, vv, o) do { if (n < SIMPLE_ROWS_MAX) { keys[n] = (kk); vals[n] = (vv); \
            rows[n].kind = (k); rows[n].key = &keys[n]; rows[n].val = &vals[n]; \
            rows[n].out = (DataPanelLine**)(o); ++n; } } while (0)
            std::string statusVal = !transfer.empty() ? transfer : detail;
            const bool hasErr = !st->peerPresent && st->lastError && st->lastError[0];
            if (!st->running) {
                if (!invStat.empty()) statusVal = invStat;
                else if (hasErr) statusVal = std::string("Offline - ") + st->lastError;
                else statusVal = "Offline";
                if (st->inGame) ROW(1, "host", "Host for friends", &g_hostBtn);
                else ROW(0, "Host", "Load or start a game to host for friends", &infoLines[infoN++]);
                if (g_autoHostToggleFn)
                    ROW(1, "autohost", std::string("Always host my games: ") +
                                       (st->autoHost ? "ON" : "OFF") + "    (switch)",
                        &g_autoHostBtn);
                ROW(2, "", "", 0);
                ROW(0, "Join a friend", games > 0 ? "Pick a friend's game:"
                                                  : "No Steam friend is hosting right now",
                    &infoLines[infoN++]);
                for (int i = 0; i < games && i < GAME_ROWS_MAX; ++i) {
                    char k[8]; _snprintf(k, sizeof(k) - 1, "game%d", i); k[sizeof(k) - 1] = '\0';
                    ROW(1, k, std::string("Join  ") + g_inviteApi->gameName(i) + "'s game", &g_gameBtns[i]);
                }
                ROW(1, "refresh", "Refresh list", &g_refreshBtn);
                ROW(2, "", "", 0);
                ROW(0, "Status", "Status: " + statusVal, &statusLine);
                ROW(2, "", "", 0);
                ROW(1, "adv", "Advanced (IP / Steam ID)", &g_advBtn);
            } else {
                ROW(0, "Status", "Status: " + statusVal, &statusLine);
                if (!players.empty()) ROW(0, "Players", "Players: " + players, &infoLines[infoN++]);
                ROW(2, "", "", 0);
                if (st->isHost) {
                    ROW(1, "invite", "Invite a Steam friend", &g_inviteBtn);
                    if (!invStat.empty()) ROW(0, "Invite", "Invite: " + invStat, &infoLines[infoN++]);
                    if (apiOk && g_inviteApi->pickerActive && g_inviteApi->pickerActive()) {
                        int fr = 0, nf = g_inviteApi->friendCount ? g_inviteApi->friendCount() : 0;
                        for (int i = 0; i < nf && fr < INVITE_ROWS_MAX; ++i) {
                            int stt = g_inviteApi->friendState(i);
                            if (stt <= 0) break;
                            char k[8]; _snprintf(k, sizeof(k) - 1, "inv%d", fr); k[sizeof(k) - 1] = '\0';
                            const char* nm = g_inviteApi->friendName(i);
                            g_friendBtnIds[fr] = g_inviteApi->friendId(i);
                            ROW(1, k, std::string("Invite  ") + (nm ? nm : "?") +
                                          (stt == 2 ? "    (in Kenshi)" : "    (online)"),
                                &g_friendBtns[fr]);
                            ++fr;
                        }
                    }
                    ROW(2, "", "", 0);
                    ROW(1, "hostset", g_panel.hostSettings ? "Host settings    (hide)"
                                                            : "Host settings    (show)",
                        &g_hostSetBtn);
                    if (g_panel.hostSettings) {
                        const char* what[3] = { "Join may build", "Join may use storage", "Join may buy" };
                        const bool  on[3]   = { st->permBuild, st->permContainers, st->permBuy };
                        const char* pk[3]   = { "permbuild", "permstore", "permbuy" };
                        for (int i = 0; i < 3; ++i)
                            ROW(1, pk[i], std::string("    ") + what[i] + ": " + (on[i] ? "YES" : "NO") +
                                              "    (switch)", &g_permBtns[i]);
                    }
                    ROW(2, "", "", 0);
                    ROW(1, "leave", "Stop hosting", &g_leaveBtn);
                } else {
                    ROW(1, "leave", "Leave session", &g_leaveBtn);
                }
            }
#undef ROW
            panelBuildSimpleSeh(g_panel.panel, &title, rows, n, &empty);
            if (g_hostBtn)    g_hostBtn->callback    = MyGUI::newDelegate(&onHostBtn);
            if (g_refreshBtn) g_refreshBtn->callback = MyGUI::newDelegate(&onRefreshBtn);
            if (g_advBtn)     g_advBtn->callback     = MyGUI::newDelegate(&onAdvBtn);
            if (g_leaveBtn)   g_leaveBtn->callback   = MyGUI::newDelegate(&onLeaveBtn);
            if (g_inviteBtn)  g_inviteBtn->callback  = MyGUI::newDelegate(&onInviteBtn);
            if (g_hostSetBtn) g_hostSetBtn->callback = MyGUI::newDelegate(&onHostSetBtn);
            if (g_autoHostBtn) g_autoHostBtn->callback = MyGUI::newDelegate(&onAutoHostBtn);
            for (int i = 0; i < GAME_ROWS_MAX; ++i)
                if (g_gameBtns[i]) g_gameBtns[i]->callback = MyGUI::newDelegate(&onGameBtn);
            for (int i = 0; i < INVITE_ROWS_MAX; ++i)
                if (g_friendBtns[i]) g_friendBtns[i]->callback = MyGUI::newDelegate(&onFriendBtn);
            for (int i = 0; i < 3; ++i)
                if (g_permBtns[i]) g_permBtns[i]->callback = MyGUI::newDelegate(&onPermBtn);
            dbgColourSeh(statusLine, !transfer.empty() || hasErr);
            for (int i = 0; i < infoN; ++i) dbgColourSeh(infoLines[i], false);
            g_panel.built = true;
            g_panel.needsRebuild = false;
            g_panel.lastStatus = detail;
            g_panel.lastTransfer = transfer;
            g_lastSimpleSig = sig;
        }
    }

    // (Re)populate the rows when anything visible changed (advanced view).
    if (g_panel.panel && g_panel.advanced && (g_panel.needsRebuild || !g_panel.built)) {
        char titleBuf[64];
        _snprintf(titleBuf, sizeof(titleBuf) - 1, "Co-op Session    -    F%d to close",
                  panelVk - VK_F1 + 1);
        titleBuf[sizeof(titleBuf) - 1] = '\0';
        std::string title    = titleBuf;
        std::string roleKey  = "role";
        std::string roleCap  = std::string("Role: ") + (g_panel.hostFlag ? "HOST" : "JOIN") + "    (switch)";
        std::string transKey = "trans";
        std::string transCap = std::string("Transport: ") + (g_panel.steamFlag ? "STEAM" : "UDP") + "    (switch)";
        std::string connKey  = "conn";
        std::string connCap  = std::string("Connection: ") + (g_panel.connectedFlag ? "ONLINE" : "OFFLINE") + "    (switch)";

        // White debug line: describes the live connection state + type. Reflects
        // the ACTUAL running session when online; the armed toggles when offline.
        std::string transStr = (st->transportSel == 0) ? "Steam" : "UDP";
        std::string dbgKey   = "Connection status";
        std::string dbgVal;
        if (st->running) {
            if (st->peerPresent)
                dbgVal = (st->isHost ? std::string("Hosting") : std::string("Joining")) +
                         " over " + transStr + " - peer connected";
            else if (st->isHost)
                dbgVal = std::string("Hosting over ") + transStr + " - waiting for peer...";
            else
                dbgVal = std::string("Joining over ") + transStr + " - connecting to host...";
        } else {
            dbgVal = std::string("Offline - will ") + (g_panel.hostFlag ? "host" : "join") +
                     " over " + (g_panel.steamFlag ? "Steam" : "UDP") + " on Connect";
        }
        // A failing session says why (protocol mismatch, unreachable host, port
        // in use) - shown amber, like the transfer line.
        const bool hasErr = !st->peerPresent && st->lastError && st->lastError[0];
        if (hasErr) dbgVal += std::string("  -  ") + st->lastError;
        // A join streaming the host's world at the menu has no leader for the
        // screen overlay, so surface the live progress here instead (amber).
        if (!transfer.empty()) { dbgVal = transfer; dbgKey = "World transfer"; }

        // Friend's SteamID: prefer the value pasted in-panel this session; fall
        // back to the config (steamPeer, mainly for advanced/back-compat use).
        // Both id rows show only the last 4 digits - the panel is often on screen
        // while streaming. Nothing needs the full digits by eye: Copy puts the
        // real id on the clipboard and Paste takes it back off.
        std::string peerKey = "Friend's Steam ID";
        std::string peerVal;
        unsigned long long peerShown = g_pastedPeer ? g_pastedPeer
                                                     : (unsigned long long)st->peerSteamId;
        if (peerShown != 0) {
            peerVal = coop::maskSteamId64(peerShown);
        } else if (g_pasteFailed) {
            peerVal = "(clipboard was not a Steam ID - copy theirs and retry)";
        } else {
            peerVal = "(click Paste friend's Steam ID)";
        }
        std::string pasteKey = "pasteid";
        std::string pasteCap = "Paste friend's Steam ID";

        std::string selfKey  = "Your Steam ID";
        std::string selfVal  = st->selfSteamId
                                   ? coop::maskSteamId64((unsigned long long)st->selfSteamId)
                                   : std::string("(Steam not running)");
        std::string copyKey  = "copyid";
        std::string copyCap  = "Copy my Steam ID";
        std::string empty    = "";

        // Outbound invite rows: online friends only (the list is sorted
        // in-Kenshi > online > offline, so stop at the first offline one).
        std::string inviteKey  = "invite";
        std::string inviteCap;
        std::string invStatKey = "Invite";
        std::string invStatVal;
        std::string friendKeys[INVITE_ROWS_MAX];
        std::string friendCaps[INVITE_ROWS_MAX];
        int friendRows = 0;
        for (int i = 0; i < INVITE_ROWS_MAX; ++i) g_friendBtnIds[i] = 0;
        if (offerInvite) {
            const bool picker = g_inviteApi->pickerActive && g_inviteApi->pickerActive();
            inviteCap = picker ? "Refresh friend list"
                               : "Invite a Steam friend    (no Steam ID needed)";
            const char* ist = g_inviteApi->status ? g_inviteApi->status() : "";
            invStatVal = (ist && ist[0]) ? std::string(ist)
                       : std::string("Click above, then pick a friend - they accept in Steam");
            if (picker) {
                int n = g_inviteApi->friendCount ? g_inviteApi->friendCount() : 0;
                for (int i = 0; i < n && friendRows < INVITE_ROWS_MAX; ++i) {
                    int stt = g_inviteApi->friendState(i);
                    if (stt <= 0) break;
                    char k[8];
                    _snprintf(k, sizeof(k) - 1, "inv%d", friendRows);
                    k[sizeof(k) - 1] = '\0';
                    friendKeys[friendRows] = k;
                    const char* nm = g_inviteApi->friendName(i);
                    friendCaps[friendRows] = std::string("Invite  ") + (nm ? nm : "?") +
                                             (stt == 2 ? "    (in Kenshi)" : "    (online)");
                    g_friendBtnIds[friendRows] = g_inviteApi->friendId(i);
                    ++friendRows;
                }
                if (friendRows == 0 && !(ist && ist[0]))
                    invStatVal = "No Steam friends online right now";
            }
        }

        PanelStrings ps;
        ps.title = &title; ps.roleKey = &roleKey; ps.roleCap = &roleCap;
        ps.transKey = &transKey; ps.transCap = &transCap;
        ps.connKey = &connKey; ps.connCap = &connCap;
        ps.dbgKey = &dbgKey; ps.dbgVal = &dbgVal;
        ps.peerKey = &peerKey; ps.peerVal = &peerVal;
        ps.pasteKey = &pasteKey; ps.pasteCap = &pasteCap;
        ps.selfKey = &selfKey; ps.selfVal = &selfVal;
        ps.copyKey = &copyKey; ps.copyCap = &copyCap;
        ps.empty = &empty;
        ps.inviteKey  = offerInvite ? &inviteKey  : 0;
        ps.inviteCap  = offerInvite ? &inviteCap  : 0;
        ps.invStatKey = offerInvite ? &invStatKey : 0;
        ps.invStatVal = offerInvite ? &invStatVal : 0;
        ps.friendRows = friendRows;
        for (int i = 0; i < INVITE_ROWS_MAX; ++i) {
            ps.friendKey[i] = &friendKeys[i];
            ps.friendCap[i] = &friendCaps[i];
        }
        std::string permKeys[3] = { "permbuild", "permstore", "permbuy" };
        std::string permCaps[3];
        {
            const char* what[3] = { "Join may build", "Join may use storage", "Join may buy" };
            const bool  on[3]   = { st->permBuild, st->permContainers, st->permBuy };
            for (int i = 0; i < 3; ++i)
                permCaps[i] = std::string(what[i]) + ": " + (on[i] ? "YES" : "NO") + "    (switch)";
        }
        for (int i = 0; i < 3; ++i) {
            ps.permKey[i] = offerPerms ? &permKeys[i] : 0;
            ps.permCap[i] = offerPerms ? &permCaps[i] : 0;
        }
        std::string backKey = "simple", backCap = "Back to the simple view";
        ps.backKey = &backKey; ps.backCap = &backCap;
        panelBuildSeh(g_panel.panel, &ps);
        if (g_simpleBtn) g_simpleBtn->callback = MyGUI::newDelegate(&onAdvBtn);

        // Delegate assignment + white-colouring live OUTSIDE the SEH frame (pointer
        // targets are valid post-build; assignment can't fault) so no delegate
        // temporary lands in it.
        if (g_roleBtn)    g_roleBtn->callback    = MyGUI::newDelegate(&onRoleBtn);
        if (g_transBtn)   g_transBtn->callback   = MyGUI::newDelegate(&onTransBtn);
        if (g_connBtn)    g_connBtn->callback    = MyGUI::newDelegate(&onConnBtn);
        if (g_copyIdBtn)  g_copyIdBtn->callback  = MyGUI::newDelegate(&onCopyIdBtn);
        if (g_pasteIdBtn) g_pasteIdBtn->callback = MyGUI::newDelegate(&onPasteIdBtn);
        if (g_inviteBtn)  g_inviteBtn->callback  = MyGUI::newDelegate(&onInviteBtn);
        for (int i = 0; i < 3; ++i)
            if (g_permBtns[i]) g_permBtns[i]->callback = MyGUI::newDelegate(&onPermBtn);
        for (int i = 0; i < INVITE_ROWS_MAX; ++i)
            if (g_friendBtns[i]) g_friendBtns[i]->callback = MyGUI::newDelegate(&onFriendBtn);
        dbgColourSeh(g_debugLine, !transfer.empty() || hasErr); // amber: streaming / error
        dbgColourSeh(g_peerLine, false);
        dbgColourSeh(g_selfLine, false);
        dbgColourSeh(g_inviteLine, false);

        g_panel.built = true;
        g_panel.needsRebuild = false;
        g_panel.lastStatus = detail;
        g_panel.lastTransfer = transfer;
        g_lastInviteSig = inviteSig;
        g_lastPermSig = permSig;
    }

    // Connect / disconnect on the Online/Offline toggle edge (edge, not level, so
    // a connect that hasn't reported running yet is not re-fired every tick). The
    // pasted friend id (0 if none) is handed to the plugin, which lets a non-zero
    // value override the config steamPeer; UDP ip/port still come from the config.
    if (g_panel.connectedFlag != g_panel.lastChkVal) {
        g_panel.lastChkVal = g_panel.connectedFlag;
        if (g_panel.connectedFlag && !st->running) {
            char b[80];
            _snprintf(b, sizeof(b) - 1, "[coop-ui] CONNECT role=%s transport=%s",
                      g_panel.hostFlag ? "HOST" : "JOIN",
                      g_panel.steamFlag ? "steam" : "udp");
            b[sizeof(b) - 1] = '\0';
            coop::logLine(b);
            if (onConnect) onConnect(g_panel.hostFlag, g_panel.steamFlag, g_pastedPeer);
        } else if (!g_panel.connectedFlag && st->running) {
            coop::logLine("[coop-ui] DISCONNECT requested");
            if (onDisconnect) onDisconnect();
        }
    }
}

// Squad tab captions are cached by the main bar; after renaming a squad (player
// names, protocol 57) ask it to rebuild the tabs. Main thread; SEH-guarded.
void refreshSquadTabs() {
    ForgottenGUI* g = ::gui;
    if (!g) return;
    __try {
        if (g->mainbar) g->mainbar->refreshPortraitTabs();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- Persistent co-op status overlay ----------------------------------------
// A fixed banner in the top-left corner of the screen showing live session status
// colored by state (0 = offline/red, 1 = waiting/yellow, 2 = connected/green).
// Unlike the character-tracked ScreenLabel this replaces, it needs no player
// character and holds its place while the camera moves - which also makes it
// visible at the title screen, where a join has no leader while it streams the
// host's world. Removed when show=false.
//
// Two widgets, because neither factory alone does the job. Measured 2026-08-04:
// createFloatingLabel hands back a bare MyGUI::Window that IS visible and
// layer-attached at the coords we ask for, but its skin carries no text region at
// all - setCaption is silently dropped (getCaption().size() stays 0) and it has no
// children, so it draws nothing. It is still the only way to get a widget parented
// to a screen layer instead of to another window, so we keep it as an invisible
// container and put Kenshi's own label factory inside it (createLabelAbs ->
// MyGUI::TextBox with a text-bearing skin, the one the datapanel rows use).
// Caption + colour go to the child; the container is only geometry.

namespace {
// Banner box in pixels: 10 px in from the top-left corner. Applied with the
// absolute setCoord instead of createFloatingLabel's normalized coords, since a
// corner inset is a pixel quantity and the reconstructed header's top/left
// argument order is ambiguous (the normalized values are overwritten either way).
const int kOverlayX = 10;
const int kOverlayY = 10;
const int kOverlayW = 520;
const int kOverlayH = 26;

MyGUI::Window*  g_overlayBox   = 0; // container: geometry + layer attachment
MyGUI::TextBox* g_overlay      = 0; // the label that actually draws the text
int             g_overlayState = -1;
std::string     g_overlayText;

int overlayColorId(int state) { return state == 2 ? 0 : (state == 1 ? 2 : 1); }

// Put the freshly-minted container in its pixel box and mint the label inside it.
// createLabelAbs takes its text by const-ref and MyGUI::Align is a trivial int
// wrapper (no destructor), so this whole frame is SEH-safe - the same rule
// dbgColourSeh follows for MyGUI::Colour.
MyGUI::TextBox* overlayBuildSeh(MyGUI::Window* box, const std::string* text) {
    __try {
        box->setCoord(kOverlayX, kOverlayY, kOverlayW, kOverlayH);
        box->setVisible(true);
        MyGUI::TextBox* l = ::gui->createLabelAbs(box, 0, 0, kOverlayW, kOverlayH,
                                                  *text, MyGUI::Align::Left);
        if (l) {
            l->setTextAlign(MyGUI::Align::Left);
            l->setVisible(true);
        }
        return l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Caption + colour in place. MyGUI::UString owns a buffer (destructor => C2712),
// so the caller builds it outside this frame and passes a pointer. false = the
// widget faulted; the caller then treats the pointer as dead.
bool overlayUpdateSeh(MyGUI::TextBox* l, const MyGUI::UString* text,
                      const MyGUI::Colour* col) {
    __try {
        l->setCaption(*text);
        l->setTextColour(*col);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Destroying the container takes its label child with it (MyGUI owns the subtree).
void overlayDestroySeh(ForgottenGUI* g, MyGUI::Window* box) {
    __try { g->destroyWidget(box); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
} // namespace

void coopOverlayTick(const char* text, int state, bool show) {
    ForgottenGUI* g = ::gui;
    if (!g) return;

    if (!show) {
        if (g_overlayBox) {
            overlayDestroySeh(g, g_overlayBox);
            g_overlayBox = 0; g_overlay = 0;
            g_overlayState = -1; g_overlayText.clear();
        }
        return;
    }

    std::string t = text ? std::string(text) : std::string();
    if (!g_overlay) {
        // createFloatingLabel takes the layer BY VALUE (an unwindable temporary
        // => C2712), so the container mint stays outside SEH, exactly like
        // createDatapanel above; ::gui was verified non-null. Layer MUST be "Info"
        // for the same reason the panel uses it - nothing draws on "Windows".
        if (g_overlayBox) { overlayDestroySeh(g, g_overlayBox); g_overlayBox = 0; }
        std::string layer = "Info";
        std::string empty;
        g_overlayBox = g->createFloatingLabel(0.01f, 0.01f, 0.30f, 0.03f, empty,
                                              MyGUI::Align::Default, layer);
        if (!g_overlayBox) {
            coop::logErrLine("[coop-ui] banner container FAILED");
            return;
        }
        g_overlay = overlayBuildSeh(g_overlayBox, &t);
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] banner box=%p label=%p",
                  (void*)g_overlayBox, (void*)g_overlay);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        if (!g_overlay) {
            coop::logErrLine("[coop-ui] banner label FAILED");
            return;
        }
        g_overlayState = -1;   // no caller state is -1: forces the caption pass
        g_overlayText.clear();
    }

    if (t != g_overlayText || state != g_overlayState) {
        MyGUI::Colour col; markerColour(overlayColorId(state), &col);
        MyGUI::UString u(t.c_str());
        if (overlayUpdateSeh(g_overlay, &u, &col)) {
            g_overlayText = t; g_overlayState = state;
        } else {
            // The GUI destroyed the widgets under us - clearGUI() on a world load
            // empties the layer and notifies nobody, so a pointer held across
            // ticks dangles silently (same lesson as the ScreenLabel registry
            // note above). Forget them and re-mint on the next tick.
            g_overlayBox = 0; g_overlay = 0;
            g_overlayState = -1; g_overlayText.clear();
        }
    }
}

} // namespace engine
} // namespace coop
