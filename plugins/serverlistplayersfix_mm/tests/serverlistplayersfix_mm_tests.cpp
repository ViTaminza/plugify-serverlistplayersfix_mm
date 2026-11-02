// serverlistplayersfix_mm tests — no engine and no Plugify host: the plugin TU is compiled in (its
// loop and guards live in an anonymous namespace), every __s2sdk_* binding slot is a recording fake
// and so are the steamlink entry points the plugin resolves the Steam API through.
//
// Covered:
//   * PITFALLS B1/B2 — the map gate: not one s2sdk client call before the first map, between
//     OnMapEnd and the next OnMapStart, or when an OnMapEnd arrived before the first update (the
//     boot-time changelevel of B3); the mid-map latch of a plugin loaded into a running map;
//   * PITFALLS C2 — slot range, connected/in-game/SourceTV checks before IsFakeClient /
//     GetClientSteamID64 / GetClientName, the PlayerSlotToEntPointer + IsValidEntHandle +
//     IsValidEntPointer validation before GetEntSchema, "<unknown>" -> "";
//   * PITFALLS B5 — a bot is never asked for its SteamID64 and is never pushed;
//   * the 5 s cadence, the GetMaxClients clamp (24 / -1 / 1000 / missing binding), the steam_api
//     retry, the score read (CCSPlayerController::m_iScore through GetEntSchema) and a clean
//     unload (PITFALLS A7), including the reload path.
//
// Run by ./test.sh (build/tests/serverlistplayersfix_mm_tests).

#include "../src/plugin.cpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

// --- the fake steamlink entry points ----------------------------------------------------------
// The plugin resolves the Steam game server API through these; the test records the pushes.

namespace steamlink {

struct Push {
	std::uint64_t steamid = 0;
	std::string name;
	std::uint32_t score = 0;
};

bool g_initSucceeds = true;   // Init() answers false while "steam_api is not loaded"
bool g_available = false;     // what IsAvailable() answers
bool g_gameServerUp = false;  // whether GameServer() returns an interface
bool g_pushSucceeds = true;   // the return value of BUpdateUserData
int g_initCalls = 0;
int g_shutdownCalls = 0;
std::vector<Push> g_pushes;
unsigned char g_gameServerToken = 0;

bool Init(std::string& error) {
	++g_initCalls;
	if (!g_initSucceeds) {
		error = "SteamAPI_SteamGameServer_v015 not found in the process (steam_api not loaded?)";
		return false;
	}
	g_available = true;
	return true;
}

bool IsAvailable() {
	return g_available;
}

void* GameServer() {
	return g_gameServerUp ? &g_gameServerToken : nullptr;
}

bool BUpdateUserData(std::uint64_t steamId, const char* name, std::uint32_t score) {
	g_pushes.push_back(Push{steamId, name != nullptr ? name : "<null>", score});
	return g_pushSucceeds;
}

// The real module forgets its dlsym'd pointers here; the engine's own interface is untouched, so
// the fake only drops the "symbols are resolved" flag (g_gameServerUp is engine state).
void Shutdown() {
	++g_shutdownCalls;
	g_available = false;
}

} // namespace steamlink

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

// --- the fake map lifecycle --------------------------------------------------------------------

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_onMapStart = callback;
	++g_mapStartRegisters;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
	++g_mapStartUnregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_onMapEnd = callback;
	++g_mapEndRegisters;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
	++g_mapEndUnregisters;
}

