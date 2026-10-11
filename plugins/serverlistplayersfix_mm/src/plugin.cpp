// serverlistplayersfix_mm — Plugify C++ port of Source2ZE's "ServerListPlayersFix"
// (symbols, DWARF and disassembly of the original: see BEHAVIOR.md).
//
// What the original does: every 5 seconds (Plat_FloatTime gate in its ISource2Server::GameFrame
// hook) it calls ISteamGameServer::BUpdateUserData for every client slot, so the Steam server
// browser's player list matches the server. It hooks ISource2Server::GameServerSteamAPIActivated
// to (re)initialise its CSteamGameServerAPIContext and ...Deactivated to drop it.
//
// Port: the same loop on the same 5 s cadence through OnPluginUpdate, the Steam game server
// interface resolved lazily through steamlink (dlsym on the process - exactly what the original's
// imports resolved to), controller/name/score read through pisexcompat/s2sdk:
//   node  = CGameEntitySystem::GetEntityInstance(CEntityIndex(slot + 1)) -> pisex::GetPlayerController
//   name  = CBasePlayerController::m_iszPlayerName                       -> pisex::GetPlayerName
//   score = IServerGameClients::GetPlayerScore(slot)                     -> CCSPlayerController::m_iScore
// The activate/deactivate hooks of the original are not reproduced: they only managed its
// CSteamGameServerAPIContext, and steam_api (re)initialises that context on first use here.
//
// KNOWN OVERLAP (BEHAVIOR.md §"s2sdk already does this"): s2sdk 2.18.2's own core
// (PlayerManager::OnGameFrame -> UpdatePlayers) runs exactly this BUpdateUserData loop every
// 5 seconds. Running this plugin as well costs one extra Steam call per player per 5 s
// (idempotent); if the operator prefers a single implementation, leave this plugin uninstalled.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>

#include <plg/plugin.hpp>
#include <plugin_export.h>

#include <pisex/pisex.hpp>
#include <s2sdk/s2sdk.hpp>

#include "steam_gameserver.hpp"

namespace {

// The original's update interval (the double at 0x1c7228 of the binary: 5.0 s).
constexpr std::chrono::seconds kUpdateInterval{5};

std::atomic<std::uint64_t> g_updates{0};
std::atomic<std::uint64_t> g_playersPushed{0};
std::atomic<bool> g_loggedFirstPush{false};

void Log(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

// The original's ServerListPlayersFix::UpdatePlayers().
void UpdatePlayers() {
	if (!pisex::IsGameReady()) {
		return;
	}

	const int maxClients = pisex::GetMaxPlayers();
	if (maxClients <= 0) {
		return;
	}

	for (int slot = 0; slot < maxClients; ++slot) {
		const std::uint64_t steamId = pisex::GetSteamID64(slot);
		if (steamId == 0) {
			continue;
		}
		const int controller = pisex::GetPlayerController(slot);
		if (controller == 0) {
			continue;
		}
		const char* name = pisex::GetPlayerName(slot);
		const int score = pisex::GetEntInt(controller, "CCSPlayerController", "m_iScore");
		if (steamlink::BUpdateUserData(steamId, name != nullptr ? name : "", static_cast<std::uint32_t>(score))) {
			g_playersPushed.fetch_add(1, std::memory_order_relaxed);
		}
	}
	g_updates.fetch_add(1, std::memory_order_relaxed);
}

class ServerListPlayersFixPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			pisex::Init("serverlistplayersfix_mm");
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
			pisex::Poll();

			if (!steamlink::IsAvailable()) {
				return {};
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
			pisex::Shutdown();
			Log("serverlistplayersfix_mm: stopped (pushes=" + std::to_string(g_updates.load()) +
					", players=" + std::to_string(g_playersPushed.load()) + ")",
				plg::Severity::Info);
		} catch (...) {
		}
		return {};
	}

private:
	std::chrono::milliseconds m_accumulated{0};
};

ServerListPlayersFixPlugin g_serverListPlayersFixPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_serverListPlayersFixPlugin)
