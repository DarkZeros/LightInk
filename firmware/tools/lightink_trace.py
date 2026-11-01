#!/usr/bin/env python3
"""LightInk trace viewer.

Decodes the binary trace the firmware streams on UART0 when built with
CONFIG_LIGHTINK_TRACE (idf.py menuconfig -> "LightInk debug trace"), replays
every SPI byte into an SSD1681 controller model and shows what the panel shows,
the controller RAM/registers, the system state (power locks and rail, light,
WiFi, BT, vibrator/speaker, battery), a timeline of each wake, and every
wake / command / button / power / WiFi / log event.

Firmware side (main/trace.cpp) mirrors all display SPI traffic, both from the
app and from the deep-sleep wake stub, Power::lock/unlock, peripherals, WiFi
events and a status snapshot before each sleep, plus scripted touches
(CONFIG_LIGHTINK_TRACE_SCRIPT, e.g. "M D D M . B B").
Events carry the device time: us since app start, or since chip wake in the stub.

Usage:
  # Live GUI (close idf.py monitor first, this tool owns the port). Unplugging is
  # fine: it waits and reconnects to the same USB device, even under a new name.
  python tools/lightink_trace.py --port /dev/ttyUSB0 [--reset]

  # Replay a capture saved from the GUI or with --record
  python tools/lightink_trace.py --replay capture.bin

  # Headless: dump each refresh as PNG / compare against golden PNGs
  python tools/lightink_trace.py --replay capture.bin --headless --frames out/
  python tools/lightink_trace.py --port /dev/ttyUSB0 --reset --headless \\
      --until-script-end --timeout 120 --compare golden/

GUI keys: Left/Right step one event, PgUp/PgDn previous/next refresh,
Up/Down (in the list) browse, Home/End first/last, F toggles follow-live.

Only needs the Python stdlib (tkinter) + pyserial for live ports.
The trace uses the console UART at its baudrate (115200 by default); on the
ESP32-S3 board that is UART0 (GPIO43/44), not the built-in USB-JTAG port.
"""

import argparse
import base64
import dataclasses
import bisect
import os
import queue
import re
import struct
import sys
import threading
import time
import zlib
from dataclasses import dataclass, field

W = H = 200
ROW = W // 8
PLANE = ROW * H
FULL = (1 << (PLANE * 8)) - 1

# ---------------------------------------------------------------------------
# Wire format (see main/trace.cpp)
# 00 A5 5A | type | len u16 LE | payload | fletcher16 over type..payload
# ---------------------------------------------------------------------------

MAGIC = b"\x00\xa5\x5a"
T_BOOT, T_STUB, T_SPI, T_BUTTON, T_SLEEP, T_SCRIPT_END = 1, 2, 3, 4, 5, 6
T_SPI_CMD, T_POWER, T_PERIPH, T_WIFI = 7, 8, 9, 10
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

APP_CAUSES = ["UNDEFINED", "ALL", "EXT0", "EXT1", "TIMER", "TOUCHPAD", "ULP", "GPIO",
              "UART", "UART1", "UART2", "WIFI", "COCPU", "COCPU_TRAP", "BT", "VAD", "VBAT"]
STUB_CAUSE_BITS = ["EXT0", "EXT1", "GPIO", "TIMER", "SDIO", "MAC", "UART0", "UART1",
                   "TOUCH", "ULP", "BT"]
BUTTONS = [(4, "MENU"), (2, "UP"), (1, "DOWN"), (8, "BACK")]


POWER_FLAGS = [(0x01, "Display"), (0x02, "Light"), (0x04, "Speaker"), (0x08, "GPS"),
               (0x10, "WiFi"), (0x20, "BT"), (0x40, "LoRa")]
WIFI_MODES = {0: "NULL", 1: "STA", 2: "AP", 3: "AP+STA", 0xFF: "not initialised"}
BT_STATUS = {0: "idle", 1: "initialised", 2: "ENABLED", 0xFF: "n/a"}
WIFI_REASONS = {2: "AUTH_EXPIRE", 3: "AUTH_LEAVE", 4: "ASSOC_EXPIRE", 8: "ASSOC_LEAVE",
                15: "4WAY_HANDSHAKE_TIMEOUT", 200: "BEACON_TIMEOUT", 201: "NO_AP_FOUND",
                202: "AUTH_FAIL", 203: "ASSOC_FAIL", 204: "HANDSHAKE_TIMEOUT", 205: "CONNECTION_FAIL"}


def flags_name(m):
    return "+".join(n for b, n in POWER_FLAGS if m & b) or "none"


def u32(p):
    return int.from_bytes(p[:4], "little")


def ms(us):
    return f"{us / 1000:.1f} ms"


def button_name(b):
    names = [n for bit, n in BUTTONS if b & bit]
    return "+".join(names) if names else "NONE"


def stub_cause_name(c):
    names = [n for i, n in enumerate(STUB_CAUSE_BITS) if c >> i & 1]
    return "|".join(names) if names else f"0x{c:x}"


def fletcher(body):
    s1 = s2 = 0
    for b in body:
        s1 = (s1 + b) % 255
        s2 = (s2 + s1) % 255
    return s1, s2


class FrameParser:
    """Splits the UART byte stream into binary frames and text log lines."""

    def __init__(self):
        self.buf = bytearray()
        self.text = bytearray()

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                keep = next((k for k in (2, 1) if self.buf.endswith(MAGIC[:k])), 0)
                self._text(self.buf[:len(self.buf) - keep], out)
                del self.buf[:len(self.buf) - keep]
                break
            if i:
                self._text(self.buf[:i], out)
                del self.buf[:i]
            if len(self.buf) < 6:
                break
            typ, ln = self.buf[3], self.buf[4] | self.buf[5] << 8
            if ln > 16384 or typ > 16:
                out.append(("bad", f"bad header type={typ} len={ln}"))
                del self.buf[:1]
                continue
            if len(self.buf) < 8 + ln:
                break
            body = bytes(self.buf[3:6 + ln])
            if fletcher(body) != (self.buf[6 + ln], self.buf[7 + ln]):
                out.append(("bad", f"checksum error type={typ} len={ln}"))
                del self.buf[:1]
                continue
            out.append(("frame", typ, body[3:]))
            del self.buf[:8 + ln]
        return out

    def _text(self, data, out):
        self.text += data.replace(b"\x00", b"")
        while (n := self.text.find(b"\n")) >= 0:
            line = self.text[:n].decode("utf-8", "replace")
            del self.text[:n + 1]
            line = ANSI.sub("", line).replace("\r", "").strip()
            line = "".join(c if c.isprintable() else "?" for c in line)
            if line:
                out.append(("text", line))


# ---------------------------------------------------------------------------
# SSD1681 model
# ---------------------------------------------------------------------------

CMD_NAMES = {
    0x01: "DRIVER_OUTPUT", 0x03: "GATE_VOLTAGE", 0x04: "SOURCE_VOLTAGE",
    0x0C: "BOOSTER_SOFT_START", 0x10: "DEEP_SLEEP", 0x11: "DATA_ENTRY_MODE",
    0x12: "SW_RESET", 0x18: "TEMP_SENSOR", 0x1A: "TEMP_WRITE", 0x20: "MASTER_ACTIVATION",
    0x21: "UPDATE_CTRL1", 0x22: "UPDATE_CTRL2", 0x24: "WRITE_FRONT", 0x26: "WRITE_BACK",
    0x2C: "VCOM", 0x32: "WRITE_LUT", 0x3C: "BORDER_WAVEFORM", 0x3F: "LUT_END_OPTION",
    0x44: "RAM_X_RANGE", 0x45: "RAM_Y_RANGE", 0x4E: "RAM_X_COUNTER", 0x4F: "RAM_Y_COUNTER",
}
RAM_OPTS = {0x0: "normal", 0x4: "bypass(0)", 0x8: "inverse"}


def cmd_name(c):
    return "DATA" if c is None else CMD_NAMES.get(c, f"CMD_{c:02X}")


def decode_upd2(u):
    flags = [n for bit, n in ((0x80, "clk on"), (0x40, "analog on"), (0x20, "load temp"),
                              (0x10, "load OTP LUT"), (0x04, "display"),
                              (0x02, "analog off"), (0x01, "clk off")) if u & bit]
    if u & 0x04:
        flags.append("mode 2 (partial)" if u & 0x08 else "mode 1 (full)")
    return ", ".join(flags) or "nothing"


def to_int(plane):
    return int.from_bytes(plane, "big")


def to_bytes(x):
    return x.to_bytes(PLANE, "big")


# Weak push on pixels whose back and front buffers agree, per partial refresh, in glass grey levels
# (0 white .. 255 black): (BB step towards black, cap, WW step towards white, floor).
# Qualitative fit of docs/display_findings.md T02, T53-T55: the OTP partial LUT greys BB
# on white after 1 refresh and it stays grey (T54), WW on black only greys after ~5;
# custom LUTs with a sustain phase (CUSTOM(N, 1)) push both at once, BB darker (T55).
WEAK_OTP = (96, 176, 24, 80)
WEAK_SUSTAIN = (160, 192, 128, 64)
LEVEL8 = [bytes(0 if v >> (7 - i) & 1 else 255 for i in range(8)) for v in range(256)]