void MapStart() {
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void MapEnd() {
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

// --- the fake client and entity layer ----------------------------------------------------------

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool fake = false;
	std::string name;
	std::uint64_t steamid64 = 0;
};

struct FakeController {
	int32_t handle = 0;            // what PlayerSlotToEntHandle answers
	bool pointerPresent = false;   // whether PlayerSlotToEntPointer answers a pointer
	bool handleValid = true;       // what IsValidEntHandle answers for that handle
	bool pointerValid = true;      // what IsValidEntPointer answers for that pointer
	std::int64_t score = 0;        // what GetEntSchema answers for m_iScore
};

std::vector<FakeClient> g_clients(64);
std::vector<FakeController> g_controllers(64);

// One sentinel byte per slot: PlayerSlotToEntPointer answers a distinct, findable address.
alignas(16) unsigned char g_controllerSentinels[64][16] = {};

void* ControllerPointer(int32_t slot) {
	return &g_controllerSentinels[slot][0];
}

int32_t PointerSlot(void* pointer) {
	const auto value = reinterpret_cast<std::uintptr_t>(pointer);
	const auto base = reinterpret_cast<std::uintptr_t>(&g_controllerSentinels[0][0]);
	const auto end = base + sizeof(g_controllerSentinels);
	if (value < base || value >= end) {
		return -1;
	}
	return static_cast<int32_t>((value - base) / 16);
}

int32_t SlotForHandle(int32_t handle) {
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (g_controllers[slot].handle == handle) {
			return slot;
		}
	}
	return -1;
}

std::vector<int32_t> g_nameSlots;
std::vector<int32_t> g_steamIdSlots;
std::vector<int32_t> g_handleSlots;
std::vector<int32_t> g_schemaSlots;

int g_clientCalls = 0; // every fake below that reads the client or entity layer counts here
int g_connectedCalls = 0;
int g_inGameCalls = 0;
int g_sourceTvCalls = 0;
int g_fakeCalls = 0;
int g_nameCalls = 0;
int g_steamIdCalls = 0;
int g_handleCalls = 0;
int g_handleValidCalls = 0;
int g_pointerValidCalls = 0;
int g_schemaCalls = 0;
int g_maxClientsCalls = 0;
int32_t g_maxClients = 64;

std::string g_lastSchemaClass;
std::string g_lastSchemaField;
int32_t g_lastSchemaElement = -1;
int32_t g_lastSchemaHandle = 0;

const FakeClient& Client(int32_t slot) {
	static const FakeClient empty;
	return slot >= 0 && slot < static_cast<int32_t>(g_clients.size())
			   ? g_clients[static_cast<std::size_t>(slot)]
			   : empty;
}

const FakeController& Controller(int32_t slot) {
	static const FakeController empty;
	return slot >= 0 && slot < static_cast<int32_t>(g_controllers.size())
			   ? g_controllers[static_cast<std::size_t>(slot)]
			   : empty;
}

bool HasSlot(const std::vector<int32_t>& slots, int32_t slot) {
	return std::find(slots.begin(), slots.end(), slot) != slots.end();
}

bool FakeIsClientConnected(int32_t slot) {
	++g_clientCalls;
	++g_connectedCalls;
	return Client(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_clientCalls;
	++g_inGameCalls;
	return Client(slot).inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientCalls;
	++g_sourceTvCalls;
	return Client(slot).sourceTV;
}

bool FakeIsFakeClient(int32_t slot) {
	++g_clientCalls;
	++g_fakeCalls;
	return Client(slot).fake;
}

plg::string FakeGetClientName(int32_t slot) {
	++g_clientCalls;
	++g_nameCalls;
	g_nameSlots.push_back(slot);
	// s2sdk answers "<unknown>" when it cannot resolve the client name.
	if (Client(slot).name.empty()) {
		return plg::string("<unknown>");
	}
	return plg::string(Client(slot).name);
}

std::uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_clientCalls;
	++g_steamIdCalls;
	g_steamIdSlots.push_back(slot);
	return Client(slot).steamid64;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_clientCalls;
	if (!Controller(slot).pointerPresent) {
		return nullptr;
	}
	return ControllerPointer(slot);
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_handleCalls;
	g_handleSlots.push_back(slot);
	return Controller(slot).handle;
}

bool FakeIsValidEntHandle(int32_t handle) {
	++g_handleValidCalls;
	const int32_t slot = SlotForHandle(handle);
	return slot >= 0 && Controller(slot).handleValid;
}

bool FakeIsValidEntPointer(void* pointer) {
	++g_pointerValidCalls;
	const int32_t slot = PointerSlot(pointer);
	return slot >= 0 && Controller(slot).pointerValid;
}

