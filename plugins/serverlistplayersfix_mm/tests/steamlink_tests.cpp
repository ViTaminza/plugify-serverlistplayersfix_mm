// Regression tests for src/steam_gameserver.cpp — the SteamInternal_ContextInit crash bug this
// plugin shared with PlayersInfo 1.1.0 (which is where it was confirmed on 27035; this plugin was
// never installed and never ran the path, the code was identical).
//
// The old implementation obtained ISteamGameServer through SteamInternal_ContextInit() with a
// zero-initialised 0x18-byte buffer. steam_api_internal.h:32-36 documents that buffer as
// `struct { void (*pFn)(void*); uintptr_t counter; void* ptr; }` and SteamInternal_ContextInit
// *calls pFn*, so the zeroed buffer was a null callback: the first query that reached steam_api
// crashed the server with SIGSEGV, RIP = 0 inside libsteam_api. The fix in PlayersInfo (see its
// tests/steamlink_tests.cpp) is the one applied here.
//
// The fix resolves the library's own flat accessor `SteamAPI_SteamGameServer_v015()` instead and
// never builds a context itself. This test compiles the real src/steam_gameserver.cpp and drives it
// through a fake dlsym(), so it can prove, at runtime and without steam_api, that
//
//   * SteamInternal_ContextInit is never requested (the regression guard),
//   * only the two documented flat symbols are requested, and exactly those decide availability,
//   * a missing accessor / a missing update wrapper fail soft (false, nullptr) and never leave a
//     half-resolved API behind that a later call could act on,
//   * an interface that is not up yet (the accessor answers nullptr) fails soft without calling
//     the update wrapper with a null instance,
//   * a live interface receives this plugin's ISteamGameServer pointer, SteamID, name and score,
//     its result is passed through, and a null name is sent as "",
//   * the accessor is asked on every call and the pointer is never cached: when the engine replaces
//     its game server interface, the next push goes to the new one (lifetime),
//   * Shutdown() forgets the resolved pointers and a later Init() resolves them again (the
//     `plg reload` path), and a failed Init() is retried once the symbols appear.
//
// No engine, no pisexcompat, no libsteam_api: only the plugin's own source is compiled in.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>

#include "steam_gameserver.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                             \
		}                                                                                          \
	} while (0)

// --- fake dynamic loader ----------------------------------------------------------------------

struct FakeSymbol {
	const char* name;
	void* address;
};

constexpr int kMaxSymbols = 4;
constexpr int kMaxRequests = 64;

FakeSymbol g_symbols[kMaxSymbols] = {};
int g_symbolCount = 0;

// Every name the production code asked for, so the test can prove what is (and is not) resolved.
const char* g_requests[kMaxRequests] = {};
int g_requestCount = 0;
bool g_contextInitRequested = false;

// The fake ISteamGameServer instances. Only their addresses matter: the update wrapper must
// receive exactly the pointer the accessor answered.
int g_instanceA = 0;
int g_instanceB = 0;

int g_accessorCalls = 0;
void* g_accessorResult = nullptr;

int g_updateCalls = 0;
void* g_updateSelf = nullptr;
std::uint64_t g_updateSteamId = 0;
std::uint32_t g_updateScore = 0;
std::string g_updateName;
bool g_updateResult = true;

void ResetFakes() {
	g_symbolCount = 0;
	g_requestCount = 0;
	g_contextInitRequested = false;
	g_accessorCalls = 0;
	g_accessorResult = nullptr;
	g_updateCalls = 0;
	g_updateSelf = nullptr;
	g_updateSteamId = 0;
	g_updateScore = 0;
	g_updateName.clear();
	g_updateResult = true;
}

void AddSymbol(const char* name, void* address) {
	if (g_symbolCount < kMaxSymbols) {
		g_symbols[g_symbolCount++] = {name, address};
	}
}

void AddAllSymbols(void* accessor, void* update) {
	AddSymbol("SteamAPI_SteamGameServer_v015", accessor);
	AddSymbol("SteamAPI_ISteamGameServer_BUpdateUserData", update);
}

bool WasRequested(const char* name) {
	for (int i = 0; i < g_requestCount; ++i) {
		if (g_requests[i] != nullptr && std::strcmp(g_requests[i], name) == 0) {
			return true;
		}
	}
	return false;
}

void* FakeSteamGameServer() {
	++g_accessorCalls;
	return g_accessorResult;
}

bool FakeBUpdateUserData(void* self, std::uint64_t steamId, const char* name, std::uint32_t score) {
	++g_updateCalls;
	g_updateSelf = self;
	g_updateSteamId = steamId;
	g_updateName = name != nullptr ? name : "";
	g_updateScore = score;
	return g_updateResult;
}

} // namespace

