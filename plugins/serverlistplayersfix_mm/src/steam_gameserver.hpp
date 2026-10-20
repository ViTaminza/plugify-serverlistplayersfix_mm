// steam_gameserver — the ISteamGameServer access the two ports need and s2sdk does not expose.
//
// The original Metamod plugins do not link libsteam_api either: they carry a copy of the hl2sdk
// CSteamGameServerAPIContext and resolve SteamInternal_ContextInit /
// SteamAPI_ISteamGameServer_* against the CS2 server process (which loads steam_api globally -
// this is how every Steam-aware Metamod plugin works). Plugify plugins are plain dlopen'ed
// shared objects, so the same functions are resolved with dlsym(RTLD_DEFAULT, ...) at startup and
// the flat C API wrappers (SteamAPI_ISteamGameServer_*) are used instead of the C++ vtable.
//
// Everything degrades to "unavailable" (false/0) when the symbols are missing, so a server
// without steam_api loaded keeps working - it just does not push Steam data / report prime status.
//
// Verified interface layout (steam_api 1.5x): `SteamInternal_ContextInit(&ctx)` fills a context
// whose first pointer is the ISteamGameServer*; the original plugin's `s_CallbackCounterAndContext`
// is exactly this 0x18-byte structure (PlayersInfo.so, symbol
// `SteamGameServer()::s_CallbackCounterAndContext`, size 0x18).

#pragma once

#include <cstdint>
#include <string>

namespace steamlink {

// Resolves the steam_api entry points. Safe to call repeatedly: a successful resolution is
// remembered, a failed one is retried on the next call (the symbols may appear with the process
// loading steam_api later). Returns false (with a message in `error`) when the process does not
// expose them.
bool Init(std::string& error);

// Whether the entry points were resolved.
[[nodiscard]] bool IsAvailable();

// The interface pointer, or nullptr while the Steam game server API is not up yet.
[[nodiscard]] void* GameServer();

// ISteamGameServer::BUpdateUserData - publishes one player to the Steam server browser.
bool BUpdateUserData(std::uint64_t steamId, const char* name, std::uint32_t score);

// ISteamGameServer::UserHasLicenseForApp - returns the EUserHasLicenseForAppResult value
// (k_EUserHasLicenseResultHasLicense = 0, ...DoesNotHaveLicense = 1, ...NoAuth = 2) or -1 when
// the call is unavailable.
int UserHasLicenseForApp(std::uint64_t steamId, std::uint32_t appId);

} // namespace steamlink