class Ssd1681:
    """The controller as measured on the real panel (docs/display_findings.md).

    - Two RAM banks, each with its own X/Y counter. 0x24 writes bank k, 0x26 bank
      1-k, 0x4E/0x4F set both counters, a write advances only its own.
    - Every mode 2 update with display (DC, CC) flips k: banks and counters swap,
      whole screen. Without the display bit only D8 (mode 2 + LUT load) swaps: C8
      (mode 2 + analog, no LUT load) doesn't (T84/T85), and without the clock (08,
      88) nothing runs (T82/T83). Analog/clock-only updates don't swap (T56/T57).
    - A full (mode 1) refresh shows one physical bank and doesn't swap (T80): the
      front buffer when an even number of mode 2 updates ran since the last full
      refresh or SWRESET, else the back one, and then 0x24 points at the bank shown
      (T76-T80). So after a full refresh the front buffer holds the glass and the
      back buffer is stale: the next partial needs a D8 or both banks written first.
      Sleep/HW reset keep the count. A mode 1 LUT load without display (D0) resets
      it like a full refresh (T77 step 5): modelled, still open.
    - Any 0x37 write stops full refreshes until SWRESET (T76/T77): modelled as a
      full that drives nothing (whether it still swaps is unknown, it does here).
    - Partial (mode 2) drives back != front pixels to front and pushes back == front pixels
      weakly (grey), see WEAK_*.
    - HW reset keeps everything. 0x12 clears both banks to white and the registers;
      without 0x1A afterwards the OTP LUT is empty and refreshes drive nothing.
    """

    def __init__(self):
        self.banks = [bytearray(b"\xff" * PLANE), bytearray(b"\xff" * PLANE)]
        self.ctr = [[0, 0], [0, 0]]  # (x byte, y line) per bank
        self.k = 0
        self.odd = False  # Odd number of mode 2 updates since the last full / SWRESET
        self.no_full = False  # 0x37 written since SWRESET: full refreshes do nothing
        self.level = bytearray(W * H)  # Glass, 0 white .. 255 black
        self.panel = bytes(b"\xff" * PLANE)  # Glass thresholded, bit 1 = white
        self.changed = bytes(PLANE)
        self.stale = bytes(PLANE)
        self.border_color = "white"
        self.sleeping = False
        self.refreshes = 0
        self.lut = "OTP"
        self.lut_sustain = False
        self.temp_set = False
        self.last_refresh = "none yet"
        self.last_write = None  # (plane, x0byte, y0, x1byte, y1)
        self.por()

    def por(self):
        self.entry = 0x03
        self.p44 = [0x00, ROW - 1]
        self.p45 = [0, 0, H - 1, 0]
        self.p4f = [0, 0]
        self.ctr = [[0, 0], [0, 0]]
        self.upd1 = [0x00, 0x00]
        self.upd2 = 0xFF
        self.border = 0xC0

    def copy(self):
        c = Ssd1681.__new__(Ssd1681)
        c.__dict__ = dict(self.__dict__)
        c.banks = [bytearray(b) for b in self.banks]
        c.ctr = [list(x) for x in self.ctr]
        c.level = bytearray(self.level)
        return c

    # Registers
    @property
    def front(self):
        return self.banks[self.k]

    @property
    def back(self):
        return self.banks[1 - self.k]

    @property
    def xc(self):
        return self.ctr[self.k][0]

    @property
    def yc(self):
        return self.ctr[self.k][1]

    @property
    def window(self):
        return (self.p44[0] & 0x3F, self.p45[0] | (self.p45[1] & 1) << 8,
                self.p44[1] & 0x3F, self.p45[2] | (self.p45[3] & 1) << 8)

    @staticmethod
    def _merge(regs, data):
        regs = list(regs)
        regs[:min(len(data), len(regs))] = data[:len(regs)]
        return regs

    def _advance(self, c):
        """Counter c = [x, y]: wraps to the window start only when equal to the end."""
        xs, ys, xe, ye = self.window
        dx = 1 if self.entry & 1 else -1
        dy = 1 if self.entry & 2 else -1

        def step_x():
            if c[0] == xe:
                c[0] = xs
                return True
            c[0] = (c[0] + dx) & 0x3F
            return False

        def step_y():
            if c[1] == ye:
                c[1] = ys
                return True
            c[1] = (c[1] + dy) & 0x1FF
            return False

        if self.entry & 4:  # Y first
            if step_y():
                step_x()
        elif step_x():
            step_y()

    def hw_reset(self):
        # Every wake pulses RES (not traced): leaves deep sleep, everything else kept (T14-T26)
        self.sleeping = False

    def apply(self, cmd, data):
        """Executes one command with its parameters, returns a description."""
        if cmd is None:
            return f"{len(data)} B without a command"
        d = data
        if cmd in (0x24, 0x26):
            bank = self.k if cmd == 0x24 else 1 - self.k
            plane, c = self.banks[bank], self.ctr[bank]
            x0, y0 = c
            mirror = not self.entry & 1  # X- mirrors the bits too (T52)
            minx, miny, maxx, maxy = 99, 999, -1, -1
            for b in d:
                x, y = c
                if x < ROW and y < H:
                    plane[y * ROW + x] = int(f"{b:08b}"[::-1], 2) if mirror else b
                    minx, maxx = min(minx, x), max(maxx, x)
                    miny, maxy = min(miny, y), max(maxy, y)
                self._advance(c)
            if maxx < 0:
                self.last_write = None
                return f"{len(d)} B outside RAM (bank {bank})"
            self.last_write = ("front" if cmd == 0x24 else "back", minx, miny, maxx, maxy)
            return (f"{len(d)} B to bank {bank} from x={x0 * 8},y={y0}: "
                    f"x {minx * 8}..{maxx * 8 + 7}  y {miny}..{maxy}")
        if cmd == 0x20:
            return self._update()
        if cmd == 0x10:
            self.sleeping = bool(d and d[0] & 3)
            mode = d[0] & 3 if d else 0
            return f"mode {mode}" + {1: " (RAM retained)", 3: " (state lost within a minute, T50)"}.get(mode, "")
        if cmd == 0x12:
            self.por()
            self.banks = [bytearray(b"\xff" * PLANE), bytearray(b"\xff" * PLANE)]
            self.sleeping = False
            self.temp_set = False
            self.lut, self.lut_sustain = "OTP", False
            self.odd = self.no_full = False
            return "registers to POR, both RAM banks cleared to white (T18), parity reset (T77)"
        if cmd == 0x1A:
            self.temp_set = True
            t = (d[0] if d else 0) + ((d[1] >> 4) / 16 if len(d) > 1 else 0)
            return f"temperature {t:g} C"
        if cmd == 0x11 and d:
            self.entry = d[0] & 7
            return (f"0b{self.entry:03b}: X{'+' if self.entry & 1 else '-'} "
                    f"Y{'+' if self.entry & 2 else '-'}, {'Y' if self.entry & 4 else 'X'} first")
        if cmd == 0x44:
            self.p44 = self._merge(self.p44, d)
            xs, _, xe, _ = self.window
            return f"bytes {xs}..{xe} (px {xs * 8}..{xe * 8 + 7})"
        if cmd == 0x45:
            self.p45 = self._merge(self.p45, d)
            _, ys, _, ye = self.window
            return f"lines {ys}..{ye}"
        if cmd == 0x4E and d:
            for c in self.ctr:
                c[0] = d[0] & 0x3F
            return f"x byte {d[0] & 0x3F} (px {(d[0] & 0x3F) * 8}), both banks"
        if cmd == 0x4F:
            self.p4f = self._merge(self.p4f, d)
            y = self.p4f[0] | (self.p4f[1] & 1) << 8
            for c in self.ctr:
                c[1] = y
            return f"line {y}, both banks"
        if cmd == 0x21:
            self.upd1 = self._merge(self.upd1, d)
            return f"front {RAM_OPTS.get(self.upd1[0] & 0xF, '?')}, back {RAM_OPTS.get(self.upd1[0] >> 4, '?')}"
        if cmd == 0x22 and d:
            self.upd2 = d[0]
            return f"0x{d[0]:02X}: {decode_upd2(d[0])}"
        if cmd == 0x3C and d:
            self.border = d[0]
            return f"0x{d[0]:02X} -> {self._border_color()}"
        if cmd == 0x37:
            self.no_full = True
            return (" ".join(f"{b:02X}" for b in d[:16])
                    + "  ·  ⚠ full refreshes now do nothing until SWRESET (T76/T77)")
        if cmd == 0x32:
            # lut.h LUT::get(): bytes 0-11 = BB per group, 36-47 = WW
            self.lut = f"custom ({len(d)} B)"
            self.lut_sustain = len(d) >= 48 and (any(d[0:12]) or any(d[36:48]))
            return f"{len(d)} B custom waveform" + (", BB/WW sustain phase" if self.lut_sustain else ", BB/WW no-op")
        return " ".join(f"{b:02X}" for b in d[:16]) + (" ..." if len(d) > 16 else "")

    def _border_color(self):
        """Measured, T35-T44: GS transition bit 0 = white; fix level VSS/VSH1 black,
        VSL white; VCOM / HiZ leave the border as it was."""
        v = self.border
        if v >> 6 == 0:
            return "white" if v & 1 else "black"
        if v >> 6 == 1:
            return ["black", "black", "white", "unchanged"][v >> 4 & 3]
        return "unchanged"

    def _eff(self, plane, opt):
        if opt == 0x4:
            return 0
        x = to_int(plane)
        return x ^ FULL if opt == 0x8 else x

    def pending(self):
        """What a refresh now would do: (partial, strongly driven pixels, target image)
        as ints. Partial drives back != front strongly (the rest only weakly), full all."""
        partial = bool(self.upd2 & 0x08)
        front = self._eff(self.front, self.upd1[0] & 0xF)
        back = self._eff(self.back, self.upd1[0] >> 4)
        if partial:
            return partial, back ^ front, front
        # A full shows the back buffer after an odd number of partials (T76/T77)
        return partial, FULL, back if self.odd else front

    def _drive(self, partial, front, back):
        """Moves the glass levels. Returns how many pixels got only a weak push."""
        lv = self.level
        frontb, backb = to_bytes(front), to_bytes(back)
        bb, bb_cap, ww, ww_floor = (WEAK_OTP if self.lut == "OTP" else
                                    WEAK_SUSTAIN if self.lut_sustain else (0, 255, 0, 0))
        weak = 0
        for i in range(PLANE):
            nb = frontb[i]
            p = i * 8
            if not partial:
                lv[p:p + 8] = LEVEL8[nb]
                continue
            diff = nb ^ backb[i]
            if diff == 0xFF:
                lv[p:p + 8] = LEVEL8[nb]
                continue
            for bit in range(8):
                m = 0x80 >> bit
                if diff & m:
                    lv[p + bit] = 0 if nb & m else 255
                elif nb & m:
                    if lv[p + bit] > ww_floor and ww:
                        lv[p + bit] = max(ww_floor, lv[p + bit] - ww)
                        weak += 1
                elif lv[p + bit] < bb_cap and bb:
                    lv[p + bit] = min(bb_cap, lv[p + bit] + bb)
                    weak += 1
        return weak

    def _update(self):
        u = self.upd2
        swaps = bool(u & 0x0C)
        if not u & 0x04:
            swaps = (u & 0x98) == 0x98  # Mode 2 + LUT load + clock: D8, 98 (T84-T86)
            if swaps:
                self.k ^= 1
                self.odd = not self.odd
            elif u & 0x10 and self.odd:
                # D0 (T77 step 5, open): 0x24 back on the bank a full refresh shows
                swaps, self.odd = True, False
                self.k ^= 1
            return (f"0x22=0x{u:02X} without display bit ({decode_upd2(u)})"
                    + ("  ·  banks swapped" if swaps else "  ·  no swap")
                    + f"  ·  full would show {'back' if self.odd else 'front'}")
        if u & 0x10:
            self.lut, self.lut_sustain = "OTP", False
        partial, _, front = self.pending()  # Full: the buffer it shows
        back = self._eff(self.back, self.upd1[0] >> 4)
        shown_back = not partial and self.odd
        blocked = not partial and self.no_full
        empty = (self.lut == "OTP" and not self.temp_set and not u & 0x20) or blocked
        prev = self.panel
        weak = 0 if empty else self._drive(partial, front, back)
        self.odd = not self.odd if partial else False
        # 1-bit glass: darker than mid grey = black
        lv = self.level
        new = bytearray(PLANE)
        stale = bytearray(PLANE)
        frontb = to_bytes(front)
        for i in range(PLANE):
            p = i * 8
            byte = st = 0
            for bit in range(8):
                v = lv[p + bit]
                white = v < 128
                byte = byte << 1 | white
                target_white = frontb[i] >> (7 - bit) & 1
                st = st << 1 | (v >= 64 if target_white else v < 192)
            new[i], stale[i] = byte, st
        self.panel, self.stale = bytes(new), bytes(stale)
        changed = to_int(prev) ^ to_int(self.panel)
        self.changed = to_bytes(changed)
        bc = self._border_color()
        if bc in ("black", "white"):
            self.border_color = bc
        if partial or shown_back:
            self.k ^= 1  # Partial: swap. Full: 0x24 ends on the bank shown (T80)
        self.refreshes += 1
        n_stale = to_int(self.stale).bit_count()
        self.last_refresh = (f"#{self.refreshes} {'partial' if partial else 'full'}, "
                             f"LUT {self.lut}, {changed.bit_count()} px changed"
                             + (f", {weak} px weakly pushed" if weak else "")
                             + (f", {n_stale} px STALE/grey" if n_stale else "")
                             + ("  ·  ⚠ 0x37 written since SWRESET: full refresh drives nothing"
                                if blocked else
                                "  ·  ⚠ temperature not set since SWRESET: OTP LUT empty, "
                                "nothing driven" if empty else "")
                             + ("  ·  showed the BACK buffer (odd number of partials since the "
                                "last full, T76/T77)" if shown_back else ""))
        return "REFRESH " + self.last_refresh

    def registers(self):
        xs, ys, xe, ye = self.window
        (bx, by), (rx, ry) = self.ctr[self.k], self.ctr[1 - self.k]
        return "\n".join([
            f"state     {'deep sleep' if self.sleeping else 'awake'}",
            f"banks     0x24 -> bank {self.k}, 0x26 -> bank {1 - self.k}",
            f"full      shows {'BACK (odd partials since last full)' if self.odd else 'front'}"
            + ("  ·  blocked by 0x37" if self.no_full else ""),
            f"counter   front x {bx * 8} y {by}  ·  back x {rx * 8} y {ry}",
            f"entry     0b{self.entry:03b}  X{'+' if self.entry & 1 else '-'} Y{'+' if self.entry & 2 else '-'} "
            f"{'Y' if self.entry & 4 else 'X'} first",
            f"window    x {xs * 8}..{xe * 8 + 7}  y {ys}..{ye}",
            f"0x21      front {RAM_OPTS.get(self.upd1[0] & 0xF, '?')}, back {RAM_OPTS.get(self.upd1[0] >> 4, '?')}",
            f"0x22      0x{self.upd2:02X} {decode_upd2(self.upd2)}",
            f"border    0x{self.border:02X} {self._border_color()}",
            f"LUT       {self.lut}" + (" (sustain)" if self.lut_sustain else "")
            + ("" if self.temp_set else "  ·  temperature NOT set"),
            f"refresh   {self.last_refresh}",
        ])


