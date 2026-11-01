# SSD1681 display experiments

Debug firmware mode that replaces the watch app with a menu of controller
experiments (`firmware/main/display_test.cpp`). Each test leaves a picture on
the panel that answers one question the datasheet leaves open. Report results
as "T04: copy" or "T12: W", using the hypothesis names below.

Naming: the panel is black/white only, so the two RAMs are called the **front
buffer** (`0x24`, "BW RAM" in the SSD1681 datasheet: the new image) and the **back
buffer** (`0x26`, "RED RAM": the previous image, which is what's on the glass
after a refresh), as in the panel's B/W documentation and the driver.

## Running

1. `idf.py menuconfig` → **LightInk display tests** → enable
   `LIGHTINK_DISPLAY_TEST`. Optionally set *Test to run at first boot* (0 = menu).
   Or append to your local `sdkconfig`:
   ```
   CONFIG_LIGHTINK_DISPLAY_TEST=y
   CONFIG_LIGHTINK_DISPLAY_TEST_START=0
   ```
2. `idf.py build flash`. Keep `CONFIG_LIGHTINK_TRACE=y` and run
   `python tools/lightink_trace.py --port /dev/ttyUSB0 --reset` to record every
   command; the viewer's SSD1681 model then shows what it *predicts* for each
   test, which you can compare with the real panel.
3. On the watch:
   - **Menu:** UP/DOWN move, MENU runs the selected test.
   - **Result screen:** MENU runs it again, DOWN/UP run the next/previous test,
     BACK returns to the menu.
   - A trace script (`CONFIG_LIGHTINK_TRACE_SCRIPT`) can drive this too, e.g.
     `"M D D D"` runs tests 1–4 one after the other.
4. The UART log (`dtest` tag) prints each test, its hypotheses, and the BUSY
   time of every refresh.

Turn it off again before using the watch normally.

## What every test does first ("prepare")

HW reset → `0x12` SWRESET → `0x01 C7 00 00` → `0x1A 32 00` (fixed 50 °C, as the
driver) → `0x11 03` → `0x3C 05` → `0x21 00` (no inversion) → the base picture
written to **both** buffers (`0x24` and `0x26`) → full refresh `0x22 D4`, `0x20` →
then, like `Display::_setRefreshMode`, a `0x22 D8`, `0x20` update without the
display bit (skipped only in T03).

So before the experiment the glass, front and back all show the base picture: a
white screen with a header (`Txx name` + question), framed white boxes **A–F**
(A B C on the top row, D E F below), a caption next to each box letter, and the
legend at the bottom.

Box captions: `fr0 bk1` = front buffer written black (0) and back buffer white (1) inside
the box; `frH` / `bkV` = horizontal / vertical stripes. Small bars with a letter
next to box A (rows) or under box A (columns) are the **marks** some tests ask
you to read.

The tests rely on each other: T01 says which RAM a full refresh shows and T02
how a partial refresh decides which pixels to drive. Most later tests read RAM
through a partial refresh, assuming T02 = **diff**. If T01/T02 come out
differently, stop and report those first.

Notation: *partial* = `0x22 DC` (OTP LUT, mode 2), *full* = `0x22 D4` (OTP LUT,
mode 1). Unless said otherwise the window is reset to the whole screen before
every `0x20`.

Positions such as "top-left" assume rotation 2 (the default, `kSettings.mDisplay.mRotation`):
RAM x grows to the **left** and RAM y grows **up** as you look at the watch.

## Readout basics

| # | Question | Sequence after prepare | Hypotheses → picture |
|---|---|---|---|
| 01 | Which RAM does a full (mode 1) refresh show? | A: front black. B: back black. C: front h-stripes, back v-stripes. Full. | **front**: A black, B white, C horizontal stripes · **back**: A white, B black, C vertical stripes · **mix**: C shows a grid or something else |
| 02 | Does a partial (mode 2) refresh only drive pixels where back ≠ front? | A: front black. B: front+back black. C: back black. D: front h-stripes. Partial. | **diff**: A black, B white, C white, D stripes · **all** (behaves like full): A black, B black, C white, D stripes · **back** (roles swapped): A white, B black, C black, D white |
| 03 | Does a partial refresh need the `0xD8` "2-buffer" update after a full one? | Same as T02, but prepare skips the `0xD8` update. | **same as T02**: the driver's extra update is unnecessary · **different**: it is needed (describe) |