// The plugin's only dynamic-loader dependency. Defining it here (instead of linking libdl) is what
// makes src/steam_gameserver.cpp testable without steam_api: the production code calls the real
// `dlsym(RTLD_DEFAULT, ...)` symbol, which resolves to this function in the test binary.
extern "C" void* dlsym(void* handle, const char* name) noexcept {
	(void)handle;
	// glibc declares `name` nonnull, so no null check here: the production code passes literals.
	if (g_requestCount < kMaxRequests) {
		g_requests[g_requestCount++] = name;
	}
	if (std::strcmp(name, "SteamInternal_ContextInit") == 0) {
		g_contextInitRequested = true;
	}
	for (int i = 0; i < g_symbolCount; ++i) {
		if (std::strcmp(g_symbols[i].name, name) == 0) {
			return g_symbols[i].address;
		}
	}
	return nullptr;
}

namespace {

constexpr std::uint64_t kSteamId = 76561198000000001ULL;

void TestMissingSymbols() {
	std::printf("-- steamlink: no steam_api at all\n");
	steamlink::Shutdown();
	ResetFakes();

	std::string error;
	CHECK(!steamlink::Init(error));
	CHECK(!error.empty());
	CHECK(!steamlink::IsAvailable());
	CHECK(steamlink::GameServer() == nullptr);
	CHECK(!steamlink::BUpdateUserData(kSteamId, "player", 3));
	CHECK(g_updateCalls == 0);
	// The crash of the first builds: they resolved and called SteamInternal_ContextInit with a
	// zeroed buffer. It must never be looked up again.
	CHECK(!g_contextInitRequested);
	CHECK(!WasRequested("SteamInternal_ContextInit"));
}

void TestOnlyTheFlatSymbolsAreResolved() {
	std::printf("-- steamlink: only the flat symbols are resolved\n");
	steamlink::Shutdown();
	ResetFakes();
	g_accessorResult = &g_instanceA;
	AddAllSymbols(reinterpret_cast<void*>(&FakeSteamGameServer),
				  reinterpret_cast<void*>(&FakeBUpdateUserData));

	std::string error;
	CHECK(steamlink::Init(error));
	CHECK(steamlink::IsAvailable());
	CHECK(WasRequested("SteamAPI_SteamGameServer_v015"));
	CHECK(WasRequested("SteamAPI_ISteamGameServer_BUpdateUserData"));
	CHECK(!g_contextInitRequested);
	// Exactly the two documented flat entry points - no guessed interface version, no context
	// struct, nothing else.
	CHECK(g_requestCount == 2);

	// A resolved API short-circuits: no second lookup on the next call.
	const int requestsBefore = g_requestCount;
	CHECK(steamlink::Init(error));
	CHECK(g_requestCount == requestsBefore);
}

void TestAccessorMissing() {
	std::printf("-- steamlink: accessor missing, update wrapper present\n");
	steamlink::Shutdown();
	ResetFakes();
	AddSymbol("SteamAPI_ISteamGameServer_BUpdateUserData",
			  reinterpret_cast<void*>(&FakeBUpdateUserData));

	std::string error;
	CHECK(!steamlink::Init(error));
	CHECK(error.find("SteamAPI_SteamGameServer_v015") != std::string::npos);
	CHECK(!steamlink::IsAvailable());
	CHECK(steamlink::GameServer() == nullptr);
	CHECK(!steamlink::BUpdateUserData(kSteamId, "player", 3));
	CHECK(g_updateCalls == 0);
}

void TestUpdateWrapperMissing() {
	std::printf("-- steamlink: accessor present, update wrapper missing\n");
	steamlink::Shutdown();
	ResetFakes();
	g_accessorResult = &g_instanceA;
	AddSymbol("SteamAPI_SteamGameServer_v015", reinterpret_cast<void*>(&FakeSteamGameServer));

	std::string error;
	CHECK(!steamlink::Init(error));
	CHECK(error.find("SteamAPI_ISteamGameServer_BUpdateUserData") != std::string::npos);
	// A partially resolved API must not be usable: Init() clears what it resolved.
	CHECK(!steamlink::IsAvailable());
	CHECK(steamlink::GameServer() == nullptr);
	CHECK(!steamlink::BUpdateUserData(kSteamId, "player", 3));
	CHECK(g_accessorCalls == 0);
	CHECK(g_updateCalls == 0);
}

void TestInterfaceNotUpYet() {
	std::printf("-- steamlink: symbols present, interface not up (nullptr)\n");
	steamlink::Shutdown();
	ResetFakes();
	g_accessorResult = nullptr; // the engine has not initialised its game server API yet
	AddAllSymbols(reinterpret_cast<void*>(&FakeSteamGameServer),
				  reinterpret_cast<void*>(&FakeBUpdateUserData));

	std::string error;
	CHECK(steamlink::Init(error));
	CHECK(steamlink::IsAvailable());
	CHECK(steamlink::GameServer() == nullptr);
	// The wrapper must not be called with a null instance. The accessor is asked once by the
	// GameServer() check above and once more by BUpdateUserData - it is never cached.
	CHECK(!steamlink::BUpdateUserData(kSteamId, "player", 3));
	CHECK(g_accessorCalls == 2);
	CHECK(g_updateCalls == 0);
}

void TestLiveInterface() {
	std::printf("-- steamlink: live interface\n");
	steamlink::Shutdown();
	ResetFakes();
	g_accessorResult = &g_instanceA;
	AddAllSymbols(reinterpret_cast<void*>(&FakeSteamGameServer),
				  reinterpret_cast<void*>(&FakeBUpdateUserData));

	std::string error;
	CHECK(steamlink::Init(error));
	CHECK(steamlink::IsAvailable());
	CHECK(steamlink::GameServer() == &g_instanceA);

	CHECK(steamlink::BUpdateUserData(kSteamId, "Alice", 12));
	CHECK(g_updateCalls == 1);
	// The wrapper has to receive the interface the accessor returned - never null, never a context
	// or counter value (which is what the crashed build passed to libsteam_api).
	CHECK(g_updateSelf == &g_instanceA);
	CHECK(g_updateSteamId == kSteamId);
	CHECK(g_updateName == "Alice");
	CHECK(g_updateScore == 12);

	// The wrapper's answer is passed through unchanged.
	g_updateResult = false;
	CHECK(!steamlink::BUpdateUserData(kSteamId, "Alice", 12));
	CHECK(g_updateCalls == 2);

	// A null name is sent as an empty string, never as a null pointer.
	g_updateResult = true;
	CHECK(steamlink::BUpdateUserData(kSteamId, nullptr, 0));
	CHECK(g_updateName.empty());
	CHECK(g_updateScore == 0);
}

void TestServerPointerIsNotCached() {
	std::printf("-- steamlink: the accessor is asked again on every push\n");
	steamlink::Shutdown();
	ResetFakes();
	g_accessorResult = &g_instanceA;
	AddAllSymbols(reinterpret_cast<void*>(&FakeSteamGameServer),
				  reinterpret_cast<void*>(&FakeBUpdateUserData));

	std::string error;
	CHECK(steamlink::Init(error));
	CHECK(steamlink::BUpdateUserData(kSteamId, "Alice", 1));
	CHECK(g_updateSelf == &g_instanceA);
	CHECK(g_accessorCalls == 1);

	// The engine deactivated and recreated its game server interface: the next push must reach the
	// new instance, not a pointer cached by this plugin.
	g_accessorResult = &g_instanceB;
	CHECK(steamlink::BUpdateUserData(kSteamId, "Alice", 2));
	CHECK(g_updateSelf == &g_instanceB);
	CHECK(g_accessorCalls == 2);

	// And back to "not up yet": the wrapper is not called at all.
	g_accessorResult = nullptr;
	CHECK(!steamlink::BUpdateUserData(kSteamId, "Alice", 3));
	CHECK(g_updateCalls == 2);
	CHECK(g_accessorCalls == 3);
}

void TestRetryAfterFailure() {
	std::printf("-- steamlink: a failed resolution is retried\n");
	steamlink::Shutdown();
	ResetFakes();
	std::string error;
	CHECK(!steamlink::Init(error));
	CHECK(!steamlink::IsAvailable());

	// steam_api is mapped into the process later (the plugin's OnPluginUpdate retries).
	g_accessorResult = &g_instanceA;
	AddAllSymbols(reinterpret_cast<void*>(&FakeSteamGameServer),
				  reinterpret_cast<void*>(&FakeBUpdateUserData));
	CHECK(steamlink::Init(error));
	CHECK(steamlink::IsAvailable());
	CHECK(steamlink::BUpdateUserData(kSteamId, "Alice", 7));
	CHECK(g_updateSelf == &g_instanceA);
}

void TestShutdownAndReload() {
	std::printf("-- steamlink: shutdown and re-init\n");
	steamlink::Shutdown();
	ResetFakes();
	g_accessorResult = &g_instanceA;
	AddAllSymbols(reinterpret_cast<void*>(&FakeSteamGameServer),
				  reinterpret_cast<void*>(&FakeBUpdateUserData));

	std::string error;
	CHECK(steamlink::Init(error));
	CHECK(steamlink::IsAvailable());

	steamlink::Shutdown();
	CHECK(!steamlink::IsAvailable());
	CHECK(steamlink::GameServer() == nullptr);
	const int accessorCallsBefore = g_accessorCalls;
	const int updateCallsBefore = g_updateCalls;
	CHECK(!steamlink::BUpdateUserData(kSteamId, "Alice", 1));
	CHECK(g_accessorCalls == accessorCallsBefore); // nothing may be called after Shutdown
	CHECK(g_updateCalls == updateCallsBefore);

	// OnPluginEnd dropped the pointers; a reload resolves them again.
	CHECK(steamlink::Init(error));
	CHECK(steamlink::IsAvailable());
	CHECK(steamlink::BUpdateUserData(kSteamId, "Alice", 1));
	CHECK(g_updateCalls == updateCallsBefore + 1);
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("serverlistplayersfix_mm steamlink tests (real steam_gameserver.cpp, fake dlsym)\n");

	TestMissingSymbols();
	TestOnlyTheFlatSymbolsAreResolved();
	TestAccessorMissing();
	TestUpdateWrapperMissing();
	TestInterfaceNotUpYet();
	TestLiveInterface();
	TestServerPointerIsNotCached();
	TestRetryAfterFailure();
	TestShutdownAndReload();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