# ---------------------------------------------------------------------------
# Timeline: decoded events + controller state at any point
# ---------------------------------------------------------------------------

@dataclass
class Event:
    idx: int
    kind: str  # wake stub cmd button sleep script_end log bad
    t: float
    group: int
    cmd: int | None = None
    data: bytearray = field(default_factory=bytearray)
    title: str = ""
    detail: str = ""
    closed: bool = True
    frame: int | None = None  # refresh frame number
    dt: int | None = None  # device us, when the frame carried it
    at: int | None = None  # dt, or the last known one in this wake
    info: dict = field(default_factory=dict)


@dataclass
class SystemState:
    """Everything but the display, as known at some event."""
    mask: int = 0  # Power lock flags, survive deep sleep (RTC)
    rail: bool = False
    wifi: str = "off"
    ntp: bool = False
    vibrator: tuple | None = None  # (at us, pattern ms)
    speaker: tuple | None = None
    battery: int | None = None
    sleep_wifi: int | None = None
    sleep_bt: int | None = None
    warnings: tuple = ()

    def copy(self):
        return dataclasses.replace(self)

    def lines(self, at=None):
        def active(p):
            return p and at is not None and p[0] <= at <= p[0] + sum(p[1]) * 1000
        return "\n".join([
            f"power     locks {flags_name(self.mask)}",
            f"rail      {'HIGH' if self.rail else 'low'}",
            f"light     {'ON' if self.mask & 0x02 else 'off'}",
            f"wifi      {self.wifi}" + ("  ·  NTP synced" if self.ntp else ""),
            f"bt        {'powered' if self.mask & 0x20 else 'off'}"
            + (f"  ·  controller {BT_STATUS.get(self.sleep_bt, self.sleep_bt)} at last sleep"
               if self.sleep_bt is not None else ""),
            f"vibrator  {'ON' if active(self.vibrator) else 'off'}"
            + (f"  ·  last {'/'.join(map(str, self.vibrator[1]))} ms" if self.vibrator else ""),
            f"speaker   {'ON' if self.mask & 0x04 else 'off'}"
            + (f"  ·  last {len(self.speaker[1]) // 2} notes" if self.speaker else ""),
            f"battery   {f'{self.battery} mV' if self.battery else '?'}",
            f"warnings  {'; '.join(self.warnings) or 'none'}",
        ])


@dataclass
class Frame:
    n: int
    event: int
    group: int
    panel: bytes
    changed: bytes
    stale: bytes
    border: str
    info: str


