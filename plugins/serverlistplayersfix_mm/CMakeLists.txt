# serverlistplayersfix_mm — Plugify C++ port of Source2ZE's "ServerListPlayersFix": pushes every
# player to the Steam server browser (ISteamGameServer::BUpdateUserData) every 5 seconds.
#
#   plugify-serverlistplayersfix_mm    the plugin shared library
#                                      (entry "bin/serverlistplayersfix_mm")
#
# Pure s2sdk: no pisexcompat and no guard library (docs/PLUGIN_STANDARD.md §7). The map gate, the
# controller checks and the m_iScore schema read live in src/plugin.cpp itself. The Steam game
# server API is resolved through dlsym at runtime (src/steam_gameserver.cpp). No config files, no
# commands - like the original.

set(SERVERLISTPLAYERSFIX_MM_VERSION "1.1.2")

# The __s2sdk_* binding globals the plugify module fills from the manifest's dependencies.
add_library(plugify-serverlistplayersfix_mm SHARED
        src/plugin.cpp
        src/steam_gameserver.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(plugify-serverlistplayersfix_mm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/plg headers are not ours to fix; they must not fail
# -Werror on our sources.
target_include_directories(plugify-serverlistplayersfix_mm SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-serverlistplayersfix_mm PRIVATE
        plugify_sdk
        ${CMAKE_DL_LIBS}
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-serverlistplayersfix_mm PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-serverlistplayersfix_mm PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="serverlistplayersfix_mm"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-serverlistplayersfix_mm PROPERTIES OUTPUT_NAME serverlistplayersfix_mm)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-serverlistplayersfix_mm PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/serverlistplayersfix_mm.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/serverlistplayersfix_mm.pplugin"
)

# --- tests --------------------------------------------------------------------------------------
# serverlistplayersfix_mm_tests: engine-free. The plugin TU is compiled into the test (its loop and
# guards live in an anonymous namespace), every __s2sdk_* binding slot and the steamlink entry
# points are recording fakes: map gate, client/controller guards, the 5 s cadence, the
# GetMaxClients clamp, the steam_api retry and the unload rules.
#
# serverlistplayersfix_mm_steamlink_tests: the *real* src/steam_gameserver.cpp against a fake
# dlsym() - proves the interface comes from the official SteamAPI_SteamGameServer_v015 accessor
# (never from a zero-initialised SteamInternal_ContextInit context, the bug that crashed PlayersInfo
# 1.1.0 on 27035), that the update wrapper receives the current interface pointer (also after the
# engine recreates it), and that a missing symbol / a half-resolved API / a not-yet-up interface
# fails soft and is retried. The test defines dlsym() itself, so it needs neither libdl nor
# libsteam_api.
#
# Both are written into build/tests so ../test.sh runs them with the rest (*_tests convention).
#
# The top-level CMakeLists declares the PLUGIFY_PORT_TESTS option *after* the plugin directories,
# so on a first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
    add_executable(serverlistplayersfix_mm_tests
            tests/serverlistplayersfix_mm_tests.cpp
            $<TARGET_OBJECTS:s2sdk_bindings>
    )
    target_include_directories(serverlistplayersfix_mm_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(serverlistplayersfix_mm_tests SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_link_libraries(serverlistplayersfix_mm_tests PRIVATE
            plugify_sdk
            -static-libstdc++ -static-libgcc
    )
    target_compile_options(serverlistplayersfix_mm_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(serverlistplayersfix_mm_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            PLUGIFY_MODULE="serverlistplayersfix_mm"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
    )
    set_target_properties(serverlistplayersfix_mm_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    # The accessor/lifetime regression test: the production translation unit, not a stub.
    add_executable(serverlistplayersfix_mm_steamlink_tests
            tests/steamlink_tests.cpp
            src/steam_gameserver.cpp
    )
    target_include_directories(serverlistplayersfix_mm_steamlink_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(serverlistplayersfix_mm_steamlink_tests PRIVATE
            -static-libstdc++ -static-libgcc
    )
    target_compile_options(serverlistplayersfix_mm_steamlink_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(serverlistplayersfix_mm_steamlink_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
    set_target_properties(serverlistplayersfix_mm_steamlink_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