## RAM copy after an update (T04–T09)

Sequence: A = front black / back white, B = front white / back black. Update #1 with
the tested `0x22` value. If #1 has the display bit, front A := white again. Then
partial update #2 shows where back ≠ front.

| # | Update #1 | Question |
|---|---|---|
| 04 | `DC` partial | What happens to back/front after a mode 2 update? |
| 05 | `D4` full | …after a mode 1 update? |
| 06 | `D8` (no display bit) | …after an update that doesn't display? |
| 07 | `F7` full + load temp + analog/clock off | …with the classic full sequence? |
| 08 | `FF` (POR value) | …with the POR sequence? |
| 09 | `CC` with custom LUT `CUSTOM_2_1` loaded (no OTP LUT load) | Does it depend on the LUT? |

Hypotheses (same for all six):
- **copy** (back ← front): A white, B white
- **swap** (back ↔ front): A white, B black
- **none** (neither RAM changes): A black, B white

## Window scope

| # | Question | Sequence | Hypotheses |
|---|---|---|---|
| 10 | Is a refresh limited to the RAM window? | A, B: front black. Window = A, then partial (window not reset). | **window**: A black, B white · **whole**: A black, B black |
| 11 | Is the back ← front copy limited to the window? | A, B: front black. Window = A, partial #1. Then front A, B := white, partial #2 (full window). | **whole copy**: A white, B white · **window copy**: A white, B black · **no copy**: A black, B black. Only valid if T10 = whole. |

## Address counters

All in box A. The marks to its left tag line bands of the box.

| # | Question | Sequence | Hypotheses (read the marks) |
|---|---|---|---|
| 12 | Do `0x24` and `0x26` share the X/Y counter? | Window A, counter at line 12. `0x24` + 72 B black (lines 12–23). `0x26` + 72 B black, no counter set. Readout: front A all black, partial. Lines where back is black stay **white**. | **S**: white band at S (each RAM has its own counter, both set by `0x4E/0x4F`) · **N**: white band at N (one counter, keeps going) · **C**: white band at C (`0x26` restarts at the window start) |
| 13 | Is the counter kept after a `0x20` refresh? | Window A, `0x24` + 144 B black (lines 0–23), partial (window kept). `0x24` + 144 B v-stripes, no window/counter, partial. | **K**: W black, stripes at K (kept) · **W**: stripes at W, K white (reset to the window start) · **else**: stripes somewhere else (describe) |
| 14 | Are counter and window kept across `0x10 01` + HW reset? | As T13, but `0x10 01` + HW reset between the writes and no refresh between them. | **K**: W black, stripes at K · **W**: stripes at W (window kept, counter reset) · **P**: W black and stripes along the screen edge at RAM (0,0) = bottom edge (POR) |
| 15 | …across `0x12` SWRESET? | As T14 with `0x12`, then the init registers again (`0x01`, `0x1A`, `0x11`, `0x3C`, `0x21`; not window/counter). | **K/W**: stripes at the mark (window kept); W black only if RAM kept · **P**: stripes along the screen edge at RAM (0,0) (window and counter back to POR) |

## Retention across sleep and resets

