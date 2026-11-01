# SSD1681 findings (GDEH0154D67, LightInk HW v4)

Results of the experiments in [display_tests.md](display_tests.md), run on
2026-11-01 with the driver's fixed temperature (`0x1A 32 00`, 50 °C) and
rotation 2. Confidence: **confirmed** = the picture matched one hypothesis and
agrees with other tests; **likely** = one test, consistent; **open** = needs a
follow-up.

Naming: the panel is black/white only, so the two RAMs are called the **front
buffer** (`0x24`, "BW RAM" in the SSD1681 datasheet: the new image) and the **back
buffer** (`0x26`, "RED RAM": the previous image, which is what's on the glass
after a refresh), as in the panel's B/W documentation and the driver.
The raw answers below are quoted as given, when the tests still said BW/RED.

## Working model (what the driver and the trace viewer should assume)

1. **Two RAM banks, each with its own X/Y address counter.** `0x24` writes bank
   *k*, `0x26` bank *1−k*. `0x4E`/`0x4F` set the counters (both); a write advances
   only its own bank's counter, and it continues across commands, deep sleep and HW
   reset. (T12, T14, T49)
2. **Every update that displays (`04`) or sets mode 2 (`08`) flips *k*:** the banks
   and their counters swap, over the whole screen. Updates with only analog/clock
   bits (`C0`, `03`) or only a LUT load (`D0`) don't. (T04–T11, T49, T56, T57, T61)
3. **What the glass gets:**
   - **A full refresh (mode 1) doesn't reliably show the front buffer.** After OTP
     partials it alternated with their count (T58–T60); after custom-LUT partials
     it didn't (T67/T68), and the state survives SWRESET. Software can't predict it,
     so **write the frame to both buffers before a full refresh** (T63).
   - **Partial (mode 2)** drives each pixel with the waveform for back → front,
     including the same-colour waveforms:
   - OTP partial: BB pushes towards black (grey after 1), WW only weakly (grey
     after a few).
   - Custom `N_1` LUTs: BB/WW have an intentional 1-unit "sustain" phase.
   (T01, T02, T53–T55)
4. **Writes wrap to the window start only when the counter equals the window end.**
   X counts through 6 bits (0–63). Missing parameters keep their old values.
   (T28–T31)
5. **X− mirrors whole pixels, bits included; the TB bit mirrors Y.** (T27, T52, T46)
6. **HW reset after `0x10 01` keeps everything** (RAM, counters, window, `0x11`,
   `0x21`, `0x22`, `0x3C`, custom LUT, temperature). (T14, T16, T20–T26)
   - `0x12` SWRESET clears the RAM to white and resets the registers. Without
     `0x1A` afterwards the OTP LUT is empty (92 ms update, no waveform). (T18)
   - `0x10 03` (mode 2) loses the controller state within a minute. (T50)

## RAM banks and refresh

- **Every `0x20` swaps the two buffers (front ↔ back), whether or not it displays.**
  Seen with `0x22` = `DC`, `D4`, `D8` (no display bit), `F7`, `FF` and `CC` with a
  custom LUT (T04–T09). **Confirmed.**
  - After a refresh, front holds the *previous* frame and back the one now on the glass.
  - The wake stub's re-write of the tens digit after a refresh (`redrawDec`) is
    the workaround for this.
- **Refresh and swap cover the whole screen, ignoring the RAM window** (T10, T11). **Confirmed.**
- **Full (mode 1) showed front in T01**, but not reliably: which buffer a full
  refresh shows depends on the refreshes before it (T58–T63, T67/T68, below).
- **Partial (mode 2, OTP LUT) drives every pixel with the waveform for
  (back → front), and the same-colour waveforms are not no-ops** (T02, T12, T32, T34):
  - back = front = black on white glass → **grey**.
  - back = front = white on black glass → black **fades** (T15, T18).
  - So a back bank that doesn't match the glass gives grey or faded areas, not
    "unchanged" pixels. **Confirmed.**
- **The driver's `0x22 D8` / `0x20` "2-buffer switch" after a full refresh isn't
  needed** (T03: partial works the same without it). It costs 92 ms of BUSY time
  and is itself a swap (T06). **Likely.**

## Address counters and window