class Timeline:
    SNAP_EVERY = 64

    def __init__(self, on_event=None):
        self.parser = FrameParser()
        self.raw = bytearray()
        self.t0 = None
        self.events = []
        self.groups = []  # event idx of each wake
        self.head = Ssd1681()
        self.snaps = [(-1, self.head.copy())]
        self.applied = 0
        self.open = None
        self.frames = []
        self.script_ended = False
        self.on_event = on_event or (lambda e: None)
        self.last_rx = 0.0
        self.sys = SystemState()
        self.sys_snaps = [(-1, self.sys.copy())]
        self.wake_span = []  # [start us, end us, slept] per wake
        self.last_at = None

    # Input
    def feed(self, data, now=None):
        now = time.time() if now is None else now
        if self.t0 is None:
            self.t0 = now
        self.raw += data
        self.last_rx = now
        t = now - self.t0
        for item in self.parser.feed(data):
            if item[0] == "text":
                self._add("log", t, title="log", detail=item[1])
            elif item[0] == "bad":
                self.close_open()
                self._add("bad", t, title="corrupt", detail=item[1])
            else:
                self._frame(item[1], item[2], t)

    def _add(self, kind, t, **kw):
        group = len(self.groups) - 1
        ev = Event(len(self.events), kind, t, group, **kw)
        if ev.dt is not None:
            self.last_at = ev.dt
        ev.at = self.last_at
        if group >= 0 and ev.at is not None:
            span = self.wake_span[group]
            span[0] = ev.at if span[0] is None else span[0]
            span[1] = ev.at
        self.events.append(ev)
        if ev.closed:
            self.on_event(ev)
        return ev

    def _new_wake(self, dt):
        self.groups.append(len(self.events))
        self.wake_span.append([None, None, False])
        self.last_at = dt
        self.head.hw_reset()
        # RTC: locks and rail survive the sleep, the rest starts over
        self.sys = SystemState(mask=self.sys.mask, rail=self.sys.rail, battery=self.sys.battery,
                               sleep_bt=self.sys.sleep_bt)

    def _sys_changed(self):
        self.sys_snaps.append((len(self.events) - 1, self.sys.copy()))

    def _frame(self, typ, p, t):
        if typ != T_SPI or (p and p[0] == 0):
            self.close_open()
        # Frames from older firmware have no timestamp: tell them apart by length
        timed = {T_BOOT: 9, T_STUB: 8, T_BUTTON: 6, T_SLEEP: 13, T_SCRIPT_END: 4}
        dt = None
        if typ in (T_SPI_CMD, T_POWER, T_PERIPH, T_WIFI) or (typ in timed and len(p) >= timed[typ]):
            dt, p = u32(p), p[4:]
        if typ == T_SPI and p and p[0]:
            if self.open:
                self.open.data += p[1:]
            else:
                self._close(self._add("cmd", t, cmd=None, data=bytearray(p[1:]), closed=False))
        elif typ in (T_SPI, T_SPI_CMD) and p:
            data = p if typ == T_SPI_CMD else p[1:]
            for i, c in enumerate(data):
                ev = self._add("cmd", t, cmd=c, title=cmd_name(c), closed=False, dt=dt)
                if i < len(data) - 1:
                    self._close(ev)
                else:
                    self.open = ev
        elif typ == T_BOOT and len(p) >= 5:
            cause = APP_CAUSES[p[0]] if p[0] < len(APP_CAUSES) else str(p[0])
            script = {1: f"scripted {button_name(p[2])}", 2: "scripted idle"}.get(p[1], "")
            step = f" (step {p[3]}/{p[4]})" if p[1] else ""
            self._new_wake(dt)
            self._add("wake", t, title=f"BOOT {cause}", detail=f"{script}{step}".strip(), dt=dt)
            self._sys_changed()
        elif typ == T_STUB and len(p) >= 4:
            self._new_wake(dt)
            self._add("stub", t, title=f"STUB {stub_cause_name(u32(p))}", detail="wake stub", dt=dt)
            self._sys_changed()
        elif typ == T_BUTTON and len(p) >= 2:
            self._add("button", t, title=button_name(p[0]),
                      detail="scripted" if p[1] else "real touch", dt=dt)
        elif typ == T_SLEEP and len(p) >= 3:
            self._sleep(t, dt, p)
        elif typ == T_SCRIPT_END:
            self.script_ended = True
            self._add("script_end", t, title="SCRIPT END", detail="", dt=dt)
        elif typ == T_POWER and len(p) >= 4:
            flag, locked, mask, rail = p[:4]
            self.sys.mask, self.sys.rail = mask, bool(rail)
            self._add("power", t, title=f"{'LOCK' if locked else 'UNLOCK'} {flags_name(flag)}",
                      detail=f"locks {flags_name(mask)}  ·  rail {'HIGH' if rail else 'low'}", dt=dt,
                      info={"flag": flag, "locked": locked, "mask": mask, "rail": bool(rail)})
            self._sys_changed()
        elif typ == T_PERIPH and len(p) >= 2:
            kind, n = p[0], p[1]
            vals = [int.from_bytes(p[2 + 2 * i:4 + 2 * i], "little") for i in range(n)]
            if kind == 1:
                self.sys.vibrator = (dt, vals)
                detail = " / ".join(f"{'on' if i % 2 == 0 else 'off'} {v}" for i, v in enumerate(vals))
                self._add("periph", t, title="VIBRATE", detail=f"{detail} ms  ({sum(vals)} ms)", dt=dt,
                          info={"kind": "vibrator", "pattern": vals})
            else:
                self.sys.speaker = (dt, [v for i, v in enumerate(vals) if i % 2])
                notes = ", ".join(f"{f} Hz {d} ms" if f else f"rest {d} ms" for f, d in zip(vals[::2], vals[1::2]))
                self._add("periph", t, title="SPEAKER", detail=f"{notes}  ({sum(vals[1::2])} ms)", dt=dt,
                          info={"kind": "speaker", "pattern": vals})
            self._sys_changed()
        elif typ == T_WIFI and p:
            self._wifi(t, dt, p[0], p[1:])
        else:
            self._add("bad", t, title="unknown", detail=f"frame type {typ}, {len(p)} B", dt=dt)

    def _sleep(self, t, dt, p):
        depth = p[2] - 256 if p[2] > 127 else p[2]
        if p[0]:
            what = "next scripted step"
        elif p[1]:
            what = "wake stub armed"
        else:
            what = "full wake"
        ui = "watchface" if depth < 0 else f"menu depth {depth}"
        detail = f"{ui}, {what}"
        warnings = []
        if len(p) >= 9:
            mask, rail, wifi, bt = p[3], p[4], p[5], p[6]
            s = self.sys
            s.mask, s.rail, s.sleep_wifi, s.sleep_bt = mask, bool(rail), wifi, bt
            s.battery = p[7] | p[8] << 8
            if mask & ~0x02:  # The light may legitimately stay on
                warnings.append(f"locks held: {flags_name(mask & ~0x02)}")
            if rail and not mask:
                warnings.append("rail high without locks")
            if wifi != 0xFF and s.wifi not in ("off", "stopped"):
                warnings.append(f"WiFi still {s.wifi}")
            if bt == 2:
                warnings.append("BT controller enabled")
            if len(p) >= 10 and p[9]:
                warnings.append(f"trace UART stalled {p[9]}x since power-on (recovered)")
            s.warnings = tuple(warnings)
            detail += f"  ·  awake {ms(dt)}  ·  battery {s.battery} mV"
            if warnings:
                detail += "  ·  ⚠ " + "; ".join(warnings)
        if dt is not None and self.groups:
            self.wake_span[-1][2] = True
        self._add("sleep", t, title="SLEEP", detail=detail, dt=dt, info={"warnings": warnings})
        self._sys_changed()

    def _wifi(self, t, dt, event, d):
        s = self.sys
        if event == 1:
            s.wifi, title, detail = "started", "WIFI START", "driver started, connecting"
        elif event == 2:
            s.wifi, title, detail = "stopped", "WIFI STOP", "radio off"
        elif event == 3:
            ssid = bytes(d[2:]).decode("utf-8", "replace")
            s.wifi, title, detail = f"connected to {ssid}", "WIFI CONNECTED", f"{ssid}, channel {d[0]}"
        elif event == 4:
            reason = WIFI_REASONS.get(d[0], str(d[0])) if d else "?"
            rssi = (d[1] - 256 if d[1] > 127 else d[1]) if len(d) > 1 else 0
            s.wifi, title, detail = "disconnected", "WIFI DISCONNECTED", f"reason {reason}, rssi {rssi} dBm"
        elif event == 5:
            ip = ".".join(str(b) for b in d[:4])
            s.wifi, title, detail = f"online {ip}", "WIFI GOT IP", ip
        elif event == 6:
            s.ntp, title, detail = True, "NTP SYNCED", "time set from pool.ntp.org"
        else:
            title, detail = f"WIFI event {event}", d.hex()
        self._add("wifi", t, title=title, detail=detail, dt=dt)
        self._sys_changed()

    def sys_at(self, idx):
        i = bisect.bisect_right(self.sys_snaps, idx, key=lambda s: s[0]) - 1
        return self.sys_snaps[i][1]

    def group_events(self, g):
        end = self.groups[g + 1] if g + 1 < len(self.groups) else len(self.events)
        return self.events[self.groups[g]:end]

    def awake_stats(self):
        """Average on-time per wake kind, in us: app until SLEEP, stub until its last event.
        Both clocks start at app start / chip wake, so the end time is the on-time."""
        out = {}
        for g, (start, end, slept) in enumerate(self.wake_span):
            kind = self.events[self.groups[g]].kind
            done = slept if kind == "wake" else g + 1 < len(self.groups)
            if end is not None and done:
                out.setdefault(kind, []).append(end)
        return {k: (sum(v) / len(v), len(v)) for k, v in out.items()}

    def link(self, text, now=None):
        """Serial link up/down: what arrives next isn't the rest of a partial frame."""
        now = time.time() if now is None else now
        if self.t0 is None:
            self.t0 = now
        self.close_open()
        self.parser.buf.clear()
        self.parser.text.clear()
        self._add("link", now - self.t0, title="LINK", detail=text)

    def close_open(self):
        if self.open:
            ev, self.open = self.open, None
            self._close(ev)

    def _close(self, ev):
        ev.detail = self.head.apply(ev.cmd, ev.data)
        ev.closed = True
        if ev.cmd == 0x20 and ev.detail.startswith("REFRESH"):
            h = self.head
            ev.frame = len(self.frames)
            self.frames.append(Frame(len(self.frames) + 1, ev.idx, ev.group, h.panel,
                                     h.changed, h.stale, h.border_color, h.last_refresh))
        self.applied += 1
        if self.applied % self.SNAP_EVERY == 0:
            self.snaps.append((ev.idx, self.head.copy()))
        self.on_event(ev)

    # Queries
    def state_at(self, idx):
        i = bisect.bisect_right(self.snaps, idx, key=lambda s: s[0]) - 1
        start, snap = self.snaps[i]
        st = snap.copy()
        st.last_write = None
        for ev in self.events[start + 1:idx + 1]:
            if ev.kind == "cmd":
                st.apply(ev.cmd, ev.data)
            elif ev.kind in ("wake", "stub"):
                st.hw_reset()
        return st

    def frame_before(self, idx):
        """Last refresh frame at or before event idx."""
        evs = [f.event for f in self.frames]
        i = bisect.bisect_right(evs, idx) - 1
        return self.frames[i] if i >= 0 else None


