// serverlistplayersfix_mm — Plugify C++ port of Source2ZE's "ServerListPlayersFix"
// (symbols, DWARF and disassembly of the original: see BEHAVIOR.md).
//
// What the original does: every 5 seconds (Plat_FloatTime gate in its ISource2Server::GameFrame
// hook) it calls ISteamGameServer::BUpdateUserData for every client slot, so the Steam server
// browser's player list matches the server. It hooks ISource2Server::GameServerSteamAPIActivated
// to (re)initialise its CSteamGameServerAPIContext and ...Deactivated to drop it.
//
// Port: the same loop on the same 5 s cadence through OnPluginUpdate; the Steam game server
// interface is resolved lazily through steamlink (dlsym on the process - exactly what the
// original's imports resolved to), while the per-player data comes straight from s2sdk:
//   controller = CCSPlayerController::FromSlot(slot) -> PlayerSlotToEntHandle, plus the slot's
//                entity pointer as the second view, both validated;
//   steamid    = GetClientSteamID64 (only a live, non-bot client has one - PITFALLS B5);
//   name       = GetClientName ("<unknown>" counts as the original's empty string);
//   score      = CCSPlayerController::m_iScore through GetEntSchema.
// The activate/deactivate hooks of the original are not reproduced: they only managed its
// CSteamGameServerAPIContext, which the official SteamAPI_SteamGameServer_v015() accessor owns
// here (steam_gameserver.hpp explains why a context must never be built by the plugin itself).
//
// Pure s2sdk, no pisexcompat and no guard library (docs/PLUGIN_STANDARD.md §7): the safety rules
// of handoff/weaponskins/PITFALLS.md live in this file.
//   * B1/B2 - the map gate g_mapReady. s2sdk client, entity and schema calls dereference a null
//     client list, entity system or serializer map before the first map and between OnMapEnd and
//     the next OnMapStart. The OnMapStart/OnMapEnd listeners are registered in OnPluginStart;
//     readiness is latched by OnMapStart and by the first OnPluginUpdate of a plugin loaded into a
//     running map (unless an OnMapEnd was seen since OnPluginStart), and dropped by OnMapEnd. No
//     client call is made while the gate is closed, and OnMapEnd only clears state (PITFALLS B2).
//   * C2 - every client query goes through HasClient: slot 0..63, IsClientConnected &&
//     IsClientInGame && !IsClientSourceTV. HasController adds a non-null PlayerSlotToEntPointer;
//     ControllerHandle() then validates the handle with IsValidEntHandle and the slot's entity
//     pointer with IsValidEntPointer before any schema call.
//   * B5 - a bot has no Steam identity: it is skipped before GetClientSteamID64 is asked.
//   * A7 - OnPluginEnd unregisters the map listeners and closes the gate, so `plg reload` leaves
//     no callback pointing into the unloaded library.
//
// Fail soft (PITFALLS A1): OnPluginStart never returns Failed - that would stop every Plugify
// plugin - and a steam_api that is mapped only after the plugin starts is picked up by the retry
// in OnPluginUpdate.
//
// KNOWN OVERLAP (BEHAVIOR.md §"s2sdk already does this"): s2sdk's own core
// (PlayerManager::OnGameFrame -> UpdatePlayers) runs exactly this BUpdateUserData loop every
// 5 seconds. Running this plugin as well costs one extra Steam call per player per 5 s
// (idempotent); if the operator prefers a single implementation, leave this plugin uninstalled.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include "steam_gameserver.hpp"

