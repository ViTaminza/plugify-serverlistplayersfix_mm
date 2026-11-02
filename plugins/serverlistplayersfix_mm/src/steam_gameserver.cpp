#include "steam_gameserver.hpp"

#include <dlfcn.h>

namespace steamlink {
namespace {

// The flat entry points libsteam_api exports (steam_api_flat.h). The accessor is the SDK's own
// type-safe wrapper around SteamInternal_ContextInit - it owns the {callback, counter, pointer}
// triple that ContextInit calls, so this plugin never has to (and must not) build one itself.
using GameServerAccessorFn = void* (*)(); // S_API ISteamGameServer* SteamAPI_SteamGameServer_v015();
using UpdateUserDataFn = bool (*)(void*, std::uint64_t, const char*, std::uint32_t);

// Symbol names are resolved by RTLD_DEFAULT lookup, never an interface version guessed by the
// plugin: both are exact exports of the steam_api build the CS2 server runs (verified with nm on
// the deployed libsteam_api.so - the accessor under this name and the flat wrapper are present).
constexpr const char* kGameServerAccessorSymbol = "SteamAPI_SteamGameServer_v015";
constexpr const char* kUpdateUserDataSymbol = "SteamAPI_ISteamGameServer_BUpdateUserData";

GameServerAccessorFn g_gameServerAccessor = nullptr;
UpdateUserDataFn g_updateUserData = nullptr;

template <typename Fn>
Fn Resolve(const char* name) {
	return reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, name));
}

// Forget a half-resolved state: IsAvailable()/GameServer()/the wrapper must never act on a
// partially resolved API.
void Clear() {
	g_gameServerAccessor = nullptr;
	g_updateUserData = nullptr;
}

} // namespace

bool Init(std::string& error) {
	// Called on every server frame while the symbols are missing (the update loop retries), so a
	// successful resolution short-circuits.
	if (g_gameServerAccessor != nullptr && g_updateUserData != nullptr) {
		return true;
	}

	Clear();
	g_gameServerAccessor = Resolve<GameServerAccessorFn>(kGameServerAccessorSymbol);
	g_updateUserData = Resolve<UpdateUserDataFn>(kUpdateUserDataSymbol);

	if (g_gameServerAccessor == nullptr) {
		Clear();
		error = std::string(kGameServerAccessorSymbol) +
				" not found in the process (is steam_api loaded, and is the game server API part of it?)";
		return false;
	}
	if (g_updateUserData == nullptr) {
		Clear();
		error = std::string(kUpdateUserDataSymbol) + " not found in the process";
		return false;
	}
	return true;
}

bool IsAvailable() {
	return g_gameServerAccessor != nullptr && g_updateUserData != nullptr;
}

void Shutdown() {
	Clear();
}

void* GameServer() {
	if (g_gameServerAccessor == nullptr) {
		return nullptr;
	}
	try {
		// The accessor is the official lookup: it returns the interface the engine brought up (and a
		// valid, initialised context), or nullptr while the Steam game server API is not up yet.
		return g_gameServerAccessor();
	} catch (...) {
		return nullptr;
	}
}

bool BUpdateUserData(std::uint64_t steamId, const char* name, std::uint32_t score) {
	if (g_updateUserData == nullptr) {
		return false;
	}
	void* gameServer = GameServer();
	if (gameServer == nullptr) {
		// The engine has not brought its game server API up yet (or steam_api is not the game
		// server build): answer "not published" instead of calling a wrapper with a null instance.
		return false;
	}
	try {
		return g_updateUserData(gameServer, steamId, name != nullptr ? name : "", score);
	} catch (...) {
		return false;
	}
}

} // namespace steamlink