# ---------------------------------------------------------------------------
# Images (stdlib only PNG)
# ---------------------------------------------------------------------------

WHITE, BLACK = b"\xff\xff\xff", b"\x00\x00\x00"
_BYTE_RGB = [b"".join(WHITE if v >> (7 - i) & 1 else BLACK for i in range(8)) for v in range(256)]
# Overlay layers: (mask, color on a white pixel, color on a black pixel), later wins
BLUE = (b"\xb4\xd2\xff", b"\x10\x30\x90")
RED = (b"\xff\x70\x70", b"\xc0\x00\x00")
ORANGE = (b"\xff\xc0\x60", b"\xb0\x50\x00")
YELLOW = (b"\xff\xf0\x90", b"\x70\x68\x00")


def last_refresh_layers(changed, stale):
    return [(changed, *BLUE), (stale, *RED)]


def next_refresh_layers(glass, partial, drive, target):
    """Pixels a refresh now changes (orange), drives without visible change (yellow,
    back buffer out of sync with the glass), or leaves wrong (red, stale result)."""
    diff = to_int(glass) ^ target
    return [(to_bytes(drive & ~diff & FULL), *YELLOW), (to_bytes(diff & drive), *ORANGE),
            (to_bytes(diff & ~drive & FULL), *RED)]


def plane_rgb(plane, layers=()):
    layers = [(m, cw, cb) for m, cw, cb in layers if any(m)]
    if not layers:
        return b"".join(_BYTE_RGB[b] for b in plane)
    out = bytearray()
    for i, b in enumerate(plane):
        hits = [(m[i], cw, cb) for m, cw, cb in layers if m[i]]
        if not hits:
            out += _BYTE_RGB[b]
            continue
        for bit in range(7, -1, -1):
            white = b >> bit & 1
            px = WHITE if white else BLACK
            for m, cw, cb in hits:
                if m >> bit & 1:
                    px = cw if white else cb
            out += px
    return bytes(out)


def glass_rgb(level, layers=()):
    """Glass grey levels (0 white .. 255 black) to RGB, overlay masks on top."""
    layers = [(m, cw, cb) for m, cw, cb in layers if any(m)]
    out = bytearray()
    for i in range(PLANE):
        hits = [(m[i], cw, cb) for m, cw, cb in layers if m[i]]
        for bit in range(8):
            v = level[i * 8 + bit]
            px = bytes((255 - v,) * 3)
            for m, cw, cb in hits:
                if m >> (7 - bit) & 1:
                    px = cw if v < 128 else cb
            out += px
    return bytes(out)


def rotate_rgb(rgb, deg):
    if deg % 360 == 0:
        return rgb
    px = [rgb[i:i + 3] for i in range(0, len(rgb), 3)]
    out = []
    for y in range(H):
        for x in range(W):
            if deg == 90:
                sx, sy = y, H - 1 - x
            elif deg == 180:
                sx, sy = W - 1 - x, H - 1 - y
            else:
                sx, sy = W - 1 - y, x
            out.append(px[sy * W + sx])
    return b"".join(out)


def add_border(rgb, color, b=6):
    c = WHITE if color == "white" else BLACK
    w = W + 2 * b
    rows = [c * w] * b
    rows += [c * b + rgb[y * W * 3:(y + 1) * W * 3] + c * b for y in range(H)]
    rows += [c * w] * b
    return b"".join(rows), w, H + 2 * b


