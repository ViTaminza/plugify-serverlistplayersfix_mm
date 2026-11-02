// steam_gameserver — the ISteamGameServer access serverlistplayersfix_mm needs (BUpdateUserData)
// and s2sdk does not expose.
//
// How the interface is obtained: through the official flat accessor libsteam_api exports,
//
//     S_API ISteamGameServer *SteamAPI_SteamGameServer_v015();          // steam_api_flat.h
//
// which is the SDK's own type-safe wrapper around SteamInternal_ContextInit: it carries the
// {callback, counter, pointer} triple that SteamInternal_ContextInit calls to find or create the
// game server interface, so calling it can never get the initialisation wrong. The engine has
// already brought its game server API up in this process, so the accessor hands out the live
// interface (null while it is not up yet - every call then fails soft).
//
// The interface method is called through the flat C wrapper the same library exports,
//
//     S_API bool SteamAPI_ISteamGameServer_BUpdateUserData(
//             ISteamGameServer* self, uint64 steamIDUser, const char* name, uint32 score);
//
// resolved with dlsym(RTLD_DEFAULT, ...) because a Plugify plugin is a plain dlopen()ed shared
// object and does not link libsteam_api itself.
//
// DO NOT call SteamInternal_ContextInit directly. It takes a caller-owned, mutable
// `struct { void (*pFn)(void*); uintptr_t counter; void* ptr; }` (steam_api_internal.h:32-36) and
// **calls pFn**. A zero-initialised 0x18-byte buffer - what this file used to pass - is a null
// callback: the identical code in PlayersInfo crashed the live server on 27035 (PlayersInfo 1.1.0,
// SIGSEGV, RIP = 0 inside libsteam_api) on its first Steam query. This plugin is not installed on
// any server and never got to run that path; the code was the same. Only the library's own accessor
// owns that triple.
//
// Everything degrades to "unavailable" (false / nullptr) when a symbol is missing or the interface
// is not up yet, so a server without steam_api keeps working - it just does not publish players.

#pragma once

#include <cstdint>
#include <string>

namespace steamlink {

// Resolves the steam_api entry points. Safe to call repeatedly: a successful resolution
// short-circuits, a failed one is retried on the next call (the symbols may appear with the process
// loading steam_api later, and the interface itself appears when the engine brings its game server
// API up). Returns false (with a message in `error`) when the process does not expose them.
bool Init(std::string& error);

// Whether both the accessor and the update wrapper resolved. The interface itself may still be null
// (see GameServer()) - this only says the symbols are there.
[[nodiscard]] bool IsAvailable();

// Drops the resolved pointers (OnPluginEnd). Nothing is unloaded: the symbols belong to the engine's
// steam_api, this plugin only forgets them; a later Init() resolves them again.
void Shutdown();

// The live ISteamGameServer, or nullptr while the Steam game server API is not up yet. The library's
// accessor is asked on every call - it is its own cache and answers the *current* interface (the
// engine can deactivate and recreate it), so no pointer is kept here.
[[nodiscard]] void* GameServer();

// ISteamGameServer::BUpdateUserData - publishes one player to the Steam server browser.
bool BUpdateUserData(std::uint64_t steamId, const char* name, std::uint32_t score);

} // namespace steamlink
