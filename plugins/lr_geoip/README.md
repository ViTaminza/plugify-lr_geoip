# lr_geoip — Plugify port of the Pisex LR module "[LR] ExStats GeoIP"

On every levels_ranks player load, looks the client's address up in
`<configs>/geoip/GeoLite2-City.mmdb` and writes `country`, `region`, `city` and `country_code`
into the core's `<lr_table>_geoip` table (one row per SteamID).

Pure `s2sdk` + `plgdb` (PLUGIN_STANDARD §7): the plugin has no pisexcompat and no hudmenu
dependency. The database is looked up in `<configs>/geoip/`, then `<configs>/lr_geoip/`, then the
Metamod layout `<GetBaseDir()>/addons/configs/geoip/`, and finally in the original relative path
`addons/configs/geoip/`; the first file found is used.

```bash
./build.sh plugify-lr_geoip lr_geoip_tests
.scratch/prodreview/t1.sh lr_geoip_tests
```

## Deploy notes

- **Update the .mmdb atomically.** The database is mmapped, so replacing it in place with `cp`
  (which truncates the destination) lets a lookup that is running during the copy touch pages
  past the new end of the file: the kernel delivers SIGBUS and the server dies. Copy to a
  temporary file in the directory the plugin picked (usually `<configs>/geoip/`, see above) and
  rename it over the old one; the plugin notices the new size/mtime on the next player load and
  reopens the file:

  ```bash
  cp GeoLite2-City.mmdb.new <configs>/geoip/.GeoLite2-City.mmdb.tmp &&
      mv <configs>/geoip/.GeoLite2-City.mmdb.tmp <configs>/geoip/GeoLite2-City.mmdb
  ```

- **Existing tables keep the old `clientip` width.** Builds before 2026-10-24 created
  `clientip varchar(16)`, which is too short for an IPv6 address (up to 45 characters).
  `CREATE TABLE IF NOT EXISTS` does not alter an existing table, so on a database that already
  has the table run once:

  ```sql
  ALTER TABLE `<lr_table>_geoip` MODIFY `clientip` varchar(45) NOT NULL default '';
  ```

  New tables are created with `varchar(45)` (SQLite uses `TEXT` and needs nothing).