std::int64_t FakeGetEntSchema(int32_t handle, const plg::string& className,
							  const plg::string& memberName, int32_t element) {
	++g_schemaCalls;
	g_schemaSlots.push_back(SlotForHandle(handle));
	g_lastSchemaHandle = handle;
	g_lastSchemaClass = ToStd(className);
	g_lastSchemaField = ToStd(memberName);
	g_lastSchemaElement = element;
	const int32_t slot = SlotForHandle(handle);
	return slot >= 0 ? Controller(slot).score : 0;
}

int32_t FakeGetMaxClients() {
	++g_maxClientsCalls;
	return g_maxClients;
}

void InstallFakes() {
	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;

	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_GetEntSchema = &FakeGetEntSchema;
	__s2sdk_GetMaxClients = &FakeGetMaxClients;
}

void ResetClientCounters() {
	g_clientCalls = 0;
	g_connectedCalls = 0;
	g_inGameCalls = 0;
	g_sourceTvCalls = 0;
	g_fakeCalls = 0;
	g_nameCalls = 0;
	g_steamIdCalls = 0;
	g_handleCalls = 0;
	g_handleValidCalls = 0;
	g_pointerValidCalls = 0;
	g_schemaCalls = 0;
	g_maxClientsCalls = 0;
	g_nameSlots.clear();
	g_steamIdSlots.clear();
	g_handleSlots.clear();
	g_schemaSlots.clear();
	g_lastSchemaClass.clear();
	g_lastSchemaField.clear();
	g_lastSchemaElement = -1;
	g_lastSchemaHandle = 0;
}

// A live, human, resolvable slot: connected, in game, a controller with a handle and a pointer.
void SetHuman(int32_t slot, const std::string& name, std::uint64_t steamid64, std::uint32_t score) {
	g_clients[static_cast<std::size_t>(slot)] = FakeClient{true, true, false, false, name, steamid64};
	FakeController controller;
	controller.handle = 1000 + slot;
	controller.pointerPresent = true;
	controller.score = static_cast<std::int64_t>(score);
	g_controllers[static_cast<std::size_t>(slot)] = controller;
}

void ResetPushes() {
	steamlink::g_pushes.clear();
}

const steamlink::Push* PushAt(std::size_t index) {
	return index < steamlink::g_pushes.size() ? &steamlink::g_pushes[index] : nullptr;
}

void PrepareTest() {
	// Drop whatever a previous test left registered; OnPluginEnd is idempotent.
	g_serverListPlayersFixPlugin.OnPluginEnd();

	g_clients.assign(64, FakeClient{});
	g_controllers.assign(64, FakeController{});
	g_maxClients = 64;

	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;

	steamlink::g_initSucceeds = true;
	steamlink::g_available = false;
	steamlink::g_gameServerUp = true;
	steamlink::g_pushSucceeds = true;
	steamlink::g_initCalls = 0;
	steamlink::g_shutdownCalls = 0;
	steamlink::g_pushes.clear();

	ResetClientCounters();
	// A test may have removed a binding on purpose: put the fakes back.
	InstallFakes();
}

// --- A. the map gate (PITFALLS B1/B2/B3) ------------------------------------------------------

void TestMapGate() {
	PrepareTest();
	g_serverListPlayersFixPlugin.OnPluginStart();
	Check(g_mapListeners, "OnPluginStart registers the map listeners");
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1,
		  "OnPluginStart registers OnMapStart and OnMapEnd once");
	Check(!g_mapReady, "the gate starts shut");
	Check(g_clientCalls == 0, "OnPluginStart makes no client call");
	Check(steamlink::g_initCalls == 1 && steamlink::g_available,
		  "OnPluginStart resolves the Steam game server API");

	SetHuman(0, "Alice", 76561198000000001ULL, 12);
	ResetPushes();

	// The boot-time changelevel (PITFALLS B3): an OnMapEnd arrives before the first update, so the
	// mid-map latch must not open the gate.
	MapEnd();
	Check(!g_mapReady, "OnMapEnd closes the gate");
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(!g_mapReady, "an OnMapEnd before the first update keeps the gate shut");
	Check(g_clientCalls == 0, "no client call while the gate is shut");
	Check(steamlink::g_pushes.empty(), "nothing is pushed while the gate is shut");

	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(60000));
	Check(g_clientCalls == 0 && steamlink::g_pushes.empty(),
		  "the gate stays shut until the next OnMapStart");

	MapStart();
	Check(g_mapReady, "OnMapStart opens the gate");
	ResetClientCounters();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.size() == 1U, "a player is pushed once the map is up");
	Check(g_clientCalls > 0, "the update queries the client list with a ready map");

	// A map change closes the gate again, and the loop makes no call in the window (B2).
	MapEnd();
	ResetClientCounters();
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(!g_mapReady, "a map change closes the gate");
	Check(g_clientCalls == 0 && steamlink::g_pushes.empty(),
		  "no client call and no push between OnMapEnd and the next OnMapStart");

	MapStart();
	ResetClientCounters();
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.size() == 1U, "the next map pushes players again");
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1,
		  "the map listeners stay registered across maps");

	g_serverListPlayersFixPlugin.OnPluginEnd();
	Check(g_mapStartUnregisters == 1 && g_mapEndUnregisters == 1,
		  "OnPluginEnd unregisters the map listeners");
	Check(g_onMapStart == nullptr && g_onMapEnd == nullptr,
		  "the map callbacks are gone from s2sdk");
}

