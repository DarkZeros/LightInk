# Watch ↔ phone Bluetooth protocol

The watch (`firmware/main/ble.cpp`, `remote_settings.cpp`) and the Android app
(`android/`, `Protocol.kt`) share this format. Change both sides together and bump the
version byte when an old peer would misread the new layout.

## Roles: the watch decides when to talk

The watch is the BLE **central**, the phone the **peripheral**. The phone advertises all
the time from a foreground service and hosts the GATT server. The watch only powers its
radio on a full wake (a touch, the hourly full wake, or for *Auto* under an hour, a full
wake scheduled for the sync), and only when the auto sync interval (5 min to 6 h, or
Off) has passed:

1. The radio starts with the display refresh, a continuous passive scan looks for the
   phone's advertisement.
2. When the refresh ends and the phone wasn't heard, the radio goes off: no phone costs
   one refresh window of radio time, about 200 ms.
3. When it was heard, the flags in the advertisement and the watch's own state decide
   whether a connection is needed at all. Nothing new on either side: radio off right
   away.
4. Otherwise the watch connects (7.5–15 ms interval), encrypts with the stored key,
   reads the app's changes, writes its values and disconnects. The whole session is
   capped at 1.2 s from radio on.

With the phone advertising every 100 ms, finding it takes 0–110 ms and connecting
about one more advertising interval. With `kLogSessions` set in `ble.cpp` (debug, off
by default) the watch logs one line per session with the times, and the step that
failed: `ble: Synced sync 292 found 2125 conn 2410 enc 13352 done 13547 ms` (ms from
radio on). See *Measured on the device* below.

Menu flows (*Pair phone*, *Sync now*) use the same session with longer limits and
BACK to cancel.

## Advertisement (phone)

Legacy connectable advertising (`ADV_IND`), 100 ms interval (250 ms with *Fast
advertising* off). Manufacturer specific data, company id `0xFFFF`:

| Offset | Size | Field                                   |
|-------:|-----:|-----------------------------------------|
| 0      | 2    | Company id `FF FF`                      |
| 2      | 2    | `'L' 'K'`                               |
| 4      | 1    | Protocol version, `1`                   |
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
shows a 6 digit passkey and Android asks for it (dialog, or a notification). After
bonding the watch writes `SCHEMA` and `STATE` with the `JUST_PAIRED` flag and a new
token, which the app adopts.

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
| Watch settings | RTC (`kSettings`), not saved yet | back to defaults, sent to the app at the next sync | lost |
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

All numbers little endian. A *value triple* is `id u8, value i16`.

Writes go in pieces, each a plain Write Request of `offset u16, total u16, data`, at
most MTU − 7 bytes of data; the app joins them and handles the value when `total` is
reached (offset 0 starts a value over). ATT long (prepared) writes aren't used: on a
Galaxy with Android 17 the watch aborted the SCHEMA long write after the second
prepared piece. The tables below describe the joined value.

### `STATE`, written by the watch at the end of every exchange

| Offset | Size | Field                                                     |
|-------:|-----:|-----------------------------------------------------------|
| 0      | 1    | Version, `1`                                              |
| 1      | 1    | Flags: `0x01` `JUST_PAIRED`                               |
| 2      | 4    | Token                                                     |
| 6      | 4    | Schema hash (CRC-32 of the `SCHEMA` blob)                 |
| 10     | 2    | Ack rev: the `PENDING` rev read in this connection, else 0 |
| 12     | 2    | Battery mV                                                |
| 14     | 2    | Battery, 0.1 % units                                      |
| 16     | 4    | Watch time, UTC seconds                                   |
| 20     | 3n   | Every value, as triples                                   |

The phone rejects a `STATE` whose token isn't its own (unless pairing). It drops the
pending changes with a rev up to the ack rev: later edits keep waiting. A schema hash
different from the one it knows (firmware update) raises `WANT_SCHEMA`.

### `PENDING`, read by the watch when the advertisement says `PENDING`

| Offset | Size | Field                              |
|-------:|-----:|------------------------------------|
| 0      | 1    | Version, `1`                       |
| 1      | 2    | Rev: the app's edit counter (u16)  |
| 3      | 3n   | Changed values, as triples         |

The watch clamps each value to the setting's range, skips unknown ids, applies them
after the display refresh and redraws when something changed.

### `SCHEMA`, written by the watch when pairing, after a firmware change, or on `WANT_SCHEMA`

| Size | Field                                                            |
|-----:|------------------------------------------------------------------|
| 1    | Version, `1`                                                     |
| 1    | Count                                                            |
|      | Per setting:                                                     |
| 1    | Id                                                               |
| 1    | Kind: 0 bool, 1 int, 2 enum (value = option index), 3 UTC offset in minutes |
| 2    | Min (i16)                                                        |
| 2    | Max (i16)                                                        |
| n    | Group, NUL terminated                                            |
| n    | Name, NUL terminated                                             |
| n    | Enum options joined by `\|`, NUL terminated (empty otherwise)    |

The app has no list of settings of its own: it builds its screens from this. To expose
a setting, add a line to the table in `remote_settings.cpp`. Ids are stable: never
reuse or renumber one.

## Power notes

- One attempt with no phone around keeps the radio on for about one refresh window plus
  the stack start. Estimate, not measured yet: ~100 mA for ~250 ms is ~7 µAh per
  attempt. At the default 1 h interval that is several times the ~2 µA idle draw, so
  for the solar budget choose *Auto: 6h* or *Off* (*Sync now* still works). The log
  line above gives the real on-time.
- Below 10 % battery the watch doesn't sync on its own.
- *Auto* 1h and 6h add no wakes: they use the hourly full wake and touches. *5min* and
  *15min* replace the wake stub's cheap minute updates by a full boot plus a sync every
  interval: at 5 min that is 12 sessions an hour, roughly 12 × 7 µAh, about 40 times
  the idle draw. They skip the extra wakes at night (*Night* in Power, 0–6 am) and
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
- **Open: the stack start.** 292 ms to bring NimBLE and the controller up is longer than
  the ~200 ms refresh window, and a background sync gives up when the refresh ends. If
  later sessions show the same, an automatic sync can never find the phone: start the
  radio earlier in the wake or widen the window. One sample so far, maybe including
  a first PHY calibration.