| # | Question | Sequence | Hypotheses |
|---|---|---|---|
| 16 | Is RAM kept across `0x10 01` (deep sleep mode 1) + HW reset? | front A black, sleep, reset, partial. | **kept**: A black · **lost**: A white, or noise anywhere |
| 17 | …across `0x10 03` (deep sleep mode 2) + HW reset? | Same with `0x10 03`. | Same |
| 18 | …across `0x12` SWRESET? | Same with `0x12`, then the init registers again. | **kept**: A black, text crisp · **cleared white**: A white, text fades · else describe |
| 19 | Does `0x21 08` (front inverse) apply in mode 2? (control for T20) | `0x21 08`, partial. | **works**: the whole screen inverts · **ignored**: no change |
| 20 | Is `0x21` kept across sleep + HW reset? | `0x21 08`, sleep, reset, partial. | **kept**: screen inverts · **reset**: no change |
| 21 | Is `0x22` kept across sleep + HW reset? (the wake stub relies on it) | A: front+back black. `0x22 D4`, sleep, reset, only `0x20`. | **kept** (`D4` full): A black, full-screen flashing · **POR** (`FF`, mode 2): A stays white · **nothing happens** at all: `0x22` cleared |
| 22 | Is `0x3C` (border) kept across sleep + HW reset? | `0x3C 02`, sleep, reset, full. | **kept**: border black · **reset**: border not black. Read with T37 (`0x02` alone). |
| 23 | Does the custom LUT work at all? (control for T24) | Load `CUSTOM_2_1`, A front black, `0x22 CC`. | **works**: A black · **broken**: A white/noise |
| 24 | Is the custom LUT kept across sleep + HW reset? | As T23 with sleep + reset after loading. | **kept**: A black · **lost**: A white/noise |
| 25 | Does `0x11 07` (Y first) work? (control for T26) | `0x11 07`, window A, 48 B black; `0x11 03`, window B, 48 B black; partial. | **Y first works**: A shows a vertical bar at mark **Y** · **ignored**: horizontal bar at mark **X**. B is always a horizontal bar (X first). |
| 26 | Is `0x11` kept across sleep + HW reset? | `0x11 07`, sleep, reset, then as T25 without setting `0x11` for A. | **kept**: A bar at Y · **reset**: A bar at X |

## Window and entry modes

| # | Question | Sequence | Hypotheses |
|---|---|---|---|
| 27 | Where does writing start in each entry mode, and is the bit order mirrored? | A `0x11 00` (X−Y−), B `01` (X+Y−), C `02` (X−Y+), D `03` (X+Y+): window set so the start is the mode's first corner, write 8 lines of [3 B black, 3 B white]. E (`02`, X−) and F (`03`, X+): 16 lines of `0x0F` (4 px black, 4 px white). | Each black block (24×8) sits at the start corner. Expected with rotation 2: **A top-left, B top-right, C bottom-left, D bottom-right**. **E = F** (same stripe colour at the right edge): bits are not mirrored in X−; **E shifted by 4 px**: bits mirrored too. |
| 28 | What happens at the end of the window? | Window A, counter start, `0x24` + 288 B black (whole box) + 72 B v-stripes. | **W**: stripes at W (wraps to the window start) · **past**: stripes above box A, over its label (runs past the window end) · **stop**: box all black, no stripes |
| 29 | `0x45` with 3 parameters (as the driver sends): is the missing 4th one kept? | `0x45 y0 0 y1 1` (end high bit = 1), then `0x45 y0 0 y1`, then as T28. | **W**: stripes at W (4th parameter reset to 0) · **above A**: the old 4th parameter (1) was kept. Read with T28 = W. |
| 30 | Counter outside the window | Window A, counter at B's corner, 72 B black. | **B black**: counter used as is · **A black**: snapped into the window · **nothing**: write ignored · else describe |
| 31 | X start > X end in X+ mode | Window X from box A's start byte to box C's byte (start > end in RAM), rows of the top row, 144 B black. | Observational: describe where the black lands (A/B/C, full-width lines, nothing…) |

## Display update options