// --- B. a plugin loaded into a running map -----------------------------------------------------

void TestMidMapLatch() {
	PrepareTest();
	g_serverListPlayersFixPlugin.OnPluginStart();
	SetHuman(0, "Alice", 76561198000000001ULL, 12);
	ResetClientCounters();

	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(1000));
	Check(g_mapReady, "the first update opens the gate for a plugin loaded into a running map");
	Check(g_clientCalls == 0, "the latch itself makes no client call");
	Check(steamlink::g_pushes.empty(), "the 5 s interval still holds after the latch");

	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(4000));
	Check(steamlink::g_pushes.size() == 1U, "the first 5 s push happens");

	// The latch is used exactly once: an update after OnMapEnd cannot reopen the gate by itself.
	MapEnd();
	Check(!g_mapReady, "OnMapEnd closes the gate of the mid-map plugin");
	ResetClientCounters();
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(!g_mapReady && g_clientCalls == 0, "the closed gate stays closed after OnMapEnd");
	Check(steamlink::g_pushes.empty(), "nothing is pushed between maps");

	g_serverListPlayersFixPlugin.OnPluginEnd();
}

// --- C. the client, controller and schema guards (PITFALLS C2/B5) ------------------------------

void TestClientGuards() {
	PrepareTest();
	g_serverListPlayersFixPlugin.OnPluginStart();
	MapStart();

	// 0: a plain human - pushed.
	SetHuman(0, "Alice", 76561198000000001ULL, 12);
	// 1: a bot - connected and in game, but no Steam identity to publish (PITFALLS B5).
	g_clients[1] = FakeClient{true, true, false, true, "Bot", 76561198000000002ULL};
	g_controllers[1].handle = 1001;
	g_controllers[1].pointerPresent = true;
	g_controllers[1].score = 5;
	// 2: connected but not in game.
	g_clients[2] = FakeClient{true, false, false, false, "Bob", 76561198000000003ULL};
	// 3: the SourceTV proxy.
	g_clients[3] = FakeClient{true, true, true, true, "SourceTV", 76561198000000004ULL};
	// 4: in game, but the slot resolves to no controller at all.
	SetHuman(4, "Carol", 76561198000000005ULL, 1);
	g_controllers[4].pointerPresent = false;
	// 5: the controller handle is stale.
	SetHuman(5, "Dave", 76561198000000006ULL, 2);
	g_controllers[5].handleValid = false;
	// 6: the handle exists, the slot's entity pointer is stale.
	SetHuman(6, "Eve", 76561198000000007ULL, 3);
	g_controllers[6].pointerValid = false;
	// 7: a live slot without a SteamID64 (not authenticated yet).
	SetHuman(7, "Frank", 0, 4);
	// 8: s2sdk cannot resolve the name -> the original's empty string.
	SetHuman(8, "", 76561198000000008ULL, 5);
	// 9: a zero score is a valid score.
	SetHuman(9, "Grace", 76561198000000009ULL, 0);
	// 10: a negative CEntityHandle (high serial bit 31 set, PITFALLS C1) is a valid controller.
	SetHuman(10, "Hank", 76561198000000010ULL, 17);
	g_controllers[10].handle = -4242;
	// 63: the last slot of the range.
	SetHuman(63, "Zoe", 76561198000000063ULL, 99);

	ResetClientCounters();
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));

	Check(steamlink::g_pushes.size() == 5U, "exactly the five valid humans are pushed");
	const steamlink::Push* slot0 = PushAt(0);
	Check(slot0 != nullptr && slot0->steamid == 76561198000000001ULL && slot0->name == "Alice" &&
			  slot0->score == 12,
		  "slot 0 is pushed with its SteamID64, name and m_iScore");
	const steamlink::Push* slot8 = PushAt(1);
	Check(slot8 != nullptr && slot8->steamid == 76561198000000008ULL && slot8->name.empty(),
		  "slot 8: \"<unknown>\" becomes an empty name");
	const steamlink::Push* slot9 = PushAt(2);
	Check(slot9 != nullptr && slot9->name == "Grace" && slot9->score == 0,
		  "slot 9: a zero score is pushed as 0");
	const steamlink::Push* slot10 = PushAt(3);
	Check(slot10 != nullptr && slot10->steamid == 76561198000000010ULL &&
			  slot10->name == "Hank" && slot10->score == 17,
		  "slot 10 (negative high-serial CEntityHandle) is pushed with its m_iScore");
	const steamlink::Push* slot63 = PushAt(4);
	Check(slot63 != nullptr && slot63->steamid == 76561198000000063ULL &&
			  slot63->name == "Zoe" && slot63->score == 99,
		  "slot 63 (the last slot) is pushed");

	// PITFALLS B5: the bot is never asked for a SteamID64 and its id never reaches Steam.
	Check(!HasSlot(g_steamIdSlots, 1), "a bot's SteamID64 is never requested");
	Check(HasSlot(g_steamIdSlots, 7), "the SteamID64 of a live but unauthenticated slot is read");
	Check(!HasSlot(g_nameSlots, 7) && !HasSlot(g_schemaSlots, 7),
		  "a zero SteamID64 stops the slot before the name and score reads");
	// PITFALLS C2: the guards stop every unusable slot before the next, more expensive call.
	Check(!HasSlot(g_handleSlots, 2), "a client that is not in game is never resolved to a handle");
	Check(!HasSlot(g_handleSlots, 3), "the SourceTV proxy is never resolved to a handle");
	Check(!HasSlot(g_handleSlots, 4), "a slot without a controller pointer is never resolved");
	Check(HasSlot(g_handleSlots, 5) && !HasSlot(g_schemaSlots, 5),
		  "an invalid controller handle stops before the schema read");
	Check(HasSlot(g_handleSlots, 6) && !HasSlot(g_schemaSlots, 6),
		  "an invalid controller pointer stops before the schema read");
	Check(HasSlot(g_schemaSlots, 0) && HasSlot(g_schemaSlots, 10) && HasSlot(g_schemaSlots, 63),
		  "the schema read runs for the pushed controllers");
	Check(g_schemaCalls == 5, "the schema is read once per pushed player");
	Check(g_lastSchemaClass == "CCSPlayerController" && g_lastSchemaField == "m_iScore" &&
			  g_lastSchemaElement == 0,
		  "the score is CCSPlayerController::m_iScore, element 0");

	g_serverListPlayersFixPlugin.OnPluginEnd();
}

