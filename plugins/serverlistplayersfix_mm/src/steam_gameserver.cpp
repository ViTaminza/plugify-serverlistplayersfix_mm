#include "steam_gameserver.hpp"

#include <dlfcn.h>

namespace steamlink {
namespace {

using ContextInitFn = void* (*)(void*);
using UpdateUserDataFn = bool (*)(void*, std::uint64_t, const char*, std::uint32_t);
using UserHasLicenseFn = int (*)(void*, std::uint64_t, std::uint32_t);

// The CSteamGameServerAPIContext the engine's steam_api fills in, 0x18 bytes and zero-initialised
// (the original plugin's `s_CallbackCounterAndContext` has exactly this shape and size).
alignas(void*) unsigned char g_context[0x18] = {};

ContextInitFn g_contextInit = nullptr;
UpdateUserDataFn g_updateUserData = nullptr;
UserHasLicenseFn g_userHasLicense = nullptr;
bool g_initialised = false;

void* GameServerUncached() {
	if (g_contextInit == nullptr) {
		return nullptr;
	}
	void* context = g_contextInit(g_context);
	if (context == nullptr) {
		return nullptr;
	}
	return *reinterpret_cast<void**>(context);
}

} // namespace

bool Init(std::string& error) {
	if (g_initialised) {
		return true;
	}
	g_initialised = true;

	// RTLD_DEFAULT: the CS2 server process itself (steam_api is loaded globally by the engine /
	// the launcher, which is why the Metamod originals could import these symbols directly).
	g_contextInit = reinterpret_cast<ContextInitFn>(dlsym(RTLD_DEFAULT, "SteamInternal_ContextInit"));
	g_updateUserData =
		reinterpret_cast<UpdateUserDataFn>(dlsym(RTLD_DEFAULT, "SteamAPI_ISteamGameServer_BUpdateUserData"));
	g_userHasLicense =
		reinterpret_cast<UserHasLicenseFn>(dlsym(RTLD_DEFAULT, "SteamAPI_ISteamGameServer_UserHasLicenseForApp"));

	if (g_contextInit == nullptr) {
		error = "SteamInternal_ContextInit not found in the process (steam_api not loaded?)";
		return false;
	}
	if (g_updateUserData == nullptr && g_userHasLicense == nullptr) {
		error = "SteamAPI_ISteamGameServer_* wrappers not found in the process";
		return false;
	}
	return true;
}

bool IsAvailable() {
	return g_contextInit != nullptr && (g_updateUserData != nullptr || g_userHasLicense != nullptr);
}

void* GameServer() {
	return GameServerUncached();
}

bool BUpdateUserData(std::uint64_t steamId, const char* name, std::uint32_t score) {
	if (g_updateUserData == nullptr) {
		return false;
	}
	void* gameServer = GameServerUncached();
	if (gameServer == nullptr) {
		return false;
	}
	return g_updateUserData(gameServer, steamId, name != nullptr ? name : "", score);
}

int UserHasLicenseForApp(std::uint64_t steamId, std::uint32_t appId) {
	if (g_userHasLicense == nullptr) {
		return -1;
	}
	void* gameServer = GameServerUncached();
	if (gameServer == nullptr) {
		return -1;
	}
	return g_userHasLicense(gameServer, steamId, appId);
}

} // namespace steamlink