| # | Question | Sequence | Hypotheses |
|---|---|---|---|
| 32 | `0x21 80`: is back inverse applied in mode 2? | A: front black (back white), `0x21 80`, partial. | **works**: A white (back reads black = front, nothing driven) · **ignored**: A black |
| 33 | `0x21 04`: front "bypass as 0" | `0x21 04`, partial, no RAM change. | **as 0**: whole screen goes black · **ignored**: no change |
| 34 | `0x21 40`: back "bypass as 0" | A: front black, `0x21 40`, partial. | **works**: A white · **ignored**: A black |
| 35–44 | Border waveform values | `0x3C` = `00`, `01`, `02`, `03`, `05`, `40`, `50`, `60`, `80`, `C0`, then full. | Report the colour of the border around the active area (white / black / grey / unchanged / flicker) |

## Panel geometry

| # | Question | Sequence | Hypotheses |
|---|---|---|---|
| 45 | `0x01` MUX = 100 lines | A, F: front black. `0x01 63 00 00`, full. | Observational: half the screen updated? stretched? Which boxes show? |
| 46 | `0x01` TB bit (gate scan direction) | `0x01 C7 00 01`, full. | **mirrored**: picture flipped (header at the bottom) · **same**: bit ignored |
| 47 | `0x0F 32 00` gate scan start (SSD1675 command, undocumented here) | `0x0F 32 00`, full. | **shifted**: the picture moves by 50 lines (wraps) · **same**: no such command |

## Timing

| # | What | Output |
|---|---|---|
| 48 | BUSY time of `D4`, `DC`, `F7`, `FF`, custom `2_1`/`6_1` with `CC`, `C0` (analog on only), `03` (analog/clock off), `C0` again | The numbers are drawn under box A and printed in the UART log. Useful for picking the cheapest sequence per update. |

## Follow-ups after the first run (T49–T57)

The first run showed that every `0x20` swaps the buffers and that the same-colour
partial waveforms aren't no-ops ([display_findings.md](display_findings.md)).
T15 and T18 were changed: without `0x1A` after a SWRESET the OTP LUT is empty
(a 92 ms update that drives nothing), so their first results don't count.