namespace {

// The original's update interval (the double at 0x1c7228 of the binary: 5.0 s).
constexpr std::chrono::seconds kUpdateInterval{5};
// steam_api may be mapped into the process after this plugin starts: a failed symbol resolution
// is retried at this pace (the resolution is cheap and logs nothing while it keeps failing).
constexpr std::chrono::seconds kInitRetryInterval{1};
// CS2 has 64 player slots; the original iterated 0..63 without asking for a maximum.
constexpr int32_t kMaxSlots = 64;

std::atomic<std::uint64_t> g_updates{0};
std::atomic<std::uint64_t> g_playersPushed{0};
std::atomic<bool> g_loggedFirstPush{false};

void Log(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

// --- PITFALLS B1/B2: the map gate -------------------------------------------------------------

bool g_mapReady = false;           // client/entity/schema calls are safe right now
bool g_mapEndedSinceStart = false; // OnMapEnd was seen since OnPluginStart
bool g_firstUpdateDone = false;    // the mid-map latch has already been decided
bool g_mapListeners = false;       // OnMapStart/OnMapEnd are registered with s2sdk

// s2sdk listeners are plain function pointers called on the game thread; they must not throw and
// they must not touch a client: OnMapEnd only clears state (PITFALLS B2).

void OnMapStartListener() {
	// The new map and its entity system are in place: client/entity/schema calls are legal again.
	g_mapReady = true;
}

void OnMapEndListener() {
	// From here until the next OnMapStart every s2sdk client query can crash (PITFALLS B2).
	g_mapReady = false;
	g_mapEndedSinceStart = true;
}

void RegisterMapListeners() {
	if (g_mapListeners) {
		return;
	}
	// PITFALLS B1: registering the listeners touches no client, so it is safe in OnPluginStart.
	// Without them nothing could ever close the gate again, so a missing binding keeps it shut
	// instead of opening it blindly.
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		Log("serverlistplayersfix_mm: the s2sdk map listeners are unavailable - every client call stays refused",
			plg::Severity::Error);
		return;
	}
	s2sdk::OnMapStart_Register(&OnMapStartListener);
	s2sdk::OnMapEnd_Register(&OnMapEndListener);
	g_mapListeners = true;
}

void UnregisterMapListeners() {
	if (!g_mapListeners) {
		return;
	}
	g_mapListeners = false;
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		s2sdk::OnMapStart_Unregister(&OnMapStartListener);
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
	}
}

// --- PITFALLS C2: the client, controller and schema guards -------------------------------------

// A CEntityHandle is index | serial << 15; -1 is invalid and 0 is never a player's controller.
bool IsUsableHandle(int32_t handle) {
	return handle != 0 && handle != -1;
}

// The map gate and the slot range: from here on a client query is safe.
bool CanQueryClient(int32_t slot) {
	return g_mapReady && slot >= 0 && slot < kMaxSlots;
}

// A live, human-addressable slot: connected, in game and not the SourceTV proxy.
bool HasClient(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsClientConnected == nullptr ||
		__s2sdk_IsClientInGame == nullptr || __s2sdk_IsClientSourceTV == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot) && s2sdk::IsClientInGame(slot) &&
			   !s2sdk::IsClientSourceTV(slot);
	} catch (...) {
		return false;
	}
}

// Bots have no SteamID64, no net channel and no identity (PITFALLS B5). IsFakeClient itself is a
// client query, so it is only reachable through HasClient.
bool IsBot(int32_t slot) {
	if (!HasClient(slot) || __s2sdk_IsFakeClient == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsFakeClient(slot);
	} catch (...) {
		return false;
	}
}

// HasClient plus a controller the slot actually resolves to (the entity pointer is one of the two
// views ControllerHandle() cross-checks below).
bool HasController(int32_t slot) {
	if (!HasClient(slot) || __s2sdk_PlayerSlotToEntPointer == nullptr) {
		return false;
	}
	try {
		return s2sdk::PlayerSlotToEntPointer(slot) != nullptr;
	} catch (...) {
		return false;
	}
}

// A connected, in-game human (bots and the SourceTV proxy are excluded).
bool IsHumanClient(int32_t slot) {
	return HasController(slot) && !IsBot(slot);
}

// The controller's entity handle, or -1 when the slot has no usable controller: only -1 (and 0)
// are invalid sentinels, while other negative values are valid handles once the serial counter
// passes bit 31 (PITFALLS C1/C2).
int32_t ControllerHandle(int32_t slot) {
	if (!HasController(slot)) {
		return -1;
	}
	if (__s2sdk_PlayerSlotToEntHandle == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr ||
		__s2sdk_IsValidEntHandle == nullptr || __s2sdk_IsValidEntPointer == nullptr) {
		return -1;
	}
	try {
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (!IsUsableHandle(handle) || !s2sdk::IsValidEntHandle(handle)) {
			return -1;
		}
		// The handle and the slot's entity pointer are two views of the same controller and both
		// have to be valid: the schema exports take the handle, while PlayerSlotToEntPointer is
		// what proves the slot still resolves to that entity (between the two reads the engine can
		// already have reclaimed it, and every s2sdk schema call dereferences the handle).
		void* controller = s2sdk::PlayerSlotToEntPointer(slot);
		if (controller == nullptr || !s2sdk::IsValidEntPointer(controller)) {
			return -1;
		}
		return handle;
	} catch (...) {
		return -1;
	}
}