// --- D. the slot count: 64, the GetMaxClients clamp and its fallback ---------------------------

void TestMaxPlayers() {
	PrepareTest();
	g_serverListPlayersFixPlugin.OnPluginStart();
	MapStart();
	SetHuman(23, "TwentyThree", 76561198000000023ULL, 23);
	SetHuman(24, "TwentyFour", 76561198000000024ULL, 24);
	SetHuman(63, "SixtyThree", 76561198000000063ULL, 63);

	// GetMaxClients() = 24: only slots 0..23 are walked.
	g_maxClients = 24;
	ResetClientCounters();
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	const steamlink::Push* limited = PushAt(0);
	Check(steamlink::g_pushes.size() == 1U && limited != nullptr &&
			  limited->name == "TwentyThree",
		  "GetMaxClients limits the walk");
	Check(!HasSlot(g_handleSlots, 24) && !HasSlot(g_handleSlots, 63),
		  "no slot at or past GetMaxClients is queried");

	// GetMaxClients() = -1 (gpGlobals not initialised): the original's fixed 0..63 range.
	g_maxClients = -1;
	ResetClientCounters();
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.size() == 3U, "a -1 GetMaxClients falls back to the 64 slots");
	Check(HasSlot(g_handleSlots, 63), "slot 63 is walked with the fallback");

	// GetMaxClients() = 0 is not a real answer either - the same 64-slot fallback applies.
	g_maxClients = 0;
	ResetClientCounters();
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.size() == 3U, "a 0 GetMaxClients falls back to the 64 slots");

	// GetMaxClients() = 1000 is clamped to the 64 slots (no out-of-range slot is touched).
	g_maxClients = 1000;
	ResetClientCounters();
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.size() == 3U, "GetMaxClients is clamped to 64 slots");

	// No binding at all: the fixed 64-slot fallback.
	__s2sdk_GetMaxClients = nullptr;
	ResetClientCounters();
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.size() == 3U, "a missing GetMaxClients falls back to 64 slots");

	// The gate is shut between maps: no slot is walked, GetMaxClients is not even asked.
	PrepareTest();
	g_serverListPlayersFixPlugin.OnPluginStart();
	MapEnd();
	SetHuman(0, "Alice", 76561198000000001ULL, 12);
	ResetClientCounters();
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(10000));
	Check(g_maxClientsCalls == 0 && g_clientCalls == 0,
		  "GetMaxClients and the client list are not touched while the gate is shut");
}