| # | Question | Sequence after prepare | Hypotheses |
|---|---|---|---|
| 49 | Do the address counters swap with the RAM banks? | Window A, counters at line 0. `0x26` + 72 B **white** (moves only the back buffer's counter to line 12, content unchanged). Partial, window kept (swap). `0x24` + 72 B black, no counter set. Partial. | Black band at **R** (lines 0–11): counters reset by `0x20` · at **F** (lines 12–23): counters swap with the banks |
| 50 | Does deep sleep mode 2 keep RAM for a minute? | front A black, `0x10 03`, wait 60 s, HW reset, partial. | **kept**: A black · **lost**: A white or noise |
| 51 | Sleep current, mode 1 vs mode 2 (PPK2) | front A black, 20 s in `0x10 01`, HW reset, 20 s in `0x10 03`, HW reset, partial. The UART log says when each phase starts. | Read both currents. The display rail stays locked during tests, so compare the two phases, not absolute values. A black = RAM survived both. |
| 52 | Does X− mirror the bit order inside a byte? | E: `0x11 02` (X−), F: `0x11 03` (X+), 16 rows of `7F` in the first byte of each row (one black pixel, the MSB), rest white. | **F** (control): line at its right edge · **E** line at its left edge: X− mirrors bits · **E** line 7 px in from the left edge: bits not mirrored |
| 53 | What do the same-colour waveforms do (OTP partial, 1 update)? | Glass: B, C black (full). Then both buffers: A black, B white, C black, D white. One partial `DC`. | **A** (white glass, RAM BB) and **B** (black glass, RAM WW): white/grey/black? Unchanged = no-op, grey = they drive. C black, D white are controls. |
| 54 | …after 5 partial updates? | As T53 with 5 × `DC`. | Do A and B get darker/lighter than in T53? |
| 55 | …with the custom `2_1` LUT? | As T53, custom LUT loaded, one `CC`. | A white, B black: BB/WW are no-ops in the custom LUT · grey: they drive too |
| 56 | Does an analog-only update (`0x22 C0`) swap? | As T04–T09 with `C0`. | **copy** A wht B wht · **swap** A wht B blk · **none** A blk B wht |
| 57 | Does an analog/clock-off update (`0x22 03`) swap? | As T56 with `03`. | Same |

## Full refresh after partial ones (T58–T63)

The watch, leaving a menu drawn with FAST (or a custom LUT) for a FULL watchface,
sent the watchface into front and ran a real full refresh (1915 ms BUSY), but the glass
kept the menu until the next refresh. Hypothesis: a full refresh always shows the
same physical bank, while `0x24` alternates with every swap.

Each test: prepare, *n* partial refreshes that change nothing (each swaps), then front
A black and a full `0x22 D4`. **Updated** = A black · **not updated** = A white.

| # | Before the full refresh | Question |
|---|---|---|
| 58 | 0 partials | Control (T01 situation) |
| 59 | 1 partial `DC` | Odd number of swaps |
| 60 | 2 partials `DC` | Even number of swaps |
| 61 | 1 partial, then `0xD0` (no display) | What the driver sent before `5033876` |
| 62 | 1 partial with custom `2_1` (`CC`) | Does the LUT matter? |
| 63 | 1 partial, A black written to **both** `0x24` and `0x26` | Candidate fix: full refresh correct whichever bank it shows |

## RAM ping-pong and gate modes (T64–T70)

`0x37` (Display Option) byte F bit 6 is "RAM ping-pong for Display Mode 2" per
the SSD1680/1681 datasheets (POR: disabled). Waveshare enables it with
`0x37 00 00 00 00 00 40 00 00 00 00` for partial refreshes. The swap measured in
T04–T09 may be this option. The OTP LUT load (`0x22` bit `10`, in `DC`/`D4`) may
reload it, so the probes use both the OTP partial and the custom `2_1` LUT
(`CC`, no LUT load).

| # | Sequence after prepare | Hypotheses |
|---|---|---|
| 64 | `0x37` ping-pong **off**, then as T04 with `DC` probe + `DC` readout | **copy** A wht B wht · **swap** A wht B blk · **none** A blk B wht |
| 65 | Ping-pong **off**, custom `2_1`, `CC` probe + `CC` readout | Same |
| 66 | Ping-pong **on**, custom `2_1`, `CC` probe + `CC` readout (control) | Same; expected **swap** |
| 67 | Ping-pong off, custom `2_1`, 1 × `CC` (no change), front A black, full `D4` | **updated** A black · **not updated** A white |
| 68 | Same with 2 × `CC` | Same as T67 = the partials no longer swap |
| 69 | `0x01 C7 00 04` (GD: first gate G1), full | Describe: shifted one line, row pairs swapped, or no change |
| 70 | `0x01 C7 00 02` (SM: interlaced gate scan), full | Describe: rows interleaved, halves swapped, or no change |

## Refreshing a band of rows (T71–T75)

`0x01` MUX (rows − 1) and `0x0F` (gate scan start) limit a refresh to a band of
gate lines. Columns can't be limited: a refresh always drives the full width (T10).

Each test: prepare, boxes A, C, E black and B, D, F white (both buffers, full
refresh), then a centre square (RAM rows 88–119, x 80–119, between the box rows)
toggled black/white *n* times by writing it to the front buffer, ending black.

| # | LUT | Rows scanned | Toggles | Look at |
|---|---|---|---|---|
| 71 | OTP partial `DC` | all 200 | 5 | Baseline BUSY time (log) |
| 72 | OTP partial | 32 (`0x01 1F 00 00`, `0x0F 58 00`) | 5 | BUSY vs T71; is the square black and in place; boxes A–F unchanged? |
| 73 | OTP partial | 32 | 21 | Do boxes A–F (rows never scanned) fade or go grey; are the band's pixels beside the square grey; any edge lines? |
| 74 | Custom `2_1` `CC` | all 200 | 5 | Baseline BUSY time |
| 75 | Custom `2_1` | 32 | 21 | As T73, with the sustain LUT; BUSY vs T74 |

## Which bank a full refresh shows (T76)

T58–T68 didn't give a rule: OTP partials alternated the result, custom-LUT ones
didn't (T67/T68), and T01/T58 ran the same sequence with different results. Each
of those tests started from whatever the menu or the previous test left behind.

T76 runs 20 full refreshes (`D4`) in one go. Before each one it does one operation,
then writes a big **F** to the front buffer (`0x24`) and a big **B** to the back
(`0x26`). Each picture stays up for 4 s. Only the change from one step to the next
matters, so the starting state doesn't.

Hypothesis **fixed bank**: a full refresh always shows the same physical bank, so the
letter flips iff the number of swaps since the previous full refresh (counting that
refresh's own swap) is odd. At the end a summary screen shows the prediction for both
possible first letters. If every step matches one column, the rule holds and the
driver could write a single bank by counting swaps. Each mismatch shows which
operation breaks it.

| Step | Operation before the full refresh | Swaps if fixed bank | Letter |
|---|---|---|---|
| 1 | none (reference) | – | – |
| 2, 3 | none | 0 | flips |
| 4 | 1 × `DC` | 1 | same |
| 5 | 2 × `DC` | 2 | flips |
| 6 | `D8` | 1 | same |
| 7 | `D0` | 0 | flips |
| 8 | `C0` | 0 | flips |
| 9 | `0x32` custom LUT load, no update | 0 | flips |
| 10 | LUT + 1 × `CC` | 1 | same |
| 11 | LUT + 2 × `CC` | 2 | flips |
| 12 | `0x37` ping-pong off | 0 | flips |
| 13 | LUT + 2 × `CC`, ping-pong off (T68) | 2 | flips |
| 14 | `0x37` ping-pong on | 0 | flips |
| 15, 16 | `0x10 01` + HW reset | 0 | flips |
| 17 | HW reset, no sleep | 0 | flips |
| 18, 19 | SWRESET + init registers | 0 | flips (the same letter twice = SWRESET resets the bank) |
| 20 | none | 0 | flips |

Optional: set `CONFIG_LIGHTINK_DISPLAY_TEST_START=76` and power-cycle the watch
fully (battery off). Step 1's letter then gives the state at power-on, which the
driver would need as its starting point.

## Rule check (T77)

T76 gave a rule: **a full refresh shows the front buffer iff an even number of mode
2 updates (`DC`, `D8`, `CC`) ran since the previous full refresh** (findings,
seventh run). T77 runs the same way as T76 (F in front, B in back, full `D4`,
summary at the end) with up to three operations per step, and its summary lists
what the rule predicts. `?` = the rule says nothing.

| Step | Operations before the full refresh | Expected |
|---|---|---|
| 1 | none (reference) | F |
| 2 | `DC` | B |
| 3 | 3 × `DC` | B |
| 4 | `DC`, LUT, `CC` (OTP and custom partials mixed) | F |
| 5 | `CC`, `D0` | B |
| 6 | `DC`, `C0` | B |
| 7 | 2 × `D8` | F |
| 8 | `DC`, sleep + HW reset | B (the driver's case: partial, wake, full) |
| 9 | sleep + HW reset, `DC` | B |
| 10 | `DC`, HW reset | B |
| 11 | `DC`, SWRESET + init | ? B = count kept, F = SWRESET resets it |
| 12 | SWRESET + init, `DC` | B |
| 13 | none | F |
| 14, 15 | `0x37` ping-pong on; none | ? does `0x37` on alone stop full refreshes? |
| 16 | SWRESET + init | F |
| 17, 18 | `0x37` ping-pong off; sleep + HW reset | ? (T76 steps 12–17 didn't refresh) |
| 19 | SWRESET + init | F |
| 20 | none | F |

## Driver-style sequences (T78, T79)

With the first driver writing one bank in FULL mode, the watch showed
leftovers: menu FULL → watchface FAST drew the watchface over the menu, and
watchface FULL → menu FAST left bits of the minute updates. T76/T77 always sent
`0x22` right before each `0x20`; the driver sends `0x22` only when the mode
changes, so its full refreshes (and every wake stub update) are a bare `0x20`. The
watch also runs `0x21 = 0x88` (both buffers inverted), the tests `0x00`.

T78 runs the same F/B method with `0x22` only written when it changes. T79 repeats it
with `0x21 = 0x88` (pictures written inverted, so F still means the front buffer).
Expected = the T77 rule.

| Step | Before the full refresh | Expected |
|---|---|---|
| 1 | none (reference, `0x22 D4` sent) | F |
| 2, 3 | none: bare `0x20` | F F |
| 4 | `DC` (so `0x22` sent twice: DC, then D4) | B |
| 5 | none | F |
| 6 | 2 × `DC` | F |
| 7 | none | F |
| 8 | `DC`, sleep + HW reset | B |
| 9 | sleep + HW reset | F |
| 10 | none | F |
| 11 | `DC` | B |
| 12 | none | F |

If the bare `0x20` steps alternate (F B F …), writing `0x22` (not the full refresh
itself) restarts the count.

## What a full refresh leaves in the RAM (T80)

Trace `build/captures/leftovers.bin` (the first single-bank FULL driver): menu FULL
→ watchface FAST gave the watchface ANDed with the menu on the first partial after
the full refreshes (#11 → #12). Two models fit T76–T79:

- **A**: a full refresh shows the front buffer and swaps like a partial, so the back
  buffer then holds the glass (what that driver assumed).
- **Z**: a full refresh shows one fixed physical bank and points `0x24` at it, no
  swap: the front buffer holds the glass, the back buffer is stale.

F = front, B = back, N = a new frame written to the front only. 5 s per step.

| Step | Sequence | A | Z |
|---|---|---|---|
| 1 | F front, B back, full `D4` | F | F |
| 2 | no RAM writes, partial `DC` | B | F stays |
| 3 | SWRESET, F front, B back, full | F | F |
| 4 | `D8`, N to front, partial | N + leftovers | clean N |
| 5 | SWRESET, F/B full, N to front, partial (the driver's way) | clean N | N + leftovers |

T81 repeats T80 with the analog switched off (`0x22 03`, no swap, T57) during
each 5 s hold, as the driver hibernates right after BUSY: the T78–T80 pictures
faded while the analog stayed on.

## A cheaper swap than `D8` (T82–T85)

The driver runs `0x22 D8` before the first partial after a full refresh, only for its
swap (T80). It costs ~92 ms of BUSY, almost all of it the analog power-on (T48: `C0`
alone takes as long). The swap seems tied to the mode 2 bit (`08`), not the analog.

Each test: prepare (full `D4`, no `D8`, like the driver), front A black, back B black,
the probe update, then the T04 readout (`DC`). The probe's BUSY time is written on
screen above the readout.

| # | Probe `0x22` | Bits | Readout |
|---|---|---|---|
| 82 | `08` | mode 2 only, no clock | **swap** A wht B blk · **none** A blk B wht · **copy** A wht B wht |
| 83 | `88` | mode 2 + clock | same |
| 84 | `C8` | mode 2 + clock + analog, no LUT load (the old driver's custom-LUT switch) | same |
| 85 | `D8` | the driver's (control, expected swap, ~92 ms) | same |
| 86 | `98` | mode 2 + clock + LUT load, no analog (after T84: only `D8` swapped) | same |

## What can't be tested this way

- **Reading RAM directly:** MISO isn't wired, so the panel and the BUSY timing
  are the only readouts. Every RAM fact comes through a refresh, hence the
  dependency on T01/T02.
- **Analog behaviour** (ghosting, contrast, LUT voltages, how long particles
  take to settle) needs eyes over time and is outside these yes/no tests.
- **Power:** use the PPK2 with the normal firmware; this mode keeps the
  display rail locked during tests.
- **What exactly a HW reset clears:** T14, T16, T20–T22, T24 and T26 each probe
  one register or memory, but registers not covered here (booster `0x0C`,
  `0x2C` VCOM without a custom LUT, etc.) aren't checked.
