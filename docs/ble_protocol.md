# Watch ↔ phone Bluetooth protocol

The watch (`firmware/main/ble.cpp`, `settings_table.cpp`) and the Android app
(`android/`, `Protocol.kt`) share this format. Change both sides together and bump the
version byte when an old peer would misread the new layout.

## Roles: the watch decides when to talk

The watch is the BLE **central**, the phone the **peripheral**. The phone advertises all
the time from a foreground service and hosts the GATT server. The watch only powers its
radio on a full wake (a touch, the hourly full wake, or for *Auto* under an hour, a full
wake scheduled for the sync), and only when the auto sync interval (5 min to 6 h, or
Off) has passed:

1. The radio starts with the display refresh (~0.6 s), a continuous passive scan looks
   for the phone's advertisement. The stack takes about 300 ms to come up.
2. The scan stops when the refresh has ended and it has lasted at least 120 ms (one
   advertising interval of the phone), or after 500 ms in any case. No phone heard:
   radio off, which costs the stack start plus up to 500 ms of scanning (~0.8 s).
3. When it was heard, the flags in the advertisement and the watch's own state decide
   whether a connection is needed at all. Nothing new on either side: radio off right
   away. The phone can't notice such an attempt: the watch counts it as *Seen*, the
   app's *Last seen* is the last time the watch connected.
4. Otherwise the watch connects (7.5–15 ms interval), encrypts with the stored key,
   reads the app's changes, writes its values and disconnects. Connecting needs one
   more advertisement of the phone to be heard, given up to 1 s. The whole session is
   capped at 1.8 s from radio on; *Sync now* and *Pair phone* search again when the
   connection didn't happen.

