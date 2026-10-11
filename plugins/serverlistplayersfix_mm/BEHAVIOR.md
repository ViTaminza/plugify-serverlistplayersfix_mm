# serverlistplayersfix_mm — what the binary does (analysis)

Analysed: `serverlistplayersfix_mm.so` (md5 `c1b09083…`, **with full DWARF debug info**, so line
numbers, function names and compilation units below are read straight out of the binary).
It is Source2ZE's public plugin `ServerListPlayersFix` (author *Poggu*, GPLv3,
<https://poggu.me>, built 2026-09-08 from the GitHub Actions path
`/__w/ServerListPlayersFix/ServerListPlayersFix/ServerListPlayersFix/`).

Files it was built from (`readelf --debug-dump=decodedline`):
`serverlistplayersfix.cpp/.h`, `cs2_sdk/schema.cpp`, `cs2_sdk/entity/cbaseplayercontroller.h`,
`utils/plat_unix.cpp` plus a vendored `hl2sdk-cs2`.

## Behaviour

* Interfaces fetched at `Load()` (`serverlistplayersfix.cpp:64-74`):
  `VEngineCvar007`, `Source2Server001` (`g_pSource2Server`), `Source2EngineToServer001`,
  `Source2GameClients001`, `SchemaSystem_001` (scanning is done with the vendored hl2sdk schema
  module), `NetworkServerService_001`, `GameResourceServiceServerV001`; KHook exported; the plugin
  registers itself as an `ISmmPlugin` + `IMetamodListener`.
* Hooks (lines 76-80): `g_pSource2Server->GameFrame`, `GameServerSteamAPIActivated` and
  `GameServerSteamAPIDeactivated` (KHook virtuals on `ISource2Server`).
* `Hook_GameFrame` (line 118): when `Plat_FloatTime()` passes the next-update stamp it runs
  `UpdatePlayers()` and pushes the stamp 5.0 s ahead (the 5.0 is the double at `.rodata 0x1c7228`).
* `Hook_GameServerSteamAPIActivated` (line 137): initialises the `CSteamGameServerAPIContext`;
  `...Deactivated` (line 144): nothing (just returns).
* `UpdatePlayers()` (lines 98-115), the actual fix. The steps, with the offsets read from the
  original's disassembly:

  * fetch the server globals (engine vtable +0x258) and the game entity system
    (`g_pGameResourceServiceServer` + 0x50); bail out when either is missing;
  * for every client slot: get the controller (engine vtable +0x228) and skip a null one; resolve
    the entity instance through `CEntityIndex(slot + 1)` and skip it when the identity check
    fails;
  * call `ISteamGameServer::BUpdateUserData` (vtable +0xc0 / +0x148) with the controller's cached
    steamid (its first qword), the controller's `m_iszPlayerName` (offset `nameOffset` in the
    entity) and `IServerGameClients::GetPlayerScore(slot)`.

The two vtable calls resolve to `IServerGameClients::GetPlayerScore(CPlayerSlot)` and
`ISteamGameServer::BUpdateUserData(CSteamID, const char* pchPlayerName, uint32 uScore)` (vtable
index 0x148/8 = 41 of `SteamGameServer015`); the name comes from
`CBasePlayerController::m_iszPlayerName` (resolved through the schema), refreshed once per player
and cached.

Purpose: the CS2 server browser's player list is filled from `BUpdateUserData`; the engine only
sends it on changes, so players stayed invisible. The plugin re-pushes every player every 5 s.

Absolute file offsets were not in the binary (it is the GitHub release build):
`GameEntitySystem()` is inlined (`g_pGameResourceServiceServer + 0x50`, the same offset
gamebanfix's gamedata carries). Nothing else: no convars, no commands, no config files, no phrases.

## s2sdk already does this

s2sdk 2.18.2 (the version this port targets) contains the *same* loop in its own core: its player
manager's `UpdatePlayers()` walks every client slot and, for each one that has a steam id and a
controller, calls `ISteamGameServer::BUpdateUserData` with the controller's player name and the
game clients' player score. `PlayerManager::OnGameFrame()` runs that pass every
`Plat_FloatTime() + 5.0`, and s2sdk's own plugin core calls it from the engine's GameFrame hook.

So on a Plugify server with s2sdk 2.18.2 the Steam player list is already maintained — the port
below is a faithful standalone implementation, but it is **redundant**. Installing it adds one
extra (idempotent) Steam call per player per 5 s; operators who prefer a single implementation
should leave it uninstalled.

## Port

`plugins/serverlistplayersfix_mm` (manifest name = the original `.vdf` alias
`serverlistplayersfix_mm`):

* `src/plugin.cpp` — the same 5 s loop: `pisex::GetSteamID64(slot)` (skip 0),
  `pisex::GetPlayerController(slot)` (skip none), `pisex::GetPlayerName(slot)`,
  `CCSPlayerController::m_iScore` as the score, then `steamlink::BUpdateUserData`.
* `src/steam_gameserver.{hpp,cpp}` — the `ISteamGameServer` access s2sdk does not expose: resolves
  `SteamInternal_ContextInit` + `SteamAPI_ISteamGameServer_BUpdateUserData` from the process with
  `dlsym(RTLD_DEFAULT, …)` (the original imported the same symbols; the CS2 server loads stem_api
  globally) and fills a 0x18-byte context exactly like the original's
  `s_CallbackCounterAndContext`. Degrades to "unavailable" without them.
* Differences from the original: no GameFrame/Steam-API vtable hooks (Plugify plugins have no
  `ISource2Server` handle; steam_api re-initialises the context on first use), and
  `IServerGameClients::GetPlayerScore` is approximated by the controller's `m_iScore`
  (pisexcompat records that field; the engine method is not exposed by s2sdk). To check in game:
  the score column in the Steam browser should match the scoreboard.

No config files, no commands, no cvars — the original has none.