- **Each RAM has its own X/Y counter, and both are set by `0x4E`/`0x4F`** (T12: the
  `0x26` write started at the programmed line, not where `0x24` stopped). **Likely.**
- **A new `0x24` continues where the last one stopped**, even across deep sleep +
  HW reset (T14). **Likely.**
- **After `0x20` the next `0x24` starts at the window start** (T13). **Open:** this
  fits "the counters swap with the banks" (the back buffer's counter was never advanced). It
  needs a test that advances the `0x26` counter before the refresh.
- **Counters wrap to the window start only when they *equal* the window end.**
  - A counter set outside the window runs to the end, then wraps (T30: a 1 px
    line from B's corner into A, then A fills).
  - Start > end in X+ runs through the whole 6-bit X range (0–63, columns 25–63
    are invisible) and wraps around the screen (T31).
  - **Confirmed** (T28, T30, T31).
- **Commands with fewer parameters keep the old values of the missing ones**
  (T29: `0x45` with 3 parameters kept a previous end-high bit of 1). **Confirmed.**
  The driver is fine as long as nothing ever writes that byte.
- **Entry modes (`0x11`) work as documented: Y-first works** (T25), and the start
  corner follows X±/Y± (T27). **Confirmed.**
- **In X− the bit order inside each byte is mirrored too** (T27: E and F stripes
  start on opposite sides). That would make it a free horizontal mirror.
  **Likely**; confirm with an asymmetric pattern.

## What survives what

| | Deep sleep `0x10 01` + HW reset | Deep sleep `0x10 03` + HW reset | `0x12` SWRESET |
|---|---|---|---|
| RAM | kept (T16) | kept for 50 ms (T17); state lost after 60 s (T50) | **cleared to white** (T18 rerun) |
| X/Y counter, window | kept (T14) | – | **open** (T15 rerun: no stripes visible) |
| `0x21` RAM options | kept (T20) | – | – |
| `0x22` sequence | kept (T21, BUSY 1911 ms = `D4`) | – | – |
| `0x3C` border | kept (T22) | – | – |
| Custom LUT | kept (T24) | – | – |
| `0x11` entry mode | kept (T26) | – | – |

- **HW reset from deep sleep mode 1 loses nothing tested.** The driver's RTC
  caching (`kState` in `display.cpp`) and the wake stub's bare `0x20` are safe.
- **Deep sleep mode 2 (`0x10 03`) also kept RAM** over a short sleep. `display.cpp`
  says mode 2 "doesn't work". It may draw less current: worth measuring with the
  PPK2 and a longer sleep.
- **SWRESET is open.**
  - After it, the `DC` readout took only **92 ms** of BUSY time instead of 300 ms.
    That is the same as a no-display update, so the LUT for the reset temperature
    register probably has no real waveform.
  - The "white, faded text" picture therefore doesn't prove the RAM was cleared.
  - Retest with `0x1A` sent again after `0x12`.

## RAM options and border

- **`0x21` works in mode 2 for every option tested:** front inverse (T19), back
  inverse (T32), front bypass-as-0 (T33: all black), back bypass-as-0 (T34).
- **Border `0x3C`, after a full refresh:**

| `0x3C` | `00` | `01` | `02` | `03` | `05` | `40` | `50` | `60` | `80` | `C0` |
|---|---|---|---|---|---|---|---|---|---|---|
| Border | black | white | black | white | white | black | black | white | white* | white* |

\* VCOM / HiZ: probably unchanged rather than driven white, since the border was
white before.
- In GS-transition mode, bit 0 picks white (1) or black (0).
- The driver's `02` (dark) and `05` (light) both behave as intended.

## Geometry

- **T45, MUX = 100 lines (`0x01 63 00 00`):** A and F stayed white. One half of
  the screen flashed during the update, and the undriven half faded slightly to
  grey. **Open**, and low priority: the panel is fixed at 200 lines.
- **T46, `0x01` TB bit (`C7 00 01`): the picture is flipped vertically** (seen as
  "mirrored in X and rotated 180°", which is the same thing). The gate scan
  direction works: a free vertical mirror. **Confirmed.** Together with X− bit
  mirroring (T27), the controller could do 180° rotation itself.
- **T47, `0x0F 32 00` (gate scan start, documented for the SSD1675): a line
  artifact about 50 lines from the screen edge**, so the command exists and acts
  at line 50. **Open:** did the picture itself move by 50 lines and wrap?

## BUSY time per update (`0x1A` = 50 °C)

| `0x22` | What | BUSY |
|---|---|---|
| `D4` | full, OTP LUT | 1912 ms |
| `F7` | full + read temperature + analog/clock off | 2057 ms |
| `DC` | partial, OTP LUT | 300 ms |
| `FF` | POR value: partial + temperature + off | 445 ms |
| `CC` | custom LUT `CUSTOM_2_1`, no LUT load | 152 ms |
| `CC` | custom LUT `CUSTOM_6_1`, no LUT load | 233 ms |
| `D8` | no display (analog on + LUT load) | 92 ms |
| `C0` | analog on only | 92 ms |
| `03` | analog + clock off only | 142 ms |
| `DC` after SWRESET | temperature register reset | 92 ms |

(T48 repeated the same numbers within 1 ms.)

- **About 92 ms of every update is the analog power-on (booster)**: `C0` alone
  takes as long as `D8`. A partial `DC` is then ~210 ms of waveform, a full `D4`
  ~1820 ms.
- **Powering analog and clock down (`03`) takes 142 ms.** `FF`/`F7` pay that plus
  the temperature read (~145 ms in total) on every update. The driver's `DC`/`D4`
  leave analog on until the `0x10` deep sleep instead.
- **The SWRESET readout (92 ms) ran no waveform at all**, only the analog power-on.
  That fits the "SWRESET: open" note above: the reset temperature register
  selects an empty LUT.
- After the `F7` run (real temperature, not 50 °C), text came out grey (T07).

## Consequences for the driver

1. **Because of the swap,** front after a refresh holds the frame before last. Either:
   - write the union of the last two frames' changed areas, or
   - write the changed area again right after the refresh (generalise
     `redrawDec`), so both banks hold the new frame.
2. **Never let back drift from the glass.** In partial mode a stale back shows up
   as grey or fading, not as untouched pixels.
3. **Drop the `0xD8` switch update:** saves 92 ms and one extra swap per FULL → partial change.
4. **Measure deep sleep mode 2** for lower display sleep current.
5. **The controller can mirror both axes itself** (X− bit mirroring and the TB
   bit), which might replace software rotation.
6. **Custom LUTs are the fastest updates:** `2_1` at 152 ms, `6_1` at 233 ms, vs
   300 ms for the OTP partial. They're also the most controllable, since the
   same-colour waveforms can be made true no-ops.

## Second run (T15/T18 fixed, T47 again, T49–T51)

- **The address counters swap with the RAM banks** (T49: band at F). So
  `0x24`/`0x26` simply address bank *k* / bank *1−k*, each bank keeps its own X/Y
  counter, and every `0x20` flips *k*. This explains T12 (S) and T13 (W). **Confirmed.**
- **SWRESET clears the RAM to white** (T18 rerun: the readout now ran a real
  300 ms waveform, box A white, text faded by the WW waveform). **Confirmed.**
  T15 rerun showed no stripes at all. If the window/counter went back to POR, they
  were written along the screen edge at RAM (0,0), possibly under the bezel.
  **Open**, and irrelevant to the driver, which always sets window and counter.
- **Deep sleep mode 2 loses the controller state within a minute** (T50). After
  60 s the readout ran the 92 ms empty update: the registers (at least the
  temperature) were gone, so the white box doesn't directly test the RAM.
  Mode 2 is only usable with a full re-init and a rewrite of both banks on every
  wake. **Confirmed** as "not a drop-in replacement for mode 1".
- **`0x0F 32 00` (T47): only a line visible during the waveform**, about 50 lines
  from RAM line 0. The final picture is not shifted. The command exists but
  doesn't move the image. **Likely**; not useful for the driver.
- **T51 froze 3 s in**, before its first sleep phase, while writing RAM. The trace
  stopped mid-frame and nothing came after. The "white box" seen was the
  preparation picture, so T51 has no result yet. Suspected cause: auto light sleep
  leaving UART0 forced to XOFF.
  - Fixed in `3cb26ad`: no light sleep in test mode, and the trace forces XON and
    then drops the frame instead of hanging.
  - A recovery shows up as a warning on the next SLEEP in the viewer.

Raw answers: T15: white, no stripes. T18: faded text, white box. T47: just a
visible line during the e-ink flashes, but all looks ok afterwards; around 0
lines from the bottom (rotated, so actually 50 lines from 0). T49: black bar in
F. T50: white after 1 min, still white. T51: no current meter; box is white,
then the device hung and buttons were not responsive.

## Third run (T52–T57)

- **X− mirrors the bit order too** (T52: E's line at its left edge, F's at its
  right). X− is a true horizontal pixel mirror; with the TB bit (T46) the
  controller can rotate 180° by itself. **Confirmed.**
- **OTP partial, same-colour waveforms** (T53/T54):
  - BB on white glass turns grey after one refresh.
  - WW on black glass leaves it black after one refresh, grey after five.
  - Both drive weakly and asymmetrically. **Confirmed.**
- **Custom `2_1` LUT** (T55): A and B both grey after one update, A darker. This
  matches the LUT's design: phase 2 drives BB = black and WW = white for one unit
  ("update the pixels that don't change a minimal amount").
  - `SSD1681_LIGHTMYINK_CUSTOM(N, 0)` would make same-colour pixels true no-ops.
  - With sustain, repeated updates pull the glass towards front even where back is
    stale (self-correcting). Without it, stale pixels never change.
- **`0x22 C0` (analog on) and `03` (analog/clock off) don't swap** (T56, T57: A
  black, B white = no change). `D8` does (T06). **Open:** which of display
  (`04`), mode 2 (`08`) or LUT load (`10`) triggers the swap. It doesn't matter
  for the driver, which only uses displaying updates.
- No trace stalls or freezes in this run (light sleep off in test mode).

Raw answers: T52: E line at the left, F at the right. T53: A is grey, B/C are
black, D is white. T54: A and B are grey, C and D same as before. T55: same as
T54, maybe the grey is darker in A than B. T56: no change, black then white.
T57: same, but the text is not very visible (grey).

## Status of the consequences (2026-11-01)

- **Done:** dropped the `0xD8` update on refresh-mode changes (`5033876`). It was
  the only path that broke "back = glass", on every menu ↔ watchface LUT switch.
- **Done:** in FULL mode every RAM write goes to both `0x24` and `0x26`, in the app
  driver and in the wake stub (`5033876`, after T58–T63). Leaving a FAST/custom
  menu for a FULL watchface kept showing the menu whenever the swap parity was
  wrong. The old extra `D0` update didn't help: it doesn't swap.
- **Already correct:**
  - The app writes the whole frame into front before each refresh (`writeAll`).
  - `Watchface::draw()` re-writes its changed areas after the refresh and
    tracks which units digit each bank holds.
  - The wake stub re-writes the tens digit after the refresh (`redrawDec`).
- **Done:** the trace viewer's model follows the working model above (`706b5a4`).
  Replaying the test captures reproduces every modelled result.
  - Which bank a full refresh shows can't be known from the trace, so the viewer
    shows front and flags full refreshes where front ≠ back.
- **Done:** every rotation in hardware, checked on the watch for 0/90/180/270°.
  - Adafruit-GFX always draws at rotation 0.
  - For 90°/270° the buffer holds the transposed picture (a byte = 8 vertical
    pixels). The controller can't transpose bits, so that part stays in software
    as an x/y swap.
  - The entry mode does the rest: X mirrored for 90°/180°, Y for 180°/270°
    (`0x11` = `03`, `02`, `00`, `01`). The RAM window starts at its corner; one
    shared function (`ramWindow`) serves the app driver and the wake stub.
- **Decided:** keep the custom LUTs' sustain phase (`N_1`). Refreshing old pixels a
  little on every update avoids the greys and shadows that build up otherwise.
- **Open:**
  - Deep sleep mode 2: not usable without a full re-init per wake.

## Fourth run (T58–T63): full refresh after partial ones

Found on the watch: Menu Lut = FAST (or a custom LUT), Watch Lut = FULL. Leaving
the menu sent the watchface to front and ran a real full refresh (1915 ms BUSY), but
the glass kept the menu until the next minute's refresh (capture
`build/captures/lutswitch.bin`, refresh #11).

| # | Before the full `D4` | Result |
|---|---|---|
| 58 | 0 partials | **not updated** (A white) |
| 59 | 1 partial `DC` | updated |
| 60 | 2 partials `DC` | **not updated** |
| 61 | 1 partial, then `D0` (old driver) | updated |
| 62 | 1 custom `CC` partial | updated |
| 63 | 1 partial, frame in both buffers | updated |

Conclusions:
- **A full refresh shows a fixed physical bank.** The parity of swaps decides
  whether that is the bank `0x24` just wrote. **Confirmed.**
- **The parity isn't reset by SWRESET:** every test's prepare does one, yet T01
  (front) and T58 (not front) differ. **Likely.**
- **`0x22 D0` (LUT load, no display, no mode 2) doesn't swap** (T61 = T59). With
  T06 (`D8` swaps) and T56 (`C0` doesn't), the swap is tied to the display (`04`)
  or mode 2 (`08`) bit, not to the LUT load. **Likely.**
- **The LUT doesn't matter** (T62).
- **Writing both buffers fixes it** (T63). **Confirmed.**
- This bug predates `5033876`: the old driver's extra `D0` update before a FULL
  refresh didn't change the parity.

Raw answers: T58: shows white. T59: black. T60: white. T61: black. T62: black.
T63: black.

## Fifth run (T64–T70): RAM ping-pong and gate modes

| # | Test | Result |
|---|---|---|
| 64 | `0x37` ping-pong off, OTP partial (`DC`) | **swap** |
| 65 | Ping-pong off, custom `2_1` (`CC`) | **swap** |
| 66 | Ping-pong on, custom `2_1` (`CC`) | **swap** |
| 67 | Ping-pong off, 1 custom partial, then full | updated |
| 68 | Ping-pong off, 2 custom partials, then full | updated |
| 69 | `0x01` GD bit | white lines between text rows |
| 70 | `0x01` SM bit | picture doubled in Y |

- **The swap can't be turned off:** `0x37` byte F bit 6 ("RAM ping-pong for
  Display Mode 2" in the SSD1680/1681 datasheets) changes nothing, with or without
  an OTP LUT load. Either this controller ignores it or the OTP overrides it.
  **Confirmed.** So the driver keeps re-writing changed areas after a refresh.
- **Which bank a full refresh shows also depends on the kind of partials before
  it.** With custom-LUT partials it showed the front buffer after both 1 and 2 of
  them (T67/T68), where OTP partials alternated (T58–T60). The rule is unknown.
  Writing both buffers before a full refresh is right either way. **Open**, and
  harmless.
- **GD and SM** (the `0x01` gate options) only reorder or interlace whole rows:
  shifted rows with gaps (GD), half the rows doubled in Y (SM). With TB (T46) and
  the entry mode, nothing in the register set moves a bit to another row, so 90°
  and 270° rotation needs the software transpose. **Confirmed.**

Raw answers: T64: swap, white black. T65: same. T66: same. T67: black. T68:
black. T69: white, many lines of text have white lines in between. T70: 2x Y
dimension.

## Sixth run (T71–T75): refreshing a band of rows

| # | Test | BUSY per refresh | Result |
|---|---|---|---|
| 71 | OTP partial, all 200 rows, 5× | 300 ms | Square toggles, ends black |
| 72 | OTP partial, 32-row band, 5× | **300 ms** | Same, no visible degradation |
| 73 | OTP partial, band, 21× | **300 ms** | Columns above/below the square fade to grey very quickly, the rest of the unscanned rows much less |
| 74 | Custom `2_1`, all rows, 5× | 152 ms | No degradation; square a little less black (shorter LUT) |
| 75 | Custom `2_1`, band, 21× | **152 ms** | Everything outside the band fades to grey |

- **Limiting the refresh to a band of rows (`0x01` MUX + `0x0F` start) doesn't
  speed it up.** BUSY is set by the LUT (frames × frame period), not by the
  number of scanned rows. **Confirmed.**
- **It degrades the rows that aren't scanned.** They still sit on the source
  (column) lines that carry the update's voltages: the columns of the changing
  pixels fade fastest, and with a sustain LUT, which drives every column,
  everything outside the band greys. Columns can't be limited at all (T10).
  **Confirmed.**
- So the driver keeps refreshing all 200 rows. To make updates faster, change the
  LUT (fewer frames, or a higher frame rate via its FR field), not the row count.

Raw answers: T71: I can see a square in the center blinking white and black,
ended in black. T72: same, no degradation. T73: same but the column fades to
grey very quickly, and the rest as well but much less. T74: no degradation at
all, but the center is not as black as the others at the end, probably due to
the custom LUT. T75: everything fades to grey, except the row where the square
is.

## Seventh run (T76): which bank a full refresh shows

20 full refreshes (`D4`) in one run, each after one operation, front = big F, back =
big B (see display_tests.md). Only the change between consecutive steps matters.

| Step | Before the full | Seen | Mode 2 updates since the previous full |
|---|---|---|---|
| 1–3 | nothing | F F F | 0 |
| 4 | 1 × `DC` | **B** | 1 |
| 5 | 2 × `DC` | F | 2 |
| 6 | `D8` | **B** | 1 |
| 7 | `D0` | F | 0 |
| 8 | `C0` | F | 0 |
| 9 | `0x32` LUT load | F | 0 |
| 10 | LUT + 1 × `CC` | **B** | 1 |
| 11 | LUT + 2 × `CC` | F | 2 |
| 12–17 | `0x37` off, LUT + 2 × `CC`, `0x37` on, 2 × sleep + HW reset, HW reset | **no refresh at all** | – |
| 18–20 | SWRESET, SWRESET, nothing | F F F | 0 |

- **Not a fixed bank:** consecutive full refreshes don't alternate (steps 1–3).
  **Confirmed.**
- **Rule: a full refresh shows the front buffer iff an even number of mode 2
  updates (`DC`, `D8`, `CC`) ran since the previous full refresh.** `D0`, `C0` and
  a LUT load don't count, and every full refresh starts the count again. Fits all
  of T76, T58–T63 and T67 (their prepare added one `D8`). **Likely**, T77 checks it.
  - Doesn't fit T68 (`0x37` written, see below) or T01 (maybe run with an older
    prepare, no record).
- **After `0x37` ping-pong off, full refreshes stop doing anything** (12–17), and
  `0x37` on, sleep + HW reset and a HW reset don't bring them back. SWRESET does
  (18). T67/T68 wrote `0x37` too but refreshed. **Open**, the driver never writes `0x37`.
- Still open: does sleep/HW reset or SWRESET between partials and a full change the
  count? (12–17 couldn't tell.) T77.

Raw answer: "FFFBFBFFFBFXXXXXXFFF, the X are where the screen does not refresh at all"

## Eighth run (T77): checking the full refresh rule

Same method as T76, up to three operations per step. Run twice, same result.

| Step | Before the full | Expected | Seen |
|---|---|---|---|
| 1 | nothing | F | F |
| 2 | `DC` | B | B |
| 3 | 3 × `DC` | B | B |
| 4 | `DC`, LUT, `CC` | F | F |
| 5 | `CC`, `D0` | B | **F** |
| 6 | `DC`, `C0` | B | B |
| 7 | 2 × `D8` | F | F |
| 8 | `DC`, sleep + HW reset | B | B |
| 9 | sleep + HW reset, `DC` | B | B |
| 10 | `DC`, HW reset | B | B |
| 11 | `DC`, SWRESET + init | ? | F |
| 12 | SWRESET + init, `DC` | B | B |
| 13 | nothing | F | F |
| 14, 15 | `0x37` on; nothing | ? | no refresh, no refresh |
| 16 | SWRESET + init | F | F |
| 17, 18 | `0x37` off; sleep + HW reset | ? | no refresh, no refresh |
| 19, 20 | SWRESET + init; nothing | F | F F |

- **Rule: a full refresh shows the front buffer iff an even number of mode 2
  updates (`DC`, `D8`, `CC`, OTP and custom mixed) ran since the last full
  refresh or SWRESET.** **Confirmed** (T76, T77, both runs identical).
- **Sleep + HW reset and a HW reset keep the count** (8–10): a partial in one wake
  and a full in the next behave as in one wake. **Confirmed.**
- **SWRESET resets the count** (11). **Likely.**
- **`C0` doesn't touch the count** (6). **Confirmed** with T76 step 8.
- **`CC` then `D0` showed the front** (5). Either `D0` (mode 1 + OTP LUT load, no
  display) resets the count like `D4`, or that `CC` didn't count because step 4's
  `D4` had replaced the custom LUT. **Open.** The driver never sends `D0` and loads
  the custom LUT on every switch to a custom mode, so it doesn't hit this.
- **Any `0x37` write (ping-pong on or off) stops full refreshes until SWRESET**
  (14, 17). T67/T68 (`0x37`, then a LUT load and partials) did refresh. **Open**,
  the driver never writes `0x37`.
- For the driver: in FULL mode, while the count is even, writing the frame to the
  front buffer only is enough, and afterwards the back buffer holds the glass as
  after a partial. With an odd count, a single bank isn't enough: write both.

Raw answer: "FBBFFBFBBBFBFXXFXXFF, tested again FBBFFBFBBBFBFXXFXXFF"

## Ninth run (T78, T79): the driver's way of sending updates

T78: `0x22` only written when it changes, so most full refreshes are a bare `0x20`,
as in the driver and the wake stub. T79: the same with `0x21 = 0x88`, as the watch.

Both: `FFFBFFFBFFBF`, exactly the T77 rule's prediction.

- **The rule holds for bare `0x20` refreshes and with both buffers inverted.**
  Writing `0x22` isn't what restarts the count, the full refresh is. **Confirmed.**
- So the leftovers seen with the first single-bank FULL driver (it assumed a full
  refresh swaps like a partial) come from
  something else in the driver's sequence. To be traced.
- Every letter faded after about 2 s during the 4 s hold (not reported for
  T76/T77). **Open.** The tests leave the analog on (`D4` has no power-off bits)
  until the next command; the driver hibernates right after BUSY.

Raw answer: "T78:FFFBFFFBFFBF, T79: FFFBFFFBFFBF, they all fade after 2 seconds, like
the driver is still active, but anyway i guess that is expected, unrelated to the tests"

## Tenth run (T80): a full refresh doesn't swap

Trace `build/captures/leftovers.bin` (the first single-bank FULL driver): the
first partial after full refreshes showed the old and new pictures ANDed (menu FULL
→ watchface FAST), and minute leftovers (watchface FULL → menu FAST).

| Step | Sequence | A (full swaps) | Z (full doesn't) | Seen |
|---|---|---|---|---|
| 1 | F front, B back, full | F | F | clean F |
| 2 | no writes, partial `DC` | B | F stays | **F stays** |
| 3 | SWRESET, F/B, full | F | F | F, fades |
| 4 | `D8`, N to front, partial | N + leftovers | clean N | N, some parts grey |
| 5 | F/B full, N to front, partial | clean N | N + leftovers | F, then a grey N |

- **A full refresh doesn't swap the buffers.** It shows one physical bank (the
  front one when an even number of partials ran since the last full refresh or
  SWRESET, T76–T79) and leaves `0x24` pointing at it: afterwards the **front
  buffer holds the glass and the back buffer is stale**. Only partial (mode 2)
  updates swap. **Confirmed** (step 2; T05's "D4 swaps" fits this too).
- So the first partial after a full refresh needs the glass in the back buffer.
  The old driver had it by writing both banks in FULL mode; the first single-bank
  driver broke it.
- **`D8` before writing the next frame** (a swap without driving the panel) should
  put the glass in the back buffer (step 4). The greys in steps 3–5 are likely the
  same fade as T78/T79: the test holds the picture with the analog on. T81 repeats
  T80 with the analog off during the holds.

Raw answer: "Clean F, F stays, F fades, N some parts ok some grey, F and then fades to N grey"

## Eleventh run (T81) and the driver fix

T81 = T80 with the analog off during the holds. Answer: step 1 clean F, step 2 F
stays (not so clean), step 3 clean F (the T78–T80 fade is gone: it came from the
analog staying on), steps 4 and 5 both a not very clean N.

- **No swap after a full refresh: confirmed again** (step 2).
- Steps 4/5 can't tell the D8 fix apart: the big letters overlap in the same place,
  and the partial greys same-colour pixels anyway. Tested on the real transitions
  instead.

The driver now: FULL mode writes the front buffer only (both banks after an odd
number of partials), skips the post-refresh re-writes after a full refresh, and
`setRefreshMode()` runs one `0xD8` update before the first partial after a full one
(`0x98` since T86, below).
Trace `build/captures/d8fix.bin`: menu FULL ↔ watchface FAST, watchface FULL with
stub minute updates ↔ menu FAST, both FAST. 55 refreshes, 7 `D8` swaps, **no stale
pixels in the viewer model, and no leftovers on the watch**. The `D8` adds about
100 ms of BUSY to the first partial after a mode switch from FULL.

Raw answers: "F clean, F stays not so clean but ok, clean F, not very clean N but
better than before, same not very clean N some parts are ok some are grey". Driver
test: "All good could not see any issue"

## Twelfth run (T82–T85): a cheaper swap than `D8`?

Right after a full refresh (as in the driver): front A black, back B black, probe,
`DC` readout. BUSY of the probe on screen.

| # | Probe | Readout | Result | BUSY |
|---|---|---|---|---|
| 82 | `08` mode 2 only | A blk, B wht | no swap | 0 ms |
| 83 | `88` mode 2 + clock | A blk, B wht | no swap | 0 ms |
| 84 | `C8` + analog, no LUT load | A blk, B wht | **no swap** | 92 ms |
| 85 | `D8` + LUT load (driver) | A wht, B blk | swap | 92 ms |

- **An update without the display bit swaps only with mode 2 *and* the LUT load
  (`D8`), not with mode 2 + analog (`C8`).** **Confirmed** (T85 vs T84, one bit apart).
  So the old driver's `C8` (switching to a custom LUT) never swapped.
- Without the clock (`08`, `88`) the update doesn't run at all (0 ms).
- `D8`'s 92 ms is the analog power-on (T48: `C0` takes the same). Open: does `98`
  (clock + LUT load + mode 2, no analog) swap, and how fast? T86.

Raw answer: "Black, white, 0ms busy, black, white, 0ms, black, white, 92ms white, black, 92ms"

## Thirteenth run (T86): `98` swaps in 1 ms

| # | Probe | Readout | Result | BUSY |
|---|---|---|---|---|
| 86 | `98` clock + LUT load + mode 2, no analog | A wht, B blk | **swap** | **1 ms** |

- **The no-display swap comes with the LUT load in mode 2, not with the analog.**
  `98` swaps like `D8` in 1 ms instead of 92 ms. **Confirmed.** The driver now uses
  `98` before the first partial after a full refresh.

Raw answer: "white, black, 1ms"

## Raw answers (as reported, 2026-11-01)

T01: BW. T02: between diff and all, because patch B is grey. T03: same as T02,
grey in patch B. T04: swap. T05: swap. T06: swap. T07: swap, but the text was
all grey. T08: swap. T09: swap. (In these last tests the patches changed 2
times: white, then black, then white again.) T10: whole screen. T11: whole copy.
T12: S, but not full white, more like very bright grey. T13: W. T14: K.
T15: all white, and text very faded like grey. T16: kept. T17: kept.
T18: white, but again very faded text. T19: works, all black (except border).
T20: screen inverted, black with white text. T21: kept. T22: kept, border black.
T23: works. T24: kept, A black. T25: A, first box has a vertical bar at Y,
second at X. T26: same as T25. T27: A TL, B TR, C BL, D BR; E has stripes at
the bottom, starting with black at BL, same on F but stripes inverted or start
at BR. T28: W has stripes, the rest of the box is black, no stripes elsewhere.
T29: stripes are now above box A, outside of it. T30: A has part of the bottom
black, B is white, except for a long 1 px line crossing A and B at the very
bottom. T31: a black area at the bottom exits the box to the left and wraps
around the screen to the C area; B fully white, C as well I think. T32: grey in
the box, not sure. T33: all black, only black, no text. T34: grey as well, not
sure. T35: border black. T36: white. T37: black. T38: white. T39: white.
T40: black. T41: black. T42: white. T43: white. T44: white. T45: both white;
box A looked ok while F was updating (the whole second part of the screen was
flashing), but then the A side faded a little into grey. T46: screen looks
mirrored in X and also rotated 180. T47: looks like a vsync line artifact
around 50 lines from the end of the screen. T48: difficult to see the text,
but A flipped a couple of times, then it shows a list of LUTs and timings.
