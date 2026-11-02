# lr_geoip (Plugify)

[LR] GEOIP: writes the country / region / city / country_code of every loaded player into the `<lr_table>_geoip` table, looked up in <configs>/geoip/GeoLite2-City.mmdb. Ported from the Pisex Levels Ranks module LR_ExStatsGeoIP. Owns its plgdb connection with the levels_ranks database settings and its own dependency-free MaxMind DB reader. Requires the levels_ranks core; no exported methods.

Plugify port (fork) of **LR_ExStatsGeoIP ([LR] GEOIP)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-lvl_ranks_modules
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2), levels_ranks (>=1.0.0)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `lr_geoip-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/lr_geoip`.

`plugins/levels_ranks` carry only the API headers of the plugins this one uses.

The release ships the MaxMind GeoLite2 databases (GeoLite2-City.mmdb, GeoLite2-Country.mmdb). This product includes GeoLite2 data created by MaxMind, available from https://www.maxmind.com.

## License
GPL-3.0, see LICENSE.