// --- E. the steam_api retry and the 5 s cadence ------------------------------------------------

void TestSteamRetry() {
	PrepareTest();
	steamlink::g_initSucceeds = false;
	const plg::PluginResult startResult = g_serverListPlayersFixPlugin.OnPluginStart();
	Check(startResult.GetCode() == plg::PluginCode::Ok,
		  "a missing steam_api does not fail OnPluginStart (PITFALLS A1)");
	Check(steamlink::g_initCalls == 1 && !steamlink::g_available,
		  "a missing steam_api is reported at start instead of failing the plugin");
	MapStart();
	SetHuman(0, "Alice", 76561198000000001ULL, 12);

	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_initCalls == 2, "the Steam game server API resolution is retried");
	Check(steamlink::g_pushes.empty(), "nothing is pushed while the Steam API is missing");

	steamlink::g_initSucceeds = true;
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(500));
	Check(steamlink::g_initCalls == 2, "the retry waits for its 1 s interval");
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(500));
	Check(steamlink::g_initCalls == 3 && steamlink::g_available, "the retry resolves the API");
	Check(steamlink::g_pushes.empty(), "the 5 s interval has only just started");

	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.size() == 1U, "the first push happens after the API is up");

	// The game server interface itself may be unavailable (the original's m_bSteamAPIReady).
	steamlink::g_gameServerUp = false;
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(10000));
	Check(steamlink::g_pushes.empty(), "nothing is pushed while the game server interface is down");
	steamlink::g_gameServerUp = true;
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.size() == 1U, "the loop resumes once the interface is up");

	g_serverListPlayersFixPlugin.OnPluginEnd();
}

// --- F. missing bindings degrade to "nothing is published" -------------------------------------