The phone asks for a 100 ms advertising interval, but the watch hears it less often:
measured, finding it took 13–720 ms from the scan start (median ~120 ms), and the
next report after one came 140–550 ms later. Connecting, encrypting and the exchange
take ~500 ms more (see *Measured on the device*). The scan reports every advertisement (no
duplicate filter): the controller's filter goes by address only, so after one
advertisement was turned down (*Pair phone* started before the app's pairing screen)
it would hide the phone's later ones with the new flags.

With `kLogSessions` set in `ble.cpp` (debug, off by default) the watch logs one line
per session with the time each step ended, in ms from the session start (-1 = not
reached), and the step that failed: `ble: Synced nvs 3 init 250 up 292 scan 293
refresh 210 found 340 conn 450 enc 500 talk 600 disc 620 done 660 ms, 0 retries` (an
example of the format, not a measurement; retries: searches again after a connection
that didn't happen). `nvs`, `init`, `up`: flash, controller and host ready;
`refresh`: the display refresh ended (background sync only); `talk`: the exchange
ended. See *Measured on the device* below.

Menu flows (*Pair phone*, *Sync now*) use the same session with longer limits and
BACK to cancel.

## Advertisement (phone)

Legacy connectable advertising (`ADV_IND`), 100 ms interval (250 ms with *Fast
advertising* off). Manufacturer specific data, company id `0xFFFF`:

| Offset | Size | Field                                   |
|-------:|-----:|-----------------------------------------|
| 0      | 2    | Company id `FF FF`                      |
| 2      | 2    | `'L' 'K'`                               |
| 4      | 1    | Protocol version, `2`                   |
| 5      | 1    | Flags, below                            |
| 6      | 4    | Token (u32 LE), `0` while pairing       |

Flags:

| Bit    | Name          | Meaning                                                   |
|--------|---------------|-----------------------------------------------------------|
| `0x01` | `PAIRING`     | The app is on its pairing screen                          |
| `0x02` | `PENDING`     | Changes made in the app wait for the watch                |
| `0x04` | `WANT_STATE`  | The app has no values from the watch                      |
| `0x08` | `WANT_SCHEMA` | The app has no settings description, or an outdated one   |

The scan response carries the service UUID, for debugging tools. The watch scans
passively and never asks for it.

The token is a random u32 the watch picks when pairing. It tells the paired phone apart
from any other phone running the app; the link key is what secures the exchange.

## Pairing and keys

*Bluetooth › Pair phone* on the watch scans for an advertisement with `PAIRING`,
connects and starts LE Secure Connections pairing. The watch is `DisplayOnly`, so it
shows a 6 digit passkey. While the app is pairing it catches Android's
`ACTION_PAIRING_REQUEST` for the device connected to its GATT server (priority 999,
ahead of the system dialog), stops the broadcast and asks for the code on a number pad,
then answers with `setPin()`; Android turns the digits into the LE passkey. A phone that
doesn't let the broadcast be stopped shows its own dialog as well, either works. After
bonding the watch writes `SCHEMA` and `STATE` with the `JUST_PAIRED` flag and a new
token, which the app adopts.

Android refuses to pair again with a device it still has a bond with. Before pairing,
and on *Forget watch*, the app removes the phone's bond with the watch through the hidden
`BluetoothDevice.removeBond()`; where a phone refuses that, Forget opens Bluetooth
settings instead. The bond of a pairing that failed halfway is removed too, but only for
the device that sent the pairing request and calls itself `LightInk`: the GATT server
callbacks report every LE link of the phone (headphones, ...), never trust those.

Pairing again goes through forgetting first, on both sides: the watch menu shows *Pair
phone* or *Forget phone*, the app *Pair watch* or *Forget watch*.

Only the Pair menu may pair. Android, as a bonded peripheral, sends an SMP security
request on each connection. NimBLE's key store is RAM only and empty on a sync, and with
no key there NimBLE answers the request with a new pairing, even on a link the watch
already encrypted with the saved key: Android then shows a pairing request, and deletes
its bond when the watch hangs up at the end of the sync (measured 2026-11-11, on the first
sync after pairing). So the session writes the saved key into NimBLE's store when the
link comes up, and NimBLE's own pairing on "key missing" is off
(`MYNEWT_VAL_BLE_RESTART_PAIR=0`, `main/CMakeLists.txt`): a phone without the bond ends
the sync with `NeedsPair`, nothing is asked on the phone.

The watch has no advertising, but it serves a GAP service (`ble_svc_gap`, name
`LightInk`, appearance 0x00C0 watch) so Android shows its name instead of its MAC. That
needs NimBLE's peripheral role for the GATT server (+12 KB flash). During a session the
watch rejects Android's connection parameter update requests (`BLE_ERR_CONN_PARMS`):
Android asks for 45 ms right after connecting, which made the exchange about 6 times
longer on air.

The app's foreground notification is there while pairing, while changes wait for the
watch, and with *Always reachable* on (the default; then on a minimum-importance
channel, which Android shows at low importance at least for a foreground service). With *Always reachable* off the
service runs without it while the app is open and Android stops it a minute or two
after (measured: ~2 min): the watch then finds no phone until the app is opened again,
and each automatic sync costs the stack start plus the whole 500 ms scan.

Android advertises from a resolvable private address that rotates, and the ESP32
controller has no LL privacy, so NimBLE could not match a later connection to its bond.
The watch therefore keeps the link key itself (`BlePeer`: LTK, EDIV, Rand, key size,
in NVS namespace `lightink`, key `ble_peer`, cached in RTC memory) and encrypts with
`ble_gap_encryption_initiate()`. NimBLE's own bond store stays in RAM. The watch uses
its public address, so the phone finds its bond.

What survives what:

| Data | Where | Reset / `idf.py flash` | `idf.py erase-flash` |
|------|-------|------------------------|----------------------|
| Pairing (`BlePeer`: token, key) | NVS, copy in RTC | kept, reloaded on cold boot | lost, pair again |
| Bluetooth stats, *Auto* interval, sent hashes | RTC (`kSettings.mBle`) | back to defaults | lost |
| Watch settings (the table, app and stored-only entries) | RTC, saved to NVS hourly when changed | kept, up to an hour of changes lost | lost, defaults sent to the app |
| Phone side: bond, token, schema, values | Android bond store, app preferences | kept | kept (remove the bond to pair again) |

`idf.py flash` writes the bootloader, partition table, otadata and the app, never the
NVS partition at 0x9000.

If the phone dropped the bond (forgotten in Android's settings), encryption fails with
*PIN or key missing* and the watch shows *Phone lost the pairing*.

## GATT service (phone)

Service `4c494748-5449-4e4b-0000-000000000000` (`LIGHTINK` in hex). All
characteristics need an encrypted, authenticated link.

| UUID suffix | Name      | Access       | Content            |
|-------------|-----------|--------------|--------------------|
| `…0001`     | `STATE`   | write        | Watch values       |
| `…0002`     | `PENDING` | read         | App changes        |
| `…0003`     | `SCHEMA`  | write        | Settings described |

The watch finds the service by UUID each time and reuses the characteristic handles
from the last discovery while the service start handle is the same (Android numbers a
service's attributes in a row).

All numbers little endian. Values travel as *records*, the same format the watch keeps
in flash (see *Settings table* below): `id u8, length u8, data`.

Writes go in pieces, each a plain Write Request of `offset u16, total u16, data`, at
most MTU − 7 bytes of data; the app joins them and handles the value when `total` is
reached (offset 0 starts a value over). ATT long (prepared) writes aren't used: on a
Galaxy with Android 17 the watch aborted the SCHEMA long write after the second
prepared piece. The tables below describe the joined value.

### `STATE`, written by the watch at the end of every exchange

| Offset | Size | Field                                                     |
|-------:|-----:|-----------------------------------------------------------|
| 0      | 1    | Version, `2`                                              |
| 1      | 1    | Flags: `0x01` `JUST_PAIRED`                               |
| 2      | 4    | Token                                                     |
| 6      | 4    | Schema hash (CRC-32 of the `SCHEMA` blob)                 |
| 10     | 2    | Ack rev: the `PENDING` rev read in this connection, else 0 |
| 12     | 2    | Battery mV                                                |
| 14     | 2    | Battery, 0.1 % units                                      |
| 16     | 4    | Watch time, UTC seconds                                   |
| 20     | n    | Every app value, as records                               |

The phone rejects a `STATE` whose token isn't its own (unless pairing). It drops the
pending changes with a rev up to the ack rev: later edits keep waiting. A schema hash
different from the one it knows (firmware update) raises `WANT_SCHEMA`.

### `PENDING`, read by the watch when the advertisement says `PENDING`

| Offset | Size | Field                              |
|-------:|-----:|------------------------------------|
| 0      | 1    | Version, `2`                       |
| 1      | 2    | Rev: the app's edit counter (u16)  |
| 3      | n    | Changed values, as records         |

The watch clamps each value to the setting's range, skips unknown ids, applies them
after the display refresh and redraws when something changed.

### `SCHEMA`, written by the watch when pairing, after a firmware change, or on `WANT_SCHEMA`

| Size | Field                                                            |
|-----:|------------------------------------------------------------------|
| 1    | Version, `2`                                                     |
| 1    | Count                                                            |
|      | Per setting:                                                     |
| 1    | Id                                                               |
| 1    | Kind: 0 bool, 1 int, 2 enum (value = option index), 3 UTC offset in minutes |
| 4    | Min (i32)                                                        |
| 4    | Max (i32)                                                        |
| n    | Group, NUL terminated                                            |
| n    | Name, NUL terminated                                             |
| n    | Enum options joined by `\|`, NUL terminated (empty otherwise)    |

The app has no list of settings of its own: it builds its screens from this. To expose
a setting, add a line to the table in `settings_table.cpp`. Ids are stable: never
reuse or renumber one.

## Settings table: one format for RTC, flash and the app

`firmware/main/settings_table.cpp` lists every persistent setting in `kSettings` once:
stable id, kind, range, and how to read and write the field. Entries flagged `kApp` are
in the schema and travel to and from the app; the others (RTC calibration, touch pad
map and cycles, battery tuning, alarms) are stored only. The same table and the same
records serve the three copies:

| Copy | Content | When |
|------|---------|------|
| RTC (`kSettings`) | The structs the firmware uses | Always |
| NVS key `settings` | Format byte `1`, then records of every entry | Loaded on a cold boot. Saved by `SettingsTable::save()` at the end of a full wake when an hour passed since its last check *and* the CRC of the records changed |
| App | Records of the `kApp` entries, schema | Each sync |

Records: `id u8, length u8, data`. Numbers are signed little endian of 1, 2 or 4 bytes
(the field's size; the app sends 4). A few small fixed structs go as their raw bytes
(`Blob`, stored only), with a check before they're applied. A reader skips unknown ids
and records whose length doesn't fit the entry, and clamps numbers to the range: a blob
stored by another firmware version still loads, the rest keep their defaults.

Power: a normal wake only compares the time of the last check. Once an hour the records
are encoded and hashed (~100 bytes, microseconds); flash is written only when they
changed. NVS appends: the ~120 byte blob takes about 6 of a page's 126 entries, so even
a change every hour fills a 4 KB page in about a day, and the pages rotate. Flash
sectors last ~100k erases: centuries at that rate. Up to an hour of changes is lost on
a reset.

The pairing key is a separate NVS key (`ble_peer`), written only when pairing.

## Power notes

- Radio on-time per automatic attempt, measured (2026-11-14): *Seen* 320–630 ms,
  *Synced* ~1.0–1.2 s, *No phone* ~0.8 s (stack start plus the 500 ms scan; it was the
  whole 1.2 s before the scan limit). The current isn't measured; at ~100 mA that is
  ~10–17 µAh for *Seen* and ~22 µAh for *No phone*. At the default 1 h interval that is
  5–11 times the ~2 µA idle draw, so for the solar budget choose *Auto: 6h* or *Off*
  (*Sync now* still works).
- No light sleep while Bluetooth is on: with the main crystal as its sleep clock
  (`CONFIG_BTDM_CTRL_LPCLK_SEL_MAIN_XTAL`) the controller holds a no-light-sleep lock
  from enable to disable, and an APB-max lock while the radio is awake. The CPU idles
  (clock gated) between events; the scan keeps the radio on all the time anyway. The
  board's 32 kHz oscillator could be the sleep clock
  (`CONFIG_BTDM_CTRL_LPCLK_SEL_EXT_32K_XTAL`, allowed with `RTC_CLK_SRC_EXT_OSC`): tried,
  the start was the same and one exchange ran slow, not adopted without a current
  measurement.
- Below 10 % battery the watch doesn't sync on its own.
- *Auto* 1h and 6h add no wakes: they use the hourly full wake and touches. *5min* and
  *15min* replace the wake stub's cheap minute updates by a full boot plus a sync every
  interval: at 5 min that is 12 sessions an hour, roughly 12 × 10–17 µAh, about 60–100
  times the idle draw. They skip the extra wakes at night (*Night* in Power, 0–6 am) and
  below 10 % battery. No wake stub change.

## Measured on the device

2026-11-09, ESP32-PICO-D4 (HW v4) and a Galaxy Z Fold (SM-F971B, Android API 37).

- **Pairing works**, after two fixes. The app showed the watch's settings, battery
  97 % (4.16 V) and the last sync. User, after the fix: "Seem to be working so far".
- **Long writes failed on this phone.** The first pairing bonded, then the watch's
  SCHEMA long write (~780 B) stopped after the second prepared piece and the watch hung
  up 18 ms later. NimBLE aborts a long write when the echo of a piece doesn't match, so
  the phone's echo of piece two was most likely off; the watch log wasn't captured to
  confirm. This phone's GATT goes through Android's new Rust stack. Fixed by sending
  plain writes of pieces (above).
- **A failed pairing leaves a stale bond on the phone.** Android then refuses the next
  pairing from the same watch ("key missing" dialog, "disconnected due to bond loss").
  Remove the watch from Android's Bluetooth settings (it shows as its address) before
  pairing again; the app's pairing screen says so. Apps can't remove a bond.
- **Timings of the pairing session**, ms from radio on: stack up 292, phone found 2125
  (the app was put in pairing mode after the watch started), connected 2410, encrypted
  13352 (typing the code), done 13547. The exchange after encryption, with the schema,
  took ~195 ms.
- **Flash writes failed at the low rail, so the pairing never survived a reset.** With
  no `Power::Lock` held, HW v4 runs at ~1.9 V: NVS reads work, but writes return OK and
  then read back as missing (`ble_peer` was absent; the NVS used-entry count grew 5 per
  write without the key ever being found). Holding the rail high fixed it at once:
  create and update read back 109 B, and an update survived a reset with the same hash.
  `Storage` now holds `Power::Flag::Flash` for every NVS call that may write. Pairings
  made before this fix have to be made again, once.
- **After the flash fix, a pairing survives flashing.** Flashed twice after pairing: the
  watch reconnected and encrypted with its stored key both times, no pairing again.
- **Android reads the name** from the GAP service: "RNR received expected name ...
  HCI_SUCCESS" right after the first connection (it timed out without the service).
- **Connection parameter request rejected**: Android's update came back with status 59
  (0x3B) and the link stayed at 15 ms; the next exchange (read, 4 schema pieces, state)
  took ~150 ms of GATT. With 45 ms accepted, requests had been ~90 ms apart.
- **Notification**: up while 2 changes waited, gone after the sync that delivered them,
  then Android stopped the background service.
- Not tried on the device yet: the number pad code entry and removeBond() (both need a
  new pairing).

2026-11-14, same watch and phone, about 140 automatic sessions with `kLogSessions`, a
sync forced on every wake and scripted touches (trace build, rail held high):

- **Stack start ~300 ms**, the same on every wake: flash init 21 ms, controller 13 ms,
  NimBLE host ready ~265 ms after that.
- **The display refresh takes ~1.0–1.1 s** in the trace build (menus and watchface)
  and ~585 ms in a normal build, not ~200 ms, so the 120 ms minimum scan never came
  into play.
- **Phone present**: *Seen* in 317–1007 ms, phone found 13–720 ms after the scan
  started (median ~120 ms). *Synced* (the daily refresh, values sent): stack 300,
  found +47–100, connected +160, encrypted +118, exchange +240–255, disconnected +16,
  radio off at 894–946 ms.
- **Phone not advertising**: every session ran into the 1.2 s cap (*No phone*). Before
  the test the app's service had been stopped by Android since the evening before, and
  in the morning it lived only ~2 min after the app was opened (`dumpsys
  bluetooth_manager`, GATT server register/unregister): this is what makes syncs fail
  and *Sync now* wait its 10 s.
- **Duplicate filter**, test build ignoring the phone's first advertisement: with the
  controller's filter on, 24 of 24 sessions never heard the phone again (*No phone*);
  with it off, 17 of 17 heard it again 140–550 ms later. A found-late session that must
  connect doesn't fit the 1.2 s cap: found at 710 and 870 ms, the exchange was cut
  (*Failed*, at discover and schema); found at 693 ms it ended at 1198 ms.
- Android's advertising set: interval 160 (100 ms), legacy, connectable, -7 dBm, one of
  9 sets on the phone. Why the watch hears it every 140–550 ms instead of every 100 ms
  is open; likely the phone's radio sharing air time: Galaxy Buds3 Pro were connected
  (classic and LE) and playing audio during all these measurements.
- **Found within the scan limit**: with the app on the screen 96 % within 400 ms and
  98 % within 500 ms of the scan start (152 sessions); with the app in the background
  (foreground service) ~80 % and ~89 % (35 sessions).
- **Connecting** took 160–640 ms after the phone was found; the old 300 ms connect
  window made *Sync now* and some background syncs fail at `connect`. With 1 s and a
  new search after a failed connect, *Sync now* synced in 724 ms.
- **Always reachable on** (quiet foreground notification): the service and its GATT
  server stayed up for 3.5 h in the background (16:58 to 20:30, then still running).
  Android raises a foreground service's notification to at least low importance
  (`importance=2` in `dumpsys notification`), so the minimum-importance channel still
  shows its icon on some phones.
- **Where the stack start goes**: HCI timings through linker wraps (test build). The
  host task is created at 34 ms but only runs at ~265 ms: the controller keeps core 0
  busy after it is enabled. With the host on core 1 it starts at 34 ms, but its first
  command (HCI Reset) waits 229 ms for the controller, so the host is up at ~275 ms
  (25 ms saved). The rest of the startup sequence takes ~30 ms. The same ~235 ms with
  the trace off, with modem sleep off, with software coexistence off and with the
  32 kHz sleep clock: it is inside the closed controller library.
