# serverlistplayersfix_mm (Plugify)

Plugify port of Source2ZE's ServerListPlayersFix (the plugin that shipped as addons/serverlistplayersfix_mm). Every 5 seconds it calls ISteamGameServer::BUpdateUserData for every connected player (steamid, controller name, score), which populates the player list of the Steam server browser. No methods, no commands, no config files. NOTE: s2sdk 2.18.2 already runs the same update every 5 seconds in its own core; installing this plugin is harmless (the calls are idempotent) but redundant.

Plugify port (fork) of **ServerListPlayersFix** by Poggu (Source2ZE) (Metamod (C++)).
Original: https://github.com/Source2ZE/ServerListPlayersFix
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, hudmenu (optional)

## Installation
Download `serverlistplayersfix_mm-1.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/serverlistplayersfix_mm`.

## License
GPL-3.0, see LICENSE.

