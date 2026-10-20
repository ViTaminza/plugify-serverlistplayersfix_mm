# serverlistplayersfix_mm — Plugify C++ port of Source2ZE's "ServerListPlayersFix": pushes every
# player to the Steam server browser (ISteamGameServer::BUpdateUserData) every 5 seconds.
#
#   plugify-serverlistplayersfix_mm    the plugin shared library
#                                      (entry "bin/serverlistplayersfix_mm")
#
# Links pisexcompat (player/schema access on s2sdk) and resolves the Steam game server API through
# dlsym at runtime (src/steam_gameserver.cpp). No config files, no commands - like the original.

set(SERVERLISTPLAYERSFIX_MM_VERSION "1.0.2")

# The generated hudmenu binding globals come through pisexcompat (PUBLIC), which is what the
# manifest dependency on the optional hudmenu plugin is for; the s2sdk globals likewise.
add_library(plugify-serverlistplayersfix_mm SHARED
        src/plugin.cpp
        src/steam_gameserver.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(plugify-serverlistplayersfix_mm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the generated s2sdk/plg headers and pisexcompat are not ours to fix; their warnings must
# not fail -Werror on our sources.
target_include_directories(plugify-serverlistplayersfix_mm SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-serverlistplayersfix_mm PRIVATE
        pisexcompat
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