def png_encode(rgb, w, h):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def png_decode_bits(data):
    """8-bit gray/RGB(A) PNG -> (w, h, list of 0 black / 1 white)."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat = 8, b""
    while pos < len(data):
        ln, t = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        if t == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif t == b"IDAT":
            idat += body
        pos += 12 + ln
    bpp = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    assert depth == 8 and bpp, "only 8-bit gray/RGB/RGBA PNGs are supported"
    raw, stride, prev, bits, p = zlib.decompress(idat), w * bpp, bytearray(w * bpp), [], 0
    for _ in range(h):
        f, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 0xFF
            elif f == 2:
                line[i] = (line[i] + b) & 0xFF
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        bits += [1 if (line[x * bpp] if bpp < 3 else sum(line[x * bpp:x * bpp + 3]) // 3) >= 128 else 0
                 for x in range(w)]
        prev = line
    return w, h, bits


def frame_png(frame, rotate=0, overlay=False):
    rgb = plane_rgb(frame.panel, last_refresh_layers(frame.changed, frame.stale) if overlay else ())
    rgb, w, h = add_border(rotate_rgb(rgb, rotate), frame.border)
    return png_encode(rgb, w, h)


# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

class SerialSource:
    """Live port. Survives unplug/replug: reopens the same USB device (VID/PID/serial
    number) whatever name it comes back as, and retries a port that fails to open."""

    def __init__(self, port, baud, reset, record):
        import serial
        from serial.tools import list_ports
        self.serial, self.list_ports = serial, list_ports
        self.port, self.baud = port, baud
        self.ident = self._ident_of(port)
        self.rec = open(record, "wb") if record else None
        self.q = queue.Queue()
        self.links = queue.Queue()
        self.ser = None
        if not self._open(reset):
            self.links.put(f"waiting for {port}")
        threading.Thread(target=self._run, daemon=True).start()

    def _ident_of(self, port):
        for p in self.list_ports.comports():
            if p.device == port:
                return (p.vid, p.pid, p.serial_number)
        return None

    def _candidates(self):
        ports = list(self.list_ports.comports())
        if self.ident and self.ident[0] is not None:
            same = [p.device for p in ports if (p.vid, p.pid, p.serial_number) == self.ident]
            # The old name first, it is the most likely
            return sorted(same, key=lambda d: d != self.port)
        return [self.port] if os.path.exists(self.port) else []  # No USB identity: same path

    def _open(self, reset=False):
        for port in self._candidates():
            ser = self.serial.Serial()
            ser.port, ser.baudrate, ser.timeout = port, self.baud, 0.05
            ser.dtr = ser.rts = False  # Don't reset / enter bootloader on open
            try:
                ser.open()
            except Exception:
                continue
            if reset:  # EN pulse through the usual RTS auto-reset circuit
                ser.rts = True
                time.sleep(0.1)
                ser.rts = False
            self.ser, self.port = ser, port
            self.ident = self._ident_of(port) or self.ident
            return True
        return False

    def _run(self):
        while True:
            if self.ser is None:
                if self._open():
                    self.links.put(f"connected to {self.port}")
                else:
                    time.sleep(0.5)
                continue
            try:
                d = self.ser.read(self.ser.in_waiting or 1)
            except Exception as e:  # Unplugged
                try:
                    self.ser.close()
                except Exception:
                    pass
                self.ser = None
                self.links.put(f"disconnected from {self.port} ({e.__class__.__name__}), "
                               "waiting for the device")
                continue
            if d:
                self.q.put(d)
                if self.rec:
                    self.rec.write(d)
                    self.rec.flush()

    def read(self):
        out = bytearray()
        while not self.q.empty():
            out += self.q.get()
        return bytes(out)

    def link_events(self):
        out = []
        while not self.links.empty():
            out.append(self.links.get())
        return out

    finished = False


class ReplaySource:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        self.finished = False

    def read(self):
        d, self.data, self.finished = self.data, b"", True
        return d

    def link_events(self):
        return []


# ---------------------------------------------------------------------------
# Headless
# ---------------------------------------------------------------------------

def run_headless(args, source):
    verbose = args.verbose
    script_end_sleep = [False]

    def on_event(ev):
        t = f"{ev.at / 1000:9.1f}ms" if ev.at is not None else f"{ev.t:9.3f}s "
        if ev.kind in ("wake", "stub"):
            print(f"\n[{t}] ==== {ev.title} {ev.detail}")
        elif ev.kind == "cmd":
            if ev.frame is not None:
                print(f"[{t}]   {ev.detail}  -> frame {ev.frame + 1:03d}")
            elif verbose:
                print(f"[{t}]   {cmd_name(ev.cmd):18} {ev.detail}")
        elif ev.kind == "log":
            print(f"[{t}]   | {ev.detail}")
        else:
            print(f"[{t}]   {ev.title} {ev.detail}")
        if ev.kind == "sleep" and tl.script_ended:
            script_end_sleep[0] = True

    tl = Timeline(on_event)
    start = time.time()
    try:
        while True:
            for text in source.link_events():
                tl.link(text)
            d = source.read()
            if d:
                tl.feed(d)
            elif time.time() - tl.last_rx > 0.3:
                tl.close_open()
            if source.finished and not d:
                break
            if args.until_script_end and script_end_sleep[0]:
                time.sleep(0.5)  # Let the last frame finish
                tl.feed(source.read())
                break
            if args.timeout and time.time() - start > args.timeout:
                print("\ntimeout")
                break
            if not d:
                time.sleep(0.02)
    except KeyboardInterrupt:
        pass
    tl.close_open()
    print(f"\n{len(tl.groups)} wakes, {len(tl.frames)} refresh frames, {len(tl.events)} events")
    for kind, (avg, n) in tl.awake_stats().items():
        print(f"average awake ({'app' if kind == 'wake' else 'stub'}, {n} wakes): {ms(avg)}")

    if args.save:
        with open(args.save, "wb") as f:
            f.write(tl.raw)
    if args.frames:
        os.makedirs(args.frames, exist_ok=True)
        for fr in tl.frames:
            with open(os.path.join(args.frames, f"{fr.n:03d}.png"), "wb") as f:
                f.write(frame_png(fr, args.rotate))
        print(f"wrote {len(tl.frames)} frames to {args.frames}")
    if args.compare:
        return compare(tl.frames, args)
    return 0


def compare(frames, args):
    golden = sorted(f for f in os.listdir(args.compare) if re.fullmatch(r"\d{3}\.png", f))
    failures = 0
    if len(golden) != len(frames):
        print(f"FAIL frame count: got {len(frames)}, golden {len(golden)}")
        failures += 1
    diff_dir = args.frames or args.compare.rstrip("/") + "_diff"
    for fr in frames:
        path = os.path.join(args.compare, f"{fr.n:03d}.png")
        if not os.path.exists(path):
            continue
        with open(path, "rb") as f:
            w, h, bits = png_decode_bits(f.read())
        got_png = frame_png(fr, args.rotate)
        _, _, got = png_decode_bits(got_png)
        if (w, h) != (W + 12, H + 12):
            print(f"FAIL {fr.n:03d}: golden size {w}x{h}")
            failures += 1
            continue
        diff = [i for i in range(len(bits)) if bits[i] != got[i]]
        if diff:
            failures += 1
            os.makedirs(diff_dir, exist_ok=True)
            rgb = bytearray(png_rgb_from_bits(got, w))
            for i in diff:
                rgb[i * 3:i * 3 + 3] = b"\xff\x00\x00"
            with open(os.path.join(diff_dir, f"{fr.n:03d}_diff.png"), "wb") as f:
                f.write(png_encode(bytes(rgb), w, h))
            print(f"FAIL {fr.n:03d}: {len(diff)} px differ ({fr.info})")
        else:
            print(f"ok   {fr.n:03d}")
    print("PASS" if not failures else f"{failures} failure(s), diffs in {diff_dir}")
    return 1 if failures else 0


def png_rgb_from_bits(bits, w):
    return b"".join(WHITE if b else BLACK for b in bits)


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

def run_gui(args, source):
    import tkinter as tk
    from tkinter import ttk, filedialog

    root = tk.Tk()
    root.title("LightInk trace")
    tl = Timeline()
    st = {"cursor": None, "rows": 0, "groups": 0, "dirty": True, "last_render": 0.0,
          "rotate": args.rotate, "slider_lock": False, "images": []}
    follow = tk.BooleanVar(value=True)
    overlay = tk.StringVar(value=args.overlay)
    show_logs = tk.BooleanVar(value=True)

    # Layout
    bar = ttk.Frame(root, padding=4)
    bar.pack(side=tk.TOP, fill=tk.X)
    bottom = ttk.Frame(root, padding=4)
    bottom.pack(side=tk.BOTTOM, fill=tk.X)
    body = ttk.Frame(root)
    body.pack(side=tk.TOP, fill=tk.BOTH, expand=True)

    left = ttk.Frame(body, padding=4)
    left.pack(side=tk.LEFT, fill=tk.Y)
    ttk.Label(left, text="Panel (glass)", font=("TkDefaultFont", 10, "bold")).pack(anchor=tk.W)
    panel_cv = tk.Canvas(left, width=2 * (W + 12), height=2 * (H + 12), bg="#888", highlightthickness=0)
    panel_cv.pack()
    panel_info = ttk.Label(left, text="", wraplength=2 * (W + 12), justify=tk.LEFT)
    panel_info.pack(anchor=tk.W, pady=(4, 0))
    legend = ttk.Label(left, text="", foreground="#666", wraplength=2 * (W + 12), justify=tk.LEFT)
    legend.pack(anchor=tk.W)
    ttk.Label(left, text="Controller", font=("TkDefaultFont", 10, "bold")).pack(anchor=tk.W, pady=(6, 0))
    regs = tk.Text(left, width=60, height=10, wrap=tk.NONE, font=("Monospace", 9), relief=tk.FLAT,
                   background=root.cget("bg"))
    regs.pack(anchor=tk.W)
    ttk.Label(left, text="System", font=("TkDefaultFont", 10, "bold")).pack(anchor=tk.W, pady=(6, 0))
    sysbox = tk.Text(left, width=60, height=9, wrap=tk.NONE, font=("Monospace", 9), relief=tk.FLAT,
                     background=root.cget("bg"))
    sysbox.pack(anchor=tk.W)
    for box in (regs, sysbox):  # Values that the selected event changed
        box.tag_configure("changed", foreground="#0050d0", font=("Monospace", 9, "bold"))

    mid = ttk.Frame(body, padding=4)
    mid.pack(side=tk.LEFT, fill=tk.Y)
    ram_cv, ram_lbl = {}, {}
    for name, label in (("front", "Front buffer 0x24 raw (new)"), ("back", "Back buffer 0x26 raw (old)"),
                        ("diff", "front != back (partial drives)")):
        ram_lbl[name] = ttk.Label(mid, text=label, font=("TkDefaultFont", 10, "bold"))
        ram_lbl[name].pack(anchor=tk.W)
        cv = tk.Canvas(mid, width=W + 2, height=H + 2, bg="#888", highlightthickness=0)
        cv.pack(pady=(0, 8))
        ram_cv[name] = cv
    ttk.Label(mid, text="green = RAM window\norange = written by\nselected command\nred = bank counter",
              foreground="#666", justify=tk.LEFT).pack(anchor=tk.W)

    right = ttk.PanedWindow(body, orient=tk.VERTICAL)
    right.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=4, pady=4)
    tree_frame = ttk.Frame(right)
    right.add(tree_frame, weight=3)
    lanes_frame = ttk.Frame(right)
    right.add(lanes_frame, weight=1)
    ttk.Label(lanes_frame, text="Wake timeline (click to jump)",
              font=("TkDefaultFont", 10, "bold")).pack(anchor=tk.W, pady=(4, 0))
    lanes_cv = tk.Canvas(lanes_frame, height=250, bg="white", highlightthickness=0)
    lanes_cv.pack(fill=tk.BOTH, expand=True)

    tree = ttk.Treeview(tree_frame, columns=("t", "detail"), selectmode="browse")
    tree.heading("#0", text="event")
    tree.heading("t", text="t ms")
    tree.heading("detail", text="detail")
    tree.column("#0", width=230, stretch=False)
    tree.column("t", width=80, stretch=False, anchor=tk.E)
    tree.column("detail", width=480)
    sb = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL, command=tree.yview)
    tree.configure(yscrollcommand=sb.set)
    sb.pack(side=tk.RIGHT, fill=tk.Y)
    tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    tree.tag_configure("wake", font=("TkDefaultFont", 9, "bold"), background="#e8eefc")
    tree.tag_configure("stub", font=("TkDefaultFont", 9, "bold"), background="#eef8e8")
    tree.tag_configure("log", foreground="#777")
    tree.tag_configure("refresh", background="#fff2b0")
    tree.tag_configure("button", foreground="#0a4fc4")
    tree.tag_configure("bad", foreground="#c00000")
    tree.tag_configure("sleep", foreground="#6a3fa0")
    tree.tag_configure("power", foreground="#8a5a00")
    tree.tag_configure("periph", foreground="#a0307a")
    tree.tag_configure("wifi", foreground="#007a7a")
    tree.tag_configure("warn", foreground="#6a3fa0", background="#ffd6d6")
    tree.tag_configure("link", foreground="#a05000", font=("TkDefaultFont", 9, "bold"))

    slider = ttk.Scale(bottom, from_=0, to=0, orient=tk.HORIZONTAL)
    slider.pack(side=tk.TOP, fill=tk.X)
    status = ttk.Label(bottom, text="waiting for data...")
    status.pack(side=tk.LEFT)

    # Navigation
    def set_cursor(idx, from_tree=False):
        if not tl.events:
            return
        idx = max(0, min(idx, len(tl.events) - 1))
        st["cursor"] = idx
        st["dirty"] = True
        if not from_tree:
            iid = f"e{idx}"
            if tree.exists(iid):
                tree.selection_set(iid)
                tree.see(iid)

    def manual(idx):
        follow.set(False)
        set_cursor(idx)

    def cur():
        return st["cursor"] if st["cursor"] is not None else len(tl.events) - 1

    def step(d):
        i = cur() + d
        while 0 <= i < len(tl.events) and not show_logs.get() and tl.events[i].kind == "log":
            i += d
        manual(i)

    def jump_refresh(d):
        evs = [f.event for f in tl.frames]
        c = cur()
        cands = [e for e in evs if (e > c if d > 0 else e < c)]
        if cands:
            manual(cands[0] if d > 0 else cands[-1])

    def jump_wake(d):
        c = cur()
        cands = [g for g in tl.groups if (g > c if d > 0 else g < c)]
        if cands:
            manual(cands[0] if d > 0 else cands[-1])

    def on_select(_):
        sel = tree.selection()
        if not sel:
            return
        iid = sel[0]
        if iid.startswith("e"):
            idx = int(iid[1:])
        elif iid.startswith("g") and iid != "g-1":
            idx = tl.groups[int(iid[1:])]
        else:
            return
        if idx != st["cursor"]:
            if idx != len(tl.events) - 1:
                follow.set(False)
            set_cursor(idx, from_tree=True)

    def on_slider(v):
        if not st["slider_lock"]:
            manual(int(float(v)))

    def rotate():
        st["rotate"] = (st["rotate"] + 90) % 360
        st["dirty"] = True

    def save_png():
        fr = tl.frame_before(cur())
        if not fr:
            return
        path = filedialog.asksaveasfilename(defaultextension=".png", initialfile=f"frame_{fr.n:03d}.png")
        if path:
            with open(path, "wb") as f:
                f.write(frame_png(fr, st["rotate"]))

    def export_frames():
        d = filedialog.askdirectory()
        if d:
            for fr in tl.frames:
                with open(os.path.join(d, f"{fr.n:03d}.png"), "wb") as f:
                    f.write(frame_png(fr, st["rotate"]))
            status.config(text=f"exported {len(tl.frames)} frames to {d}")

    def save_capture():
        path = filedialog.asksaveasfilename(defaultextension=".bin", initialfile="capture.bin")
        if path:
            with open(path, "wb") as f:
                f.write(tl.raw)

    for text, cmd in (("⏮ wake", lambda: jump_wake(-1)), ("◆ refresh", lambda: jump_refresh(-1)),
                      ("◀", lambda: step(-1)), ("▶", lambda: step(1)),
                      ("refresh ◆", lambda: jump_refresh(1)), ("wake ⏭", lambda: jump_wake(1))):
        ttk.Button(bar, text=text, command=cmd, width=len(text) + 1).pack(side=tk.LEFT)
    ttk.Separator(bar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=6)
    ttk.Checkbutton(bar, text="Follow live", variable=follow,
                    command=lambda: follow.get() and set_cursor(len(tl.events) - 1)).pack(side=tk.LEFT)
    ttk.Label(bar, text="  Overlay:").pack(side=tk.LEFT)
    for text, value in (("off", "off"), ("last refresh", "last"), ("next refresh", "next")):
        ttk.Radiobutton(bar, text=text, value=value, variable=overlay,
                        command=lambda: st.update(dirty=True)).pack(side=tk.LEFT)
    ttk.Label(bar, text=" ").pack(side=tk.LEFT)
    ttk.Checkbutton(bar, text="Logs", variable=show_logs,
                    command=lambda: rebuild_tree()).pack(side=tk.LEFT)
    ttk.Button(bar, text="Rotate", command=rotate).pack(side=tk.LEFT, padx=(6, 0))
    ttk.Separator(bar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=6)
    ttk.Button(bar, text="Save PNG", command=save_png).pack(side=tk.LEFT)
    ttk.Button(bar, text="Export frames", command=export_frames).pack(side=tk.LEFT)
    ttk.Button(bar, text="Save capture", command=save_capture).pack(side=tk.LEFT)
    src = ttk.Label(bar, text=args.port or os.path.basename(args.replay), foreground="#666")
    src.pack(side=tk.RIGHT)

    tree.bind("<<TreeviewSelect>>", on_select)
    slider.configure(command=on_slider)
    for w in (root, tree):
        w.bind("<Left>", lambda e: (step(-1), "break")[1])
        w.bind("<Right>", lambda e: (step(1), "break")[1])
        w.bind("<Prior>", lambda e: (jump_refresh(-1), "break")[1])
        w.bind("<Next>", lambda e: (jump_refresh(1), "break")[1])
        w.bind("<Home>", lambda e: (manual(0), "break")[1])
        w.bind("<End>", lambda e: (manual(len(tl.events) - 1), "break")[1])
    root.bind("<f>", lambda e: (follow.set(not follow.get()), follow.get() and set_cursor(len(tl.events) - 1)))

    # Tree sync
    def ensure_group(g):
        iid = f"g{g}"
        if tree.exists(iid):
            return iid
        if g < 0:
            tree.insert("", tk.END, iid=iid, text="before first wake", open=True)
            return iid
        ev = tl.events[tl.groups[g]]
        tree.insert("", tk.END, iid=iid, text=f"#{g + 1} {ev.title}",
                    values=group_values(g), tags=(ev.kind,), open=True)
        if follow.get() and g > 0 and tree.exists(f"g{g - 1}"):
            tree.item(f"g{g - 1}", open=False)
        return iid

    def t_text(ev):
        return f"{ev.at / 1000:.1f}" if ev.at is not None else f"{ev.t:.3f}s"

    def group_values(g):
        ev = tl.events[tl.groups[g]]
        end = tl.wake_span[g][1]
        awake = f"awake {ms(end)}" if end is not None else ""
        return t_text(ev), "  ·  ".join(x for x in (ev.detail, awake) if x)

    def row_values(ev):
        if ev.kind == "cmd":
            text = cmd_name(ev.cmd) if ev.cmd is None else f"{ev.cmd:02X} {cmd_name(ev.cmd)}"
            tags = ("refresh",) if ev.frame is not None else ()
            return text, (t_text(ev), ev.detail if ev.closed else f"... {len(ev.data)} B"), tags
        tags = ("warn",) if ev.info.get("warnings") else (ev.kind,)
        return ev.title, (t_text(ev), ev.detail), tags

    def insert_rows():
        for ev in tl.events[st["rows"]:]:
            if ev.kind in ("wake", "stub"):
                ensure_group(ev.group)
            elif show_logs.get() or ev.kind != "log":
                text, values, tags = row_values(ev)
                tree.insert(ensure_group(ev.group), tk.END, iid=f"e{ev.idx}", text=text,
                            values=values, tags=tags)
        st["rows"] = len(tl.events)

    def rebuild_tree():
        tree.delete(*tree.get_children())
        st["rows"] = 0
        insert_rows()
        st["dirty"] = True

    pending_updates = set()

    def on_event(ev):
        if ev.idx < st["rows"]:
            pending_updates.add(ev.idx)
    tl.on_event = on_event

    # Rendering
    def photo(rgb, w, h, zoom):
        img = tk.PhotoImage(data=base64.b64encode(png_encode(rgb, w, h)))
        if zoom > 1:
            img = img.zoom(zoom)
        st["images"].append(img)
        return img

    # Logic-analyzer style view of one wake
    LANE_H, LABEL_W = 16, 100
    FLAG_COLORS = {0x01: "#7aa6d8", 0x02: "#f5d33b", 0x04: "#d07ac0", 0x08: "#7cc47c",
                   0x10: "#4fb3b3", 0x20: "#6f7fe0", 0x40: "#c49a6c"}

    def nice_step(span):
        raw = span / 8
        mag = 10 ** max(0, len(str(int(raw))) - 1)
        return next(m * mag for m in (1, 2, 5, 10) if m * mag >= raw)

    def draw_lanes(idx):
        cv = lanes_cv
        cv.delete("all")
        st["lanes"] = None
        ev = tl.events[idx]
        g = ev.group
        if g < 0:
            return
        evs = tl.group_events(g)
        end = tl.wake_span[g][1]
        if end is None:
            cv.create_text(8, 8, anchor=tk.NW, fill="#666",
                           text="no device timestamps in this wake (older firmware)")
            return
        end = max(end, 1)
        x0, x1 = LABEL_W, max(cv.winfo_width(), 500) - 12
        span = end * 1.02

        def X(us):
            return x0 + (x1 - x0) * us / span

        # Power locks and rail, starting from what the RTC kept over the sleep
        s0 = tl.sys_at(tl.groups[g])
        mask, rail = s0.mask, s0.rail
        since = {b: 0 for b, _ in POWER_FLAGS if mask & b}
        rail_since = 0 if rail else None
        flag_iv = {b: [] for b, _ in POWER_FLAGS}
        rail_iv, vib_iv = [], []
        marks = {"display": [], "wifi": [], "touch": [], "logs": []}
        for e in evs:
            if e.at is None:
                continue
            if e.kind == "power":
                m, r = e.info["mask"], e.info["rail"]
                for b, _ in POWER_FLAGS:
                    if m & b and not mask & b:
                        since[b] = e.at
                    elif mask & b and not m & b:
                        flag_iv[b].append((since.pop(b, 0), e.at))
                if r and not rail:
                    rail_since = e.at
                elif rail and not r:
                    rail_iv.append((rail_since or 0, e.at))
                    rail_since = None
                mask, rail = m, r
            elif e.kind == "periph" and e.info["kind"] == "vibrator":
                t = e.at
                for i, d in enumerate(e.info["pattern"]):
                    if i % 2 == 0:
                        vib_iv.append((t, t + d * 1000))
                    t += d * 1000
            elif e.kind == "cmd":
                marks["display"].append((e.at, "#e08000" if e.frame is not None else "#888",
                                         "refresh" if e.frame is not None else "", e.idx))
            elif e.kind == "wifi":
                marks["wifi"].append((e.at, "#007a7a", e.title.replace("WIFI ", "").lower(), e.idx))
            elif e.kind == "button":
                marks["touch"].append((e.at, "#0a4fc4", e.title, e.idx))
            elif e.kind == "log":
                marks["logs"].append((e.at, "#aaa", "", e.idx))
        for b, t in since.items():
            flag_iv[b].append((t, end))
        if rail_since is not None:
            rail_iv.append((rail_since, end))

        lanes = [("awake", [(0, end)], "#9ec5ff", None), ("rail high", rail_iv, "#f2b84b", None)]
        lanes += [(f"lock {n}", flag_iv[b], FLAG_COLORS[b], None) for b, n in POWER_FLAGS if flag_iv[b]]
        if vib_iv:
            lanes.append(("vibrator", vib_iv, "#d0508a", None))
        lanes += [(name, [], None, marks[name]) for name in ("display", "wifi", "touch", "logs") if marks[name]]

        top = 18
        step = nice_step(span)
        t = 0
        while t <= span:
            cv.create_line(X(t), top - 4, X(t), top + LANE_H * len(lanes), fill="#eee")
            cv.create_text(X(t), 2, anchor=tk.N, text=f"{t / 1000:g} ms", fill="#666", font=("TkDefaultFont", 7))
            t += step
        for i, (name, ivs, color, mk) in enumerate(lanes):
            y = top + i * LANE_H
            if i % 2:
                cv.create_rectangle(0, y, x1 + 12, y + LANE_H, fill="#f6f6f6", outline="")
            cv.create_text(4, y + LANE_H / 2, anchor=tk.W, text=name, font=("TkDefaultFont", 8))
            for a, b in ivs:
                cv.create_rectangle(X(a), y + 3, max(X(b), X(a) + 2), y + LANE_H - 3, fill=color, outline="")
            for at, c, label, _ in mk or ():
                cv.create_line(X(at), y + 2, X(at), y + LANE_H - 2, fill=c, width=2 if label else 1)
                if label and name != "display":
                    cv.create_text(X(at) + 3, y + LANE_H / 2, anchor=tk.W, text=label, fill=c,
                                   font=("TkDefaultFont", 7))
        if ev.at is not None:
            cv.create_line(X(ev.at), top - 4, X(ev.at), top + LANE_H * len(lanes), fill="#e00000")
        done = tl.wake_span[g][2] or evs[0].kind == "stub"
        cv.create_text(x1, top + LANE_H * len(lanes) + 2, anchor=tk.NE, fill="#666", font=("TkDefaultFont", 8),
                       text=f"wake #{g + 1}: awake {ms(end)}" + ("" if done else " so far"))
        st["lanes"] = (x0, x1, span, evs)

    def on_lanes_click(e):
        if not st.get("lanes"):
            return
        x0, x1, span, evs = st["lanes"]
        t = (e.x - x0) / (x1 - x0) * span
        timed = [ev for ev in evs if ev.at is not None and (show_logs.get() or ev.kind != "log")]
        if timed:
            manual(min(timed, key=lambda ev: abs(ev.at - t)).idx)

    lanes_cv.bind("<Button-1>", on_lanes_click)
    lanes_cv.bind("<Configure>", lambda e: st.update(dirty=True))

    def render():
        st["images"] = []
        if not tl.events:
            return
        idx = cur()
        ev = tl.events[idx]
        s = tl.state_at(idx)
        fr = tl.frame_before(idx)

        partial, drive, target = s.pending()
        mode = overlay.get()
        if mode == "next":
            layers = next_refresh_layers(s.panel, partial, drive, target)
            n = [to_int(m).bit_count() for m, _, _ in layers]
            legend.config(text=f"next {'partial' if partial else 'full'} refresh: orange = will change ({n[1]} px), "
                               f"yellow = driven, no visible change ({n[0]} px), "
                               f"red = will stay wrong ({n[2]} px)")
        elif mode == "last" and fr:
            layers = last_refresh_layers(fr.changed, fr.stale)
            legend.config(text="last refresh: blue = changed, red = stale/grey (glass not at front: weak push or stale back)")
        else:
            layers = ()
            legend.config(text="")

        panel_cv.delete("all")
        if fr:
            rgb = glass_rgb(s.level, layers)
            rgb, w, h = add_border(rotate_rgb(rgb, st["rotate"]), s.border_color)
            panel_cv.create_image(0, 0, anchor=tk.NW, image=photo(rgb, w, h, 2))
            panel_info.config(text=f"frame {fr.n:03d}: {fr.info}  ·  wake #{fr.group + 1}")
        else:
            panel_info.config(text="no refresh yet")

        for name, cv in ram_cv.items():
            cv.delete("all")
            if name == "diff":
                # What the controller compares, after the 0x21 RAM options
                d = s._eff(s.front, s.upd1[0] & 0xF) ^ s._eff(s.back, s.upd1[0] >> 4)
                rgb = plane_rgb(b"\xff" * PLANE, [(to_bytes(d), ORANGE[1], ORANGE[1])])
                cv.create_image(1, 1, anchor=tk.NW, image=photo(rgb, W, H, 1))
                ram_lbl[name].config(text=f"front != back: {d.bit_count()} px (partial drives)")
                continue
            plane = s.front if name == "front" else s.back
            cv.create_image(1, 1, anchor=tk.NW, image=photo(plane_rgb(plane), W, H, 1))
            xs, ys, xe, ye = s.window
            cv.create_rectangle(1 + min(xs, xe) * 8, 1 + min(ys, ye), 1 + max(xs, xe) * 8 + 8,
                                1 + max(ys, ye) + 1, outline="#00b000")
            lw = s.last_write
            if lw and lw[0] == name and ev.kind == "cmd" and ev.cmd in (0x24, 0x26):
                _, x0, y0, x1, y1 = lw
                cv.create_rectangle(1 + x0 * 8, 1 + y0, 1 + x1 * 8 + 8, 1 + y1 + 1,
                                    outline="#ff8000", width=2)
            cx, cy = s.ctr[s.k if name == "front" else 1 - s.k]  # Each bank has its own counter
            cv.create_rectangle(1 + cx * 8, 1 + cy, 1 + cx * 8 + 8, 2 + cy, outline="#e00000")

        # Highlight what changed since the previous event
        prev = (tl.state_at(idx - 1).registers(), tl.sys_at(idx - 1).lines(tl.events[idx - 1].at)) \
            if idx else ("", "")
        for box, text, before in ((regs, s.registers(), prev[0]),
                                  (sysbox, tl.sys_at(idx).lines(ev.at), prev[1])):
            box.config(state=tk.NORMAL)
            box.delete("1.0", tk.END)
            old = before.splitlines()
            for i, line in enumerate(text.splitlines()):
                changed = i < len(old) and line != old[i]
                box.insert(tk.END, line[:10])  # Label column
                box.insert(tk.END, line[10:] + "\n", ("changed",) if changed else ())
            box.config(state=tk.DISABLED)
        draw_lanes(idx)

        g = ev.group
        stats = "  ·  ".join(f"avg awake {'app' if k == 'wake' else 'stub'} {ms(a)} (n={n})"
                             for k, (a, n) in sorted(tl.awake_stats().items()))
        status.config(text=f"event {idx + 1}/{len(tl.events)}  ·  wake {g + 1}/{len(tl.groups)}  ·  "
                           f"frames {len(tl.frames)}  ·  {len(tl.raw)} B received"
                           + (f"  ·  {stats}" if stats else "")
                           + ("  ·  script ended" if tl.script_ended else ""))
        st["slider_lock"] = True
        slider.configure(to=max(0, len(tl.events) - 1))
        slider.set(idx)
        st["slider_lock"] = False

    def poll():
        for text in source.link_events():
            tl.link(text)
            src.config(text=text, foreground="#a05000" if "connected to" not in text else "#666")
        d = source.read()
        if d:
            tl.feed(d)
        elif tl.open and time.time() - tl.last_rx > 0.3:
            tl.close_open()
        if len(tl.events) > st["rows"] or pending_updates:
            insert_rows()
            for i in sorted(pending_updates):
                if tree.exists(f"e{i}"):
                    text, values, tags = row_values(tl.events[i])
                    tree.item(f"e{i}", text=text, values=values, tags=tags)
            pending_updates.clear()
            for g in range(max(0, len(tl.groups) - 2), len(tl.groups)):
                if tree.exists(f"g{g}"):
                    tree.item(f"g{g}", values=group_values(g))
            if follow.get():
                set_cursor(len(tl.events) - 1)
            st["dirty"] = True
        now = time.time()
        if st["dirty"] and now - st["last_render"] > 0.1:
            st["dirty"] = False
            st["last_render"] = now
            render()
        root.after(30, poll)

    if args.screenshot:  # Testing aid: render, save a screenshot with ImageMagick, quit
        def shot():
            if args.goto is not None:
                manual(args.goto)
                render()
            root.update()
            os.system(f"import -window {root.winfo_id()} {args.screenshot}")
            root.destroy()
        root.after(1500, shot)

    root.geometry("1600x960")
    poll()
    root.mainloop()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n\n".join(__doc__.split("\n\n")[1:]))
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="serial port, e.g. /dev/ttyUSB0")
    src.add_argument("--replay", help="raw capture file to replay")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--reset", action="store_true", help="pulse RTS (EN) on connect to capture a first boot")
    ap.add_argument("--record", help="also write the raw serial stream to this file")
    ap.add_argument("--rotate", type=int, default=180, choices=(0, 90, 180, 270),
                    help="panel view rotation (180 = the watch's default rotation 2)")
    ap.add_argument("--overlay", default="last", choices=("off", "last", "next"),
                    help="GUI panel overlay: last refresh changes, or what the next refresh will do")
    ap.add_argument("--headless", action="store_true", help="no GUI, print decoded events")
    ap.add_argument("-v", "--verbose", action="store_true", help="headless: print every command")
    ap.add_argument("--frames", help="headless: write every refresh as NNN.png here")
    ap.add_argument("--compare", help="headless: compare refresh frames with NNN.png golden images here")
    ap.add_argument("--save", help="headless: save the raw capture here")
    ap.add_argument("--until-script-end", action="store_true", help="headless: stop after the scripted run")
    ap.add_argument("--timeout", type=float, default=0, help="headless: stop after N seconds")
    ap.add_argument("--screenshot", help=argparse.SUPPRESS)
    ap.add_argument("--goto", type=int, help=argparse.SUPPRESS)
    args = ap.parse_args()

    source = (SerialSource(args.port, args.baud, args.reset, args.record) if args.port
              else ReplaySource(args.replay))
    sys.exit(run_headless(args, source) if args.headless else run_gui(args, source))


if __name__ == "__main__":
    main()
