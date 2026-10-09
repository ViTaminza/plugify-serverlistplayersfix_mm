# serverlistplayersfix_mm — what the binary does (analysis)

Analysed: `ref/src/binaries/serverlistplayersfix_mm/bin/linuxsteamrt64/serverlistplayersfix_mm.so`
(md5 `c1b09083…`, identical to the copy in the 27035 server backup; **with full DWARF debug info**,
so line numbers, function names and compilation units below are read straight out of the binary).
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
* Hooks (lines 76-80): `g_pSource2Server->GameFrame` (`KHook::Virtual<ISource2Server, void, bool,
  bool, bool>`), `GameServerSteamAPIActivated`, `GameServerSteamAPIDeactivated`.
* `Hook_GameFrame` (line 118): `if (Plat_FloatTime() > g_flNextUpdate) { UpdatePlayers();
  g_flNextUpdate = now + 5.0; }` — the 5.0 is the double at `.rodata 0x1c7228`.
* `Hook_GameServerSteamAPIActivated` (line 137): `CSteamGameServerAPIContext::Init()`;
  `...Deactivated` (line 144): nothing (just returns).
* `UpdatePlayers()` (lines 98-115), the actual fix:

```cpp
CGlobalVars* globals = engine->GetServerGlobals();          // engine vtbl +0x258
CGameEntitySystem* es = GameEntitySystem();                 // g_pGameResourceServiceServer + 0x50
if (!globals || !es) return;
for (int i = 0; i < globals->maxClients; i++) {
    CBasePlayerController* c = engine->GetClientController(i);          // engine vtbl +0x228
    if (!c) continue;
    CEntityInstance* e = es->GetEntityIdentity(CEntityIndex(i + 1))->GetEntityInstance();
    if (!e) continue;                                                   // entity identity check
    // the controller holds m_iszPlayerName; steamid comes from ISteamGameServer's API
    SteamGameServer()->BUpdateUserData(c->m_steamID /*first qword*/, e + nameOffset,
                                       gameclients->GetPlayerScore(i));  // vtbl +0xc0 / +0x148
}
```

The two vtable calls resolve to `IServerGameClients::GetPlayerScore(CPlayerSlot)` and
`ISteamGameServer::BUpdateUserData(CSteamID, const char* pchPlayerName, uint32 uScore)` (vtable
index 0x148/8 = 41 of `SteamGameServer015`); the name comes from
`CBasePlayerController::m_iszPlayerName` (`schema::GetOffset("CBasePlayerController",
"m_iszPlayerName")`), refreshed once per player and cached.

Purpose: the CS2 server browser's player list is filled from `BUpdateUserData`; the engine only
sends it on changes, so players stayed invisible. The plugin re-pushes every player every 5 s.

Absolute file offsets were not in the binary (it is the GitHub release build):
`GameEntitySystem()` is inlined (`g_pGameResourceServiceServer + 0x50`, the same offset
gamebanfix's gamedata carries). Nothing else: no convars, no commands, no config files, no phrases.

## s2sdk already does this

s2sdk 2.18.2 (the version this port targets, `ref/s2sdk-src/version.txt`) contains the *same* loop
in its own core:

* `src/core/player_manager.cpp:344-357` — `PlayerManager::UpdatePlayers()`:
  `for (i = 0; i < gpGlobals->maxClients; ++i) if (auto steamId = g_pEngineServer->GetClientSteamID(i))
  if (auto* controller = GetController(i)) g_pSteam->BUpdateUserData(*steamId,
  controller->GetPlayerName(), g_pSource2GameClients->GetPlayerScore(i));`
* `player_manager.cpp:224-233` — `PlayerManager::OnGameFrame()` runs it every `Plat_FloatTime() + 5.0`,
  and `src/core/plugin.cpp:242` calls `g_PlayerManager.OnGameFrame()` from the engine's GameFrame
  hook.

So on a Plugify server with s2sdk 2.18.2 the Steam player list is already maintained — the port
below is a faithful standalone implementation, but it is **redundant**. It is kept because the task
asked for it; installing it adds one extra (idempotent) Steam call per player per 5 s. Operators
who prefer a single implementation should leave it uninstalled.

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
  (`ref/pisex/SchemaEntity/CCSPlayerController.h:26` records that field; the engine method is not
  exposed by s2sdk). Live check: the score column in the Steam browser should match the scoreboard.

No config files, no commands, no cvars — the original has none.