// The SteamID64 the original's cached controller value carried. A bot has no Steam identity, so
// only a validated human client may be asked (PITFALLS B5/C2).
std::uint64_t ClientSteamID64(int32_t slot) {
	if (!IsHumanClient(slot) || __s2sdk_GetClientSteamID64 == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

// CBasePlayerController::m_iszPlayerName. "<unknown>" is s2sdk's placeholder for a name that could
// not be resolved and counts as empty, exactly like the original's empty string.
std::string ClientName(int32_t slot) {
	if (!HasClient(slot) || __s2sdk_GetClientName == nullptr) {
		return {};
	}
	try {
		const plg::string name = s2sdk::GetClientName(slot);
		std::string out(name.data(), name.size());
		if (out == "<unknown>") {
			out.clear();
		}
		return out;
	} catch (...) {
		return {};
	}
}

// CCSPlayerController::m_iScore of a validated controller handle. The typed schema read needs a
// ready map (GetEntSchema dereferences the serializer/type info - PITFALLS C8); a build without
// the field or without the export simply contributes the original's default score of 0.
std::uint32_t ControllerScore(int32_t controllerHandle) {
	if (!g_mapReady || !IsUsableHandle(controllerHandle) || __s2sdk_GetEntSchema == nullptr ||
		__s2sdk_IsValidEntHandle == nullptr) {
		return 0;
	}
	try {
		if (!s2sdk::IsValidEntHandle(controllerHandle)) {
			return 0;
		}
		const std::int64_t score = s2sdk::GetEntSchema(
			controllerHandle, plg::string("CCSPlayerController"), plg::string("m_iScore"), 0);
		return static_cast<std::uint32_t>(static_cast<std::int32_t>(score));
	} catch (...) {
		return 0;
	}
}

// pisex::GetMaxPlayers() of the old port: the number of slots to walk, 0..64. GetMaxClients() is
// the live value; it answers -1 while gpGlobals is not initialised, so anything outside 1..64
// falls back to the original's fixed 0..63 range. 0 while the gate is closed - the caller must not
// query a single slot then.
int32_t MaxPlayers() {
	if (!g_mapReady) {
		return 0;
	}
	if (__s2sdk_GetMaxClients == nullptr) {
		return kMaxSlots;
	}
	try {
		const int32_t maxClients = s2sdk::GetMaxClients();
		if (maxClients <= 0) {
			return kMaxSlots;
		}
		return maxClients < kMaxSlots ? maxClients : kMaxSlots;
	} catch (...) {
		return kMaxSlots;
	}
}

// The original's ServerListPlayersFix::UpdatePlayers(): push every connected player again.
void UpdatePlayers() {
	if (!g_mapReady) {
		return;
	}

	const int32_t maxClients = MaxPlayers();
	if (maxClients <= 0) {
		return;
	}

	for (int32_t slot = 0; slot < maxClients; ++slot) {
		// PITFALLS C2: nothing below this line runs for a slot without a live, resolvable client.
		if (!HasController(slot)) {
			continue;
		}
		const std::uint64_t steamId = ClientSteamID64(slot); // 0 for bots and unauthenticated slots
		if (steamId == 0) {
			continue;
		}
		const int32_t controller = ControllerHandle(slot);
		if (!IsUsableHandle(controller)) {
			continue;
		}
		const std::string name = ClientName(slot);
		const std::uint32_t score = ControllerScore(controller);
		if (steamlink::BUpdateUserData(steamId, name.c_str(), score)) {
			g_playersPushed.fetch_add(1, std::memory_order_relaxed);
		}
	}
	g_updates.fetch_add(1, std::memory_order_relaxed);
}

class ServerListPlayersFixPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			m_accumulated = std::chrono::milliseconds::zero();
			m_retryAccumulated = std::chrono::milliseconds::zero();

			// A fresh start (including `plg reload`) begins with the gate shut: PITFALLS B1.
			g_mapReady = false;
			g_mapEndedSinceStart = false;
			g_firstUpdateDone = false;
			RegisterMapListeners();

			std::string error;
			if (!steamlink::Init(error)) {
				Log("serverlistplayersfix_mm: Steam game server API unavailable (" + error +
						") - the fix stays off",
					plg::Severity::Warning);
				return {}; // never Failed: that would stop every Plugify plugin
			}
			Log("serverlistplayersfix_mm: ready (pushing player data to Steam every 5 s)", plg::Severity::Info);
		} catch (const std::exception& ex) {
			Log(std::string("serverlistplayersfix_mm: OnPluginStart failed: ") + ex.what(), plg::Severity::Error);
		} catch (...) {
			Log("serverlistplayersfix_mm: OnPluginStart failed with an unknown exception", plg::Severity::Error);
		}
		return {};
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override {
		try {
			// A plugin loaded into a running map never saw OnMapStart: its first update is the
			// "a map is up" signal (PITFALLS B1). An OnMapEnd seen since OnPluginStart means the
			// server is between maps (or doing the boot-time changelevel of PITFALLS B3), so the
			// latch waits for the next OnMapStart instead.
			if (!g_firstUpdateDone) {
				g_firstUpdateDone = true;
				if (g_mapListeners && !g_mapEndedSinceStart) {
					g_mapReady = true;
				}
			}

			if (!steamlink::IsAvailable()) {
				// OnPluginStart may have run before steam_api was mapped (or a server build may
				// export the symbols later): retry the resolution, otherwise the fix stays off
				// for the whole boot. Init() only remembers a fully successful resolution.
				m_retryAccumulated += dt;
				if (m_retryAccumulated < kInitRetryInterval) {
					return {};
				}
				m_retryAccumulated = std::chrono::milliseconds::zero();
				std::string error;
				if (!steamlink::Init(error)) {
					return {};
				}
				Log("serverlistplayersfix_mm: Steam game server API resolved after startup, pushing player data",
					plg::Severity::Info);
			}
			if (steamlink::GameServer() == nullptr) {
				// The Steam game server API is not up yet (the original's m_bSteamAPIReady).
				return {};
			}
			if (!g_loggedFirstPush.exchange(true, std::memory_order_relaxed)) {
				Log("serverlistplayersfix_mm: Steam game server API is up, pushing player data",
					plg::Severity::Info);
			}

			m_accumulated += dt;
			if (m_accumulated < kUpdateInterval) {
				return {};
			}
			m_accumulated = std::chrono::milliseconds::zero();
			UpdatePlayers();
		} catch (const std::exception& ex) {
			Log(std::string("serverlistplayersfix_mm: update failed: ") + ex.what(), plg::Severity::Error);
		} catch (...) {
			Log("serverlistplayersfix_mm: update failed with an unknown exception", plg::Severity::Error);
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			// PITFALLS A7: nothing of this plugin may stay registered in the engine.
			UnregisterMapListeners();
			// A stale callback (the engine can still hold the pointer for a moment) can no
			// longer reach a client.
			g_mapReady = false;
			g_mapEndedSinceStart = true;
			g_firstUpdateDone = false;
			// The steam_api symbols belong to the engine's library; this only forgets the resolved
			// pointers, so a stale lookup cannot outlive this plugin (nothing is unloaded).
			steamlink::Shutdown();

			Log("serverlistplayersfix_mm: stopped (pushes=" + std::to_string(g_updates.load()) +
					", players=" + std::to_string(g_playersPushed.load()) + ")",
				plg::Severity::Info);
		} catch (...) {
		}
		return {};
	}

private:
	std::chrono::milliseconds m_accumulated{0};
	std::chrono::milliseconds m_retryAccumulated{0};
};

ServerListPlayersFixPlugin g_serverListPlayersFixPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_serverListPlayersFixPlugin)
