# lr_geoip (Plugify)

[LR] GEOIP: writes the country / region / city / country_code of every loaded player into the `<lr_table>_geoip` table, looked up in <configs>/geoip/GeoLite2-City.mmdb. Ported from the Pisex Levels Ranks module LR_ExStatsGeoIP (production plugin lr_geoip). Owns its plgdb connection with the levels_ranks database settings and its own dependency-free MaxMind DB reader. Requires the levels_ranks core; no exported methods.

Plugify port (fork) of **LR_ExStatsGeoIP ([LR] GEOIP)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-lvl_ranks_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, levels_ranks, hudmenu (optional)

## Installation
Download `lr_geoip-1.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/lr_geoip`.

`plugins/levels_ranks` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