void TestMissingBindings() {
	// A. no map listeners: the gate can never open, so no client is ever queried.
	PrepareTest();
	__s2sdk_OnMapStart_Register = nullptr;
	g_serverListPlayersFixPlugin.OnPluginStart();
	Check(!g_mapListeners, "without the register bindings the map listeners stay unregistered");
	SetHuman(0, "Alice", 76561198000000001ULL, 12);
	ResetClientCounters();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(10000));
	Check(!g_mapReady, "the gate never opens without the map listeners");
	Check(g_clientCalls == 0 && steamlink::g_pushes.empty(),
		  "no client call and no push without the map listeners");
	g_serverListPlayersFixPlugin.OnPluginEnd();

	// B. no schema export: the player is still published, with the original's default score 0.
	PrepareTest();
	g_serverListPlayersFixPlugin.OnPluginStart();
	MapStart();
	SetHuman(0, "Alice", 76561198000000001ULL, 12);
	__s2sdk_GetEntSchema = nullptr;
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	const steamlink::Push* noSchema = PushAt(0);
	Check(steamlink::g_pushes.size() == 1U && noSchema != nullptr && noSchema->score == 0,
		  "a missing schema read publishes the player with score 0");
	g_serverListPlayersFixPlugin.OnPluginEnd();

	// C. no controller handle export: the controller cannot be addressed, so nothing is pushed.
	PrepareTest();
	g_serverListPlayersFixPlugin.OnPluginStart();
	MapStart();
	SetHuman(0, "Alice", 76561198000000001ULL, 12);
	__s2sdk_PlayerSlotToEntHandle = nullptr;
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.empty(), "without PlayerSlotToEntHandle nothing is pushed");
	g_serverListPlayersFixPlugin.OnPluginEnd();

	// D. no IsValidEntPointer export: the controller cannot be validated, so nothing is pushed.
	PrepareTest();
	g_serverListPlayersFixPlugin.OnPluginStart();
	MapStart();
	SetHuman(0, "Alice", 76561198000000001ULL, 12);
	__s2sdk_IsValidEntPointer = nullptr;
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.empty(), "without IsValidEntPointer nothing is pushed");
	g_serverListPlayersFixPlugin.OnPluginEnd();

	// E. a failed BUpdateUserData is counted as "not pushed", but the loop keeps going.
	PrepareTest();
	g_serverListPlayersFixPlugin.OnPluginStart();
	MapStart();
	SetHuman(0, "Alice", 76561198000000001ULL, 12);
	SetHuman(1, "Bob", 76561198000000002ULL, 13);
	steamlink::g_pushSucceeds = false;
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.size() == 2U, "a failing BUpdateUserData does not stop the other players");
	g_serverListPlayersFixPlugin.OnPluginEnd();
}

// --- G. a clean unload and the reload path (PITFALLS A7) ---------------------------------------

void TestUnload() {
	PrepareTest();
	g_serverListPlayersFixPlugin.OnPluginStart();
	MapStart();
	SetHuman(0, "Alice", 76561198000000001ULL, 12);
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.size() == 1U, "the plugin works before the unload");

	g_serverListPlayersFixPlugin.OnPluginEnd();
	Check(!g_mapReady, "OnPluginEnd closes the gate");
	Check(!g_mapListeners, "OnPluginEnd drops the listener bookkeeping");
	Check(steamlink::g_shutdownCalls == 1,
		  "OnPluginEnd forgets the resolved steam_api pointers (steamlink::Shutdown)");
	Check(g_onMapStart == nullptr && g_onMapEnd == nullptr, "the map callbacks are gone from s2sdk");
	Check(g_mapStartUnregisters == 1 && g_mapEndUnregisters == 1,
		  "OnPluginEnd unregisters both map listeners");

	// The host may call one more update after the end: it must not touch a client.
	ResetClientCounters();
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(10000));
	Check(g_clientCalls == 0 && steamlink::g_pushes.empty(),
		  "an update after OnPluginEnd makes no client call and pushes nothing");

	// plg reload: a fresh start re-registers everything exactly once and starts with the gate shut.
	g_serverListPlayersFixPlugin.OnPluginStart();
	Check(g_mapStartRegisters == 2 && g_mapEndRegisters == 2,
		  "a reload registers the map listeners again");
	Check(!g_mapReady, "a reload starts with the gate shut");
	MapStart();
	ResetPushes();
	g_serverListPlayersFixPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	Check(steamlink::g_pushes.size() == 1U, "the reloaded plugin pushes again");

	g_serverListPlayersFixPlugin.OnPluginEnd();
}

} // namespace

int main() {
	std::printf("serverlistplayersfix_mm tests (fake s2sdk bindings and steamlink, no engine)\n");

	InstallFakes();

	TestMapGate();
	TestMidMapLatch();
	TestClientGuards();
	TestMaxPlayers();
	TestSteamRetry();
	TestMissingBindings();
	TestUnload();

	std::printf("serverlistplayersfix_mm_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
