// SteamInvite - Steam overlay invite + lobby-based automatic SteamID exchange.
//
// The two-code exchange (SteamP2P's setPeer) is callback-free but forces players
// to read out and type 17-digit SteamIDs. This module layers Steam's native
// invite flow on top of the SAME P2P tunnel so nobody types an ID:
//
//   HOST : creates a 2-player friends-only Steam lobby, then opens the Steam
//          overlay's invite dialog. When the friend accepts and enters the lobby
//          the host learns their SteamID from lobby membership and connects.
//   JOIN : Steam delivers GameLobbyJoinRequested when the friend clicks the
//          invite (both players must already be in-game - Kenshi's launcher does
//          not forward +connect_lobby from a cold start). We join the lobby,
//          read the owner (= host SteamID) and connect as the client.
//
// Everything runs on the MAIN thread: tick() drives the Steam callback pump
// itself (SteamAPI_RunCallbacks) because Kenshi does not - its async CreateLobby
// result never dispatched until we added the call. This is safe even if the game
// also pumped: RunCallbacks just drains the message queue, so sequential
// main-thread calls can't double-deliver a single posted callback. tick() also
// polls lobby membership. The
// resolved peer is handed back through the ConnectFn (the plugin's coopUiConnect),
// which reuses the normal Steam-transport connect path. Manual ID entry stays as
// a fallback for setups where the overlay is unavailable.

#ifndef KENSHICOOP_STEAMINVITE_H
#define KENSHICOOP_STEAMINVITE_H

#include <string>

namespace coop {
namespace steaminvite {

typedef unsigned long long SteamId;

// Fired (on the main thread, from the Steam callback pump / tick) when an invite
// resolves a peer. Matches Plugin.cpp's coopUiConnect(isHost, useSteam, peerId).
typedef void (*ConnectFn)(bool isHost, bool useSteam, SteamId peerId);

// Resolve ISteamMatchmaking/ISteamFriends from the game's steam_api64.dll and
// register the invite/lobby/P2P Steam callbacks. Idempotent; safe to call every
// frame. Requires steamp2p::init() to have succeeded first (shared Steam client).
// Returns false (and logs why) when the interfaces are unavailable.
bool init(ConnectFn onConnect);
bool ready();

// Host action: open the in-panel friend picker. Creates the lobby (async) and
// refreshes the friend list. (Replaces the Steam overlay invite dialog, whose
// friend list is broken by a Steam-client web-view bug on many titles.)
void beginInvite();

// Send a direct Steam lobby invite to a specific friend (from the picker). The
// friend gets a Steam notification; clicking it fires GameLobbyJoinRequested on
// their side and auto-joins. If the lobby is still being created, the invite is
// queued and sent on completion.
void inviteFriend(SteamId id);

// "Host for friends" (protocol 57): the caller is ALREADY hosting (open Steam
// host); this opens a friends-only lobby so Steam lists the game to friends and
// invites can be sent. Players connect to the host themselves on entering.
void hostLobby();

// "Join a friend": friends playing Kenshi inside a lobby (read from Steam's
// friend game info - no invite needed). The list refreshes every ~3 s while
// browsing is on; joinGame(i) enters that lobby and connects as a join.
void        setBrowsing(bool on);
void        refreshGameList();
int         gameCount();
const char* gameName(int i);
SteamId     gameHostId(int i);
void        joinGame(int i);

// In-panel picker accessors (main thread). The list is cached in our own buffers
// (Steam's GetFriendPersonaName pointer is transient) and sorted in-Kenshi first.
// friendState: 0 = offline, 1 = online, 2 = currently playing Kenshi.
bool        pickerActive();
int         friendCount();
SteamId     friendId(int i);
const char* friendName(int i);
int         friendState(int i);

// Main-thread housekeeping: pumps Steam callbacks, refreshes the picker list, and
// polls host-side lobby membership for the arriving friend. Call every frame.
void tick();

// One-line human status for the panel/overlay (never null; "" when idle).
const char* status();

// Our own Steam persona name ("" when Steam's friends interface is unavailable).
std::string selfName();

// Leave any lobby and clear invite state (called on disconnect / teardown).
void reset();

} // namespace steaminvite
} // namespace coop

#endif // KENSHICOOP_STEAMINVITE_H
