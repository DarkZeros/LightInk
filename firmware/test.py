#!/usr/bin/env python3
"""LightInk on-device tests.

Builds test images, flashes them, replays scripted touches, and checks the watch:
  * trace suite: the debug trace (CONFIG_LIGHTINK_TRACE) streams every wake, button,
    power lock, peripheral, WiFi event, radio burst and display command over UART0.
    Each scripted step is checked against a model of the menus (which item has the
    focus, what the action must do) and every sleep against what may stay powered.
  * power suite: the same scripts on an image without the trace (CONFIG_LIGHTINK_SCRIPT),
    powered and measured by a Monsoon power monitor with USB cut off. Checks the idle
    floor, the average current with the minute wake stub, and that every feature
    leaves nothing powered behind.

The watch only needs flashing once per image (trace, power, release): each test then
writes its script to one flash sector (CONFIG_LIGHTINK_SCRIPT_FLASH_OFFSET) and cold
boots. Every test starts from a cold boot, and the test images keep the settings and
the phone pairing out of flash (CONFIG_LIGHTINK_NO_STORAGE), so the settings start at
their defaults with no phone paired, and what the watch has stored is still there when
the normal image is flashed again.
The Radio menu sends garbage bursts in the test images (CONFIG_LIGHTINK_FAKE_SIGNALS),
never the signals of secrets.cpp.

Setup:
  Trace suite: the watch on USB (CP2104), its battery or the Monsoon powering it.
  Power suite: `pip install monsoon`. The Monsoon main output on the battery pads,
  the watch USB through the Monsoon USB passthrough port (--usb monsoon, default), so
  USB can be cut while measuring: the TP4054 charger and the CP2104 hang off VBUS and
  would corrupt the reading. With --usb manual the script asks to unplug it.
  The Monsoon output should be a charged LiPo voltage (--vout 4.0): the battery %
  decides how often the watchface updates.

Usage (from firmware/, the IDF environment is sourced automatically):
  ./test.py list
  ./test.py selftest                       # no hardware: models, scripts, analysis
  ./test.py build [--variant trace power release]
  ./test.py run --suite trace              # every trace test
  ./test.py run -k ntp light               # tests whose name contains ntp or light
  ./test.py run --suite power --monsoon hvpm --idle-minutes 5
  ./test.py run --suite power --save-baseline   # record the power numbers to compare
  ./test.py table [latest]                 # result table of a run again
  ./test.py compare [old] [new]            # what changed between two runs, biggest % first
  ./test.py compare 20261108-101500 latest --metric floor_uA avg_uA --min-pct 5
  ./test.py analyze build/test/results/<run>/<test>/power   # recompute a capture
  ./test.py erase-script                   # clear the script sector afterwards

A run ends with a table: result, time taken and the power figures of every test
(idle floor, average, wakes/min, wake length and charge, peak; trace tests show
the awake time per app wake and per wake stub instead, they can't measure current).
summary.json keeps it with the git commit, so `compare` shows what a code change did:
every metric that moved, sorted by the size of the change in %, either way.

Results go to build/test/results/<date>/: summary.json, and per test the raw trace
(open it with tools/lightink_trace.py --replay), decoded events, refresh frames,
power capture (power.f32 + .json, 10 ms summary .csv) and result.json.

The power limits and feature signatures below are first estimates: run the power
suite once with --save-baseline, check the numbers, then tune LIMITS.
"""

import argparse
import array
import datetime
import json
import os
import shutil
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field

FW = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(FW, "tools"))
import lightink_trace as lt  # noqa: E402

TARGET = "esp32"
SCRIPT_OFFSET = 0x3FF000  # CONFIG_LIGHTINK_SCRIPT_FLASH_OFFSET, one 4 KB sector
SECTOR = 4096
MAX_SCRIPT = 160  # kMaxScript in trace.cpp
USB_IDS = {(0x10C4, 0xEA60)}  # CP2104 on the watch
BUILD = os.path.join(FW, "build", "test")
BASELINE = os.path.join(FW, "tools", "power_baseline.json")
GOLDEN = os.path.join(FW, "tools", "test_golden")

# Images. Every test image reads its script from flash; "release" is what users flash
# Every image keeps the settings out of flash: the model starts from the defaults, and
# release has to idle with the same settings as power to be compared with it
VARIANTS = {
    "trace": ["CONFIG_LIGHTINK_TRACE=y", "CONFIG_LIGHTINK_SCRIPT_FLASH=y",
              f"CONFIG_LIGHTINK_SCRIPT_FLASH_OFFSET=0x{SCRIPT_OFFSET:X}", "CONFIG_LIGHTINK_FAKE_SIGNALS=y",
              "CONFIG_LIGHTINK_NO_STORAGE=y"],
    "power": ["CONFIG_LIGHTINK_SCRIPT=y", "CONFIG_LIGHTINK_SCRIPT_FLASH=y",
              f"CONFIG_LIGHTINK_SCRIPT_FLASH_OFFSET=0x{SCRIPT_OFFSET:X}", "CONFIG_LIGHTINK_FAKE_SIGNALS=y",
              "CONFIG_LIGHTINK_NO_STORAGE=y"],
    "release": ["CONFIG_LIGHTINK_NO_STORAGE=y"],
}

# Power limits, first estimates (README: ~2 uA idle). Tune after the first Monsoon run
LIMITS = {
    "floor_uA": 10.0,       # Median deep sleep current between wakes
    "avg_uA": 60.0,         # Mean over the idle window, minute wake stub included
    "max_burst_ms": 3000,   # Longest awake burst while idle (a full wake + refresh)
    "wakes_per_min": (0.5, 2.5),
    "baseline_tol": 0.25,   # Allowed rise over the baseline (relative)
    "baseline_abs_uA": 1.0, # ... plus this much, for the noise on tiny numbers
}
BURST_MA = 0.3     # Above this the watch is awake (or something is on)
MERGE_MS = 100     # Bursts closer than this are one wake

FLAG = {"Display": 0x01, "Light": 0x02, "Speaker": 0x04, "GPS": 0x08, "Wifi": 0x10, "BT": 0x20, "Lora": 0x40}
PANIC = ("Guru Meditation", "abort() was called", "Backtrace:", "panic'ed", "Rebooting...",
         "assert failed", "Stack canary", "stack overflow", "Brownout", "rst:0x")


def log(msg=""):
    print(msg, flush=True)


# ---------------------------------------------------------------------------
# Menu model: mirrors menus.cpp and the navigation of ui.cpp / Core::handleTouch
# ---------------------------------------------------------------------------

@dataclass
class Node:
    kind: str  # menu action bool loop range number datetime label
    name: str
    items: list = field(default_factory=list)  # menu children
    key: str = ""        # settings key for bool/loop/range
    size: int = 0        # loop size / range (lo, hi)
    lo: int = 0
    hi: int = 0
    act: str = ""        # action id, see Model.action()
    forbidden: str = ""  # why a script must not run it
    hidden: bool = False  # UI::Optional whose condition is false: keeps its index, skipped

    @property
    def captures(self):  # capture_input(): MENU on it goes one level deeper
        return self.kind in ("menu", "range", "number", "datetime")


def M_(name, *items):
    return Node("menu", name, list(items))


def A_(name, act="", forbidden="", hidden=False):
    return Node("action", name, act=act or name, forbidden=forbidden, hidden=hidden)


def B_(name, key):
    return Node("bool", name, key=key)


def L_(name, key, size):
    return Node("loop", name, key=key, size=size)


def R_(name, key, lo, hi):
    return Node("range", name, key=key, lo=lo, hi=hi)


DEFAULTS = {
    "beep": False, "sound": 0, "vib": False, "first": 8, "last": 22,
    "wf_type": 0, "wf_debug": False, "wf_battery": False, "wf_moon": False, "wf_sun": False, "wf_tides": False,
    "invert": False, "border": False, "flip": True, "menu_lut": 1, "watch_lut": 1,
    "night": False, "auto": True, "low_voltage": True,
    "haptic": False, "touch_beep": False, "sensitivity": 60, "menu_rate": 0, "watch_rate": 2,
    "ble_every": 3,
    "utc": 1,
}
SOUNDS = 7      # Beep::Sound (beep.h)
SYNC_EVERY = 5  # SyncEvery (ble.h): Off 5min 15min 1h 6h
# Fake signals (main/test_signals.cpp)
TEST_SIGNALS = [("Test signals", ["Garbage OOK", "Garbage PWM"])]


def build_menus(m):
    """The menu tree as generateMenus() builds it for the current settings."""
    alarms = []
    if not all(m.alarms):
        alarms.append(A_("Add Alarm", "alarm_add"))
    alarms += [A_(f"Alarm {i}", f"alarm_del:{i}") for i, on in enumerate(m.alarms) if on]
    return M_("Main Menu",
        M_("Clock",
            Node("datetime", "Set DateTime"),
            Node("number", "UTC", key="utc"),
            M_("Calibration", A_("Sync", "cal"), A_("Reset", "cal"), Node("label", "Info")),
            # The entry under Beep is only there while it is on. The watch shows the name
            # of the sound on it, here it has a fixed one to have a path
            M_("Hour Beep", B_("Beep", "beep"), A_("Sound", "sound", hidden=not m.s["beep"]),
               B_("Vibrate", "vib"),
               R_("First Hour", "first", 0, 24), R_("Last Hour", "last", 0, 24)),
            M_("Alarms", *alarms),
            A_("NTP", "ntp"),
            A_("GPS", "gps")),
        M_("Watchface", L_("Style", "wf_type", 2), B_("DEBUG MODE", "wf_debug"),
           B_("Show Battery %", "wf_battery"), B_("Moon Phases", "wf_moon"),
           B_("Sunset/Sunrise", "wf_sun"), B_("Tides", "wf_tides")),
        M_("Display", B_("Invert", "invert"), B_("Border", "border"), B_("Flip 180", "flip"),
           L_("Menu Lut", "menu_lut", 4), L_("Watch Lut", "watch_lut", 4)),
        M_("Power", B_("Night (0-6am)", "night"), B_("Auto (<25%)", "auto"), B_("Low Voltage", "low_voltage")),
        M_("Touch", B_("Haptic", "haptic"), B_("Beep", "touch_beep"),
           R_("Sensitivity", "sensitivity", 40, 80),
           L_("Menu Rate", "menu_rate", 4), L_("Watch Rate", "watch_rate", 4)),
        # No phone is ever paired in a test: "Pair phone", not "Forget phone"
        M_("Bluetooth", A_("Pair phone", "ble_pair"), A_("Sync now", "ble_sync"),
           L_("Auto", "ble_every", SYNC_EVERY), Node("label", "Status")),
        M_("Radio", *[M_(g, *[A_(n, "radio") for n in names]) for g, names in TEST_SIGNALS]),
        M_("Test",
           A_("Vib 2x75ms", "vib2"), A_("Vib 200ms", "vib200"), A_("Beep", "beep"),
           A_("Tetris Sound", "tetris"), A_("Lora Start Rcv", "lora_rx"),
           A_("Light toggle", "light"),
           A_("Display Restore", forbidden="loops until a real touch, scripts can't stop it"),
           A_("Parallel All", "parallel")),
        M_("Test2", Node("label", "Battery V")),
    )


@dataclass
class Step:
    key: str            # M U D B .
    depth: int          # ui.mDepth after the wake
    state: list         # ui.mState after the wake
    menu: bool          # focused element is a menu (state[depth] is its selection)
    effects: list       # what the wake must show in the trace
    light: bool
    listening: bool
    low_voltage: bool
    note: str = ""
    chain: tuple = ()   # menus entered (names), for the golden frames


class Model:
    """Watch state that the script changes, starting from a cold boot."""

    def __init__(self, wifi=True):
        self.depth = -1
        self.state = [0, 0, 0, 0]
        self.watchdog = False
        self.popup = False
        self.light = False
        self.listening = False
        self.gps = False  # After the GPS flush prefix (see Nav.flush_gps)
        self.alarms = [True] * 5
        self.s = dict(DEFAULTS)
        self.wifi = wifi

    def root(self):
        return build_menus(self)

    def current(self):
        """findUi(): the focused element."""
        item = self.root()
        for i in range(self.depth):
            idx = self.state[i]
            if item.kind != "menu" or idx >= len(item.items):
                break
            item = item.items[idx]
        return item

    def chain(self):
        """Names of the menus entered below the main menu."""
        item, names = self.root(), []
        for i in range(self.depth):
            idx = self.state[i]
            if item.kind != "menu" or idx >= len(item.items):
                break
            item = item.items[idx]
            names.append(item.name)
        return names

    def index(self, menu):
        """Sub::index(): the selected entry, or the next one that is shown if it is hidden."""
        size = len(menu.items)
        raw = self.state[self.depth]
        ind = raw if raw < size else 0  # curIndex()
        for _ in range(size):
            if not menu.items[ind].hidden:
                break
            ind = (ind + 1) % size
        return ind

    def step(self, menu, ind, add):
        """Sub::button_updown(): the next shown entry from ind, down (+1) or up (-1)."""
        size = len(menu.items)
        for _ in range(size):
            ind = (ind + size + add) % size
            if not menu.items[ind].hidden:
                break
        return ind

    def action(self, act, fx):
        if act == "vib2":
            fx.append(("vibrator", [75, 75, 75]))
        elif act == "vib200":
            fx.append(("vibrator", [200]))
        elif act == "beep":
            fx.append(("speaker", None))  # Beep::preview(): the hour sound, some tell the hour
        elif act == "sound":
            self.s["sound"] = (self.s["sound"] + 1) % SOUNDS
            fx.append(("speaker", None))  # The new sound is played
        elif act == "ble_sync":
            self.popup = self.depth >= 0  # Not paired: no radio, just the message
        elif act == "ble_pair":
            # Scans for the app in pairing mode (60 s), then "Phone not found"
            fx.append(("lock", FLAG["BT"]))
            self.popup = self.depth >= 0
        elif act == "tetris":
            fx.append(("speaker", None))
        elif act == "light":
            self.light = not self.light
        elif act == "lora_rx":
            self.listening = True
        elif act == "radio":
            fx.append(("radio", 433.92))
        elif act == "ntp":
            fx.append(("wifi", self.wifi))
            self.popup = self.depth >= 0
        elif act == "gps":
            # Only "GPS resync" is scripted (after Nav.flush_gps): a second press races the
            # wake's background GPS task, which may turn it off first. It gives up by itself
            self.gps = True
            fx.append(("lock", FLAG["GPS"]))
        elif act == "parallel":
            fx += [("vibrator", [70]), ("speaker", None)]
            self.light = False  # Light::off() at the end
        elif act == "alarm_add":
            self.alarms[self.alarms.index(False)] = True
        elif act.startswith("alarm_del:"):
            self.alarms[int(act.split(":")[1])] = False

    def press(self, key):
        """One scripted wake. Returns the effects the trace must show."""
        fx = []
        if key == ".":  # Timer wake: the watchdog returns to the watchface
            if self.watchdog:
                self.depth = -1
                self.popup = False
            self.watchdog = True
        else:
            if self.s["haptic"]:
                fx.append(("vibrator", [25]))
            # Button beep (Touch/Beep): on the menus, and for the press that enters them
            if self.s["touch_beep"] and (self.depth >= 0 or key == "M"):
                fx.append(("speaker", [4096, 20]))
            self.watchdog = False
            self._touch(key, fx)
        if self.depth < 0:
            self.light = False  # The watchface draw turns it off
        return fx

    def _touch(self, key, fx):
        if self.popup:  # Swallows the press, on the watchface as well
            self.popup = False
            return
        if self.depth < 0:
            if key == "B":
                fx.append(("light_flash",))
            elif key == "M":
                self.depth = 0
            return
        item = self.current()
        if key == "B":
            self.depth -= 1
        elif item.kind == "menu":
            size = len(item.items)
            if key == "M":
                if not size:
                    return
                idx = self.index(item)
                child = item.items[idx]
                if child.hidden:  # All of it is hidden
                    return
                if child.forbidden:
                    raise ValueError(f"{child.name}: {child.forbidden}")
                if child.captures:
                    self.state[self.depth] = idx
                    self.depth += 1
                elif child.kind == "bool":
                    self.s[child.key] = not self.s[child.key]
                elif child.kind == "loop":
                    self.s[child.key] = (self.s[child.key] + 1) % child.size
                elif child.kind == "action":
                    self.action(child.act, fx)
            else:
                self.state[self.depth] = self.step(item, self.index(item), 1 if key == "D" else -1)
        elif item.kind == "range" and key in "UD":
            v = self.s[item.key] + (1 if key == "U" else -1)
            self.s[item.key] = item.lo if v > item.hi else item.hi if v < item.lo else v
        elif item.kind == "number" and key in "UD":
            self.s[item.key] = max(-12, min(12, self.s[item.key] + (1 if key == "U" else -1)))


class Nav:
    """Builds a touch script and the expected outcome of every step."""

    def __init__(self, wifi=True):
        self.m = Model(wifi)
        self.steps = []

    @property
    def script(self):
        return "".join(s.key for s in self.steps)

    def press(self, keys, note=""):
        for k in keys.replace(" ", ""):
            fx = self.m.press(k)
            m = self.m
            self.steps.append(Step(k, m.depth, list(m.state), m.depth >= 0 and m.current().kind == "menu",
                                   fx, m.light, m.listening, m.s["low_voltage"], note, tuple(m.chain())))
        return self

    def idle(self, n=1, note="idle"):
        return self.press("." * n, note)

    def flush_gps(self):
        """Idle wakes until the first boot GPS search has given up (6 x 10 s of GPS time)."""
        return self.idle(7, "GPS flush")

    def dismiss(self):
        if self.m.popup:
            self.press("B", "dismiss popup")
        return self

    def home(self):
        self.dismiss()
        while self.m.depth >= 0:
            self.press("B", "home")
        return self

    def _move(self, name):
        menu = self.m.current()
        names = [n.name for n in menu.items if not n.hidden]
        if name not in names:
            raise ValueError(f"{name!r} not in {menu.name}: {names}")
        target, size = [n.name for n in menu.items].index(name), len(menu.items)
        # The raw index may be stale from another menu at this depth, or on a hidden
        # entry: the watch moves from the one it shows selected
        start = self.m.index(menu)
        if start == target:
            return
        best = None
        for key, add in (("D", 1), ("U", -1)):
            v = start
            for n in range(1, size + 1):
                v = self.m.step(menu, v, add)
                if v == target:
                    if best is None or n < best[1]:
                        best = (key, n)
                    break
        self.press(best[0] * best[1], f"to {name}")

    def resolve(self, path):
        """Path to names, against the tree: some names contain '/' (Sunset/Sunrise)."""
        node, rest, parts = self.m.root(), path, []
        while rest:
            names = [c.name for c in node.items if not c.hidden] if node.kind == "menu" else []
            hits = [n for n in names if rest == n or rest.startswith(n + "/")]
            if not hits:
                raise ValueError(f"{rest!r} not in {node.name}: {names}")
            name = max(hits, key=len)
            parts.append(name)
            node = next(c for c in node.items if c.name == name)
            rest = rest[len(name) + 1:]
        return parts

    def go(self, path):
        """Focus the item at path ("Clock/NTP"), entering menus as needed."""
        parts = self.resolve(path)
        self.dismiss()
        if self.m.depth < 0:
            self.press("M", "open menu")
        if self.m.current().kind != "menu":
            self.press("B", "leave input")
        chain, want = self.m.chain(), parts[:-1]
        k = 0
        while k < min(len(chain), len(want)) and chain[k] == want[k]:
            k += 1
        for _ in range(len(chain) - k):
            self.press("B", "up")
        for name in want[k:]:
            self._move(name)
            self.press("M", f"enter {name}")
        self._move(parts[-1])
        return self

    def select(self, path, times=1):
        """Focus the item and press MENU on it (run, toggle, cycle or enter)."""
        self.go(path)
        for _ in range(times):
            self.press("M", path)
        return self


# ---------------------------------------------------------------------------
# Test catalog
# ---------------------------------------------------------------------------

@dataclass
class Test:
    name: str
    suite: str               # trace | power
    doc: str
    nav: Nav
    variant: str = ""        # default: the suite's image
    step_ms: int = 1000
    stubs: int = 0           # trace: wake stub runs to wait for after the script
    tail_s: float = 3        # trace: listen this long after the end
    extra_s: float = 0       # time the actions take beyond a normal wake
    slow: bool = False
    golden: bool = False     # trace: compare menu frames with tools/test_golden/<name>/
    # Menus whose frames change between runs: UTC and GPS state (Clock), battery (Test2)
    golden_skip: tuple = (("Clock",), ("Test2",))
    allow: tuple = ()        # trace: sleep warnings that are expected here
    # power
    floor_mA: tuple = None   # expected (lo, hi) idle floor instead of LIMITS
    signatures: list = field(default_factory=list)  # (label, mA, ms): sustained in the script
    compare: str = ""        # name of a test whose idle numbers this one must match
    idle_minutes: float = 0  # override --idle-minutes

    @property
    def image(self):
        return self.variant or self.suite


def walk_all(nav):
    """Enter every menu and step over every item, then back out."""
    def visit(path, node):
        shown = [c for c in node.items if not c.hidden]
        nav.go(f"{path}/{shown[0].name}" if path else shown[0].name)
        for _ in shown:
            nav.press("D", f"walk {node.name}")
        for child in shown:
            if child.kind == "menu" and child.items:
                nav.select(f"{path}/{child.name}" if path else child.name)
                visit(f"{path}/{child.name}" if path else child.name, child)
                nav.press("B", "up")
    root = nav.m.root()
    for top in root.items:
        nav.select(top.name)
        visit(top.name, top)
        nav.press("B", "up")
    return nav.home()


def tests(wifi=True):
    N = lambda: Nav(wifi)  # noqa: E731
    out = []

    def T(name, doc, nav, **kw):
        out.append(Test(name, "trace", doc, nav, **kw))

    def P(name, doc, nav, **kw):
        out.append(Test(name, "power", doc, nav, **kw))

    # Trace suite ----------------------------------------------------------
    T("boot", "Cold boot: first boot (NTP, GPS) then the wake stub redraws every minute",
      N(), stubs=2)
    T("menu_walk", "Every menu and item, the focus checked at each step; stub resumes after",
      walk_all(N()), stubs=1, golden=True)
    T("watchdog", "Two timer wakes in a menu return to the watchface",
      N().go("Display/Border").idle(2))
    T("watchface_light", "BACK on the watchface flashes the light during the redraw",
      N().press("B").idle().press("B"))
    T("light", "Test/Light toggle keeps the LED on across sleeps; the watchface turns it off",
      N().select("Test/Light toggle").idle().press("M", "toggle off").press("M", "toggle on").home())
    T("vibrator", "Test/Vib patterns reach the motor", N().select("Test/Vib 2x75ms").select("Test/Vib 200ms").home())
    T("speaker", "Test/Beep plays and releases the speaker rail", N().select("Test/Beep").home())
    T("tetris", "Test/Tetris Sound plays to the end and removes its popup",
      N().select("Test/Tetris Sound").home(), extra_s=60, slow=True)
    T("ntp", "Clock/NTP: WiFi connects, NTP syncs, WiFi is off before sleeping, popup",
      N().select("Clock/NTP").dismiss().select("Clock/NTP").idle(2), extra_s=20)
    T("gps", "Clock/GPS resync locks the GPS rail, the search gives up and releases it",
      N().flush_gps().select("Clock/GPS").home().idle(7, "GPS search"))
    T("lora_tx", "Radio sends the garbage OOK and PWM bursts at 433.92 MHz",
      N().select("Radio/Test signals/Garbage OOK").select("Radio/Test signals/Garbage PWM").home())
    T("lora_rx", "Test/Lora Start Rcv keeps the SX1262 listening (DIO1 wake) through sleeps",
      N().select("Test/Lora Start Rcv").home().idle())
    nav = N()
    for item in ("Invert", "Border", "Flip 180"):
        nav.select(f"Display/{item}").home().idle(note="watchface").select(f"Display/{item}")
    for lut in ("Menu Lut", "Watch Lut"):
        for _ in range(4):
            nav.select(f"Display/{lut}").home().idle(note="watchface")
    T("settings_display", "Invert, border, flip and every LUT, each drawn on the watchface", nav.home())
    nav = N()
    for _ in range(2):
        nav.select("Watchface/Style").home().idle(note="watchface")
    for item in ("DEBUG MODE", "Show Battery %", "Moon Phases", "Sunset/Sunrise", "Tides"):
        nav.select(f"Watchface/{item}").home().idle(note="watchface").select(f"Watchface/{item}")
    T("settings_watchface", "Both watchface styles and every watchface option, drawn", nav.home())
    nav = N().select("Power/Night (0-6am)", 2).select("Power/Auto (<25%)", 2)
    nav.select("Power/Low Voltage").home().idle().select("Power/Low Voltage").home().idle()
    T("settings_power", "Night/Auto toggles; Low Voltage off keeps the rail high in sleep",
      nav, allow=("rail high without locks",))
    nav = N().select("Touch/Haptic").select("Touch/Beep")
    nav.select("Touch/Sensitivity").press("UU").press("DD").press("B")
    nav.select("Touch/Menu Rate", 4).select("Touch/Watch Rate", 4)
    nav.select("Touch/Haptic").select("Touch/Beep").home()
    T("settings_touch", "Haptic vibrates and Beep ticks on every touch; sensitivity and rates cycle", nav)
    nav = N().select("Clock/UTC").press("U").press("D").press("B")
    nav.select("Clock/Calibration").select("Clock/Calibration/Sync").select("Clock/Calibration/Reset")
    nav.select("Clock/Calibration/Info")
    nav.select("Clock/Hour Beep/Beep")      # On: the sound entry appears under it
    nav.select("Clock/Hour Beep/Sound", 2)  # Each press takes the next sound and plays it
    nav.select("Clock/Hour Beep/Beep")      # Off: hidden again, DOWN goes over it to Vibrate
    nav.select("Clock/Hour Beep/Vibrate", 2)
    for item in ("First Hour", "Last Hour"):
        nav.select(f"Clock/Hour Beep/{item}").press("U").press("D").press("B")
    T("settings_clock", "UTC, calibration actions, the hourly beep settings and its sound entry",
      nav.home(), extra_s=10)
    T("ble_sync", "Bluetooth/Sync now with no phone paired: the radio stays off, popup",
      N().select("Bluetooth/Sync now").home())
    T("ble_auto", "Bluetooth/Auto through every interval: with no phone paired no wake syncs",
      N().select("Bluetooth/Auto", SYNC_EVERY).home().idle(2))
    T("ble_pair", "Bluetooth/Pair phone looks for the app for 60 s, gives up with the radio off, popup",
      N().select("Bluetooth/Pair phone").home(), extra_s=70, slow=True)
    T("alarms", "Removing an alarm rebuilds the menu with Add Alarm, adding it back",
      N().select("Clock/Alarms/Alarm 0").select("Clock/Alarms/Add Alarm").home())
    T("parallel", "Test/Parallel All: vibrator, light and speaker together, light off at the end",
      N().select("Test/Parallel All").home(), extra_s=60, slow=True)

    # Power suite ----------------------------------------------------------
    P("idle", "Watchface idle: deep sleep floor and the minute wake stub", N(), idle_minutes=5)
    P("release_idle", "Same on the release image: the test hooks must not change idle power",
      N(), variant="release", compare="idle", idle_minutes=5)
    P("menu_walk", "After walking every menu the watch idles like after a cold boot",
      walk_all(N().flush_gps()), compare="idle")
    P("light", "LED on draws mA in sleep, off again on the watchface",
      N().flush_gps().select("Test/Light toggle").idle().home(), compare="idle",
      signatures=[("LED on in sleep", 0.5, 800)])
    P("vibrator", "Vib 200ms burst, nothing left on", N().flush_gps().select("Test/Vib 200ms").home(),
      compare="idle", signatures=[("motor on", 15, 150)])
    P("speaker", "Beep burst, speaker rail released", N().flush_gps().select("Test/Beep").home(),
      compare="idle", signatures=[("speaker on", 5, 50)])  # Casio chime: two 62 ms tones
    P("ntp", "WiFi NTP sync burst, WiFi really off afterwards", N().flush_gps().select("Clock/NTP").home(),
      compare="idle", extra_s=20, signatures=[("WiFi on", 40, 500)])
    P("lora_tx", "Garbage OOK + PWM bursts, radio back to sleep",
      N().flush_gps().select("Radio/Test signals/Garbage OOK").select("Radio/Test signals/Garbage PWM").home(),
      compare="idle", signatures=[("TX at 22 dBm", 20, 10)])
    P("lora_rx", "LoRa listening in deep sleep (SX1262 ~4.6 mA)",
      N().flush_gps().select("Test/Lora Start Rcv").home(), floor_mA=(2.0, 8.0))
    P("gps", "GPS resync draws ~20 mA, gives up after 60 s of wakes, then idles",
      N().flush_gps().select("Clock/GPS").home(), compare="idle", extra_s=80,
      signatures=[("GPS on", 10, 2000)])
    P("low_voltage_off", "Low Voltage off: rail at 3.3 V in sleep (reported, compared by hand)",
      N().flush_gps().select("Power/Low Voltage").home(), floor_mA=(0.0, 0.5))
    P("touch_fast", "Watch Rate 250ms: faster touch sampling (reported)",
      N().flush_gps().select("Touch/Watch Rate", 2).home(), floor_mA=(0.0, 0.5))
    return out


# ---------------------------------------------------------------------------
# Tools: IDF environment, build, flash
# ---------------------------------------------------------------------------

_env = None


def idf_env():
    """os.environ with esp-idf/export.sh sourced (IDF_CCACHE_ENABLE kept)."""
    global _env
    if _env is None:
        clean = {k: v for k, v in os.environ.items() if not k.startswith("IDF_") or k == "IDF_CCACHE_ENABLE"}
        out = subprocess.run(["bash", "-c", ". ./esp-idf/export.sh > /dev/null 2>&1 && env -0"],
                             cwd=FW, env=clean, capture_output=True)
        if out.returncode:
            sys.exit("could not source esp-idf/export.sh (run ./setup.sh once)")
        _env = dict(kv.split("=", 1) for kv in out.stdout.decode().split("\0") if "=" in kv)
    return _env


def run(cmd, cwd=FW, quiet=False):
    p = subprocess.run(cmd, cwd=cwd, env=idf_env(), capture_output=True, text=True)
    if p.returncode:
        log(p.stdout[-4000:] + p.stderr[-4000:])
        raise RuntimeError(f"failed: {' '.join(cmd)}")
    if not quiet:
        log(p.stdout[-400:].rstrip())
    return p.stdout


def build(variant):
    d = os.path.join(BUILD, variant)
    os.makedirs(d, exist_ok=True)
    defaults = os.path.join(BUILD, f"{variant}.defaults")
    text = "".join(line + "\n" for line in VARIANTS[variant])
    old = open(defaults).read() if os.path.exists(defaults) else None
    if old != text:
        with open(defaults, "w") as f:
            f.write(text)
        # Defaults only fill unset symbols: start the sdkconfig over when they change
        if os.path.exists(os.path.join(d, "sdkconfig")):
            os.remove(os.path.join(d, "sdkconfig"))
    log(f"build {variant} -> {os.path.relpath(d, FW)}")
    run(["idf.py", "-B", d, "-D", f"IDF_TARGET={TARGET}", "-D", f"SDKCONFIG={d}/sdkconfig",
         "-D", f"SDKCONFIG_DEFAULTS={FW}/sdkconfig.defaults;{defaults}", "build"], quiet=True)
    return d


_esptool5 = None


def esptool(port, *args, after="hard-reset", baud=460800):
    global _esptool5
    py = shutil.which("python", path=idf_env()["PATH"]) or sys.executable
    if _esptool5 is None:
        v = run([py, "-m", "esptool", "version"], quiet=True)
        _esptool5 = not any(line.strip().startswith(("v4.", "4.")) for line in v.splitlines())
    if not _esptool5:  # v4 spells the commands and options with underscores
        args = tuple(a.replace("-", "_") if a in ("write-flash", "erase-region") else a for a in args)
        after = after.replace("-", "_")
    return run([py, "-m", "esptool", "--chip", TARGET, "-p", port, "-b", str(baud),
                "--after", after, *args], cwd=os.getcwd(), quiet=True)


def flash_image(variant, port):
    d = os.path.join(BUILD, variant)
    log(f"flash {variant} on {port}")
    args = open(os.path.join(d, "flash_args")).read().split()
    old = os.getcwd()
    os.chdir(d)  # flash_args paths are relative to the build dir
    try:
        esptool(port, "write-flash", *args, after="no-reset")
    finally:
        os.chdir(old)


def script_blob(script, step_ms):
    steps = script.encode()
    if len(steps) > MAX_SCRIPT:
        raise ValueError(f"script has {len(steps)} steps, the firmware keeps {MAX_SCRIPT}")
    blob = b"LIS1" + struct.pack("<HB", step_ms, len(steps)) + steps
    return blob + b"\xff" * (SECTOR - len(blob))


def write_script(port, script, step_ms=1000):
    path = os.path.join(BUILD, "script.bin")
    os.makedirs(BUILD, exist_ok=True)
    with open(path, "wb") as f:
        f.write(script_blob(script, step_ms))
    esptool(port, "write-flash", f"0x{SCRIPT_OFFSET:X}", path, after="no-reset")


def find_port(want, timeout=10):
    from serial.tools import list_ports
    end = time.time() + timeout
    while True:
        ports = list(list_ports.comports())
        if want and want != "auto" and any(p.device == want for p in ports):
            return want
        for p in ports:
            if (p.vid, p.pid) in USB_IDS:
                return p.device
        if time.time() > end:
            raise RuntimeError(f"watch serial port not found ({want or 'auto'})")
        time.sleep(0.5)


# ---------------------------------------------------------------------------
# Trace capture and checks
# ---------------------------------------------------------------------------

def capture_trace(port, test, outdir, script):
    """Cold boot with the script in flash, decode the trace until the test is done."""
    raw = os.path.join(outdir, "trace.bin")
    n = len(script)
    # First boot (NTP, up to 60 s of GPS wakes without WiFi), steps, stub runs a minute apart
    timeout = 90 + n * (test.step_ms / 1000 + 3) + test.extra_s + test.stubs * 75 + test.tail_s
    tl = lt.Timeline()
    src = lt.SerialSource(port, 115200, True, raw)  # RTS pulse: boot what we just wrote
    start, done_at, timed_out = time.time(), None, False
    shown = start
    try:
        while True:
            if time.time() - shown > 20:
                shown = time.time()
                step = max((e.info.get("step", 0) for e in tl.events if e.kind == "wake"), default=0)
                log(f"  {shown - start:4.0f} s: {len(tl.groups)} wakes, step {step}/{n}, "
                    f"{stub_count(tl, 0)} stub runs")
            for text in src.link_events():
                tl.link(text)
            d = src.read()
            if d:
                tl.feed(d)
            elif time.time() - tl.last_rx > 0.3:
                tl.close_open()
            if done_at is None and trace_done(tl, test, n):
                done_at = time.time()
            if done_at and time.time() - done_at >= test.tail_s:
                break
            if time.time() - start > timeout:
                timed_out = True
                break
            if not d:
                time.sleep(0.02)
    finally:
        src.close()
    tl.close_open()
    return tl, timed_out


def stub_count(tl, after):
    return sum(1 for g in tl.groups if g > after and tl.events[g].kind == "stub")


def trace_done(tl, test, n):
    if n:
        ends = [e.idx for e in tl.events if e.kind == "script_end"]
        if not ends or not any(e.kind == "sleep" for e in tl.events[ends[0]:]):
            return False
        return stub_count(tl, ends[0]) >= test.stubs
    sleeps = [e.idx for e in tl.events if e.kind == "sleep"]
    return bool(sleeps) and stub_count(tl, sleeps[0]) >= test.stubs


@dataclass
class Result:
    name: str
    fails: list = field(default_factory=list)
    warns: list = field(default_factory=list)
    notes: list = field(default_factory=list)
    metrics: dict = field(default_factory=dict)
    skipped: str = ""

    def fail(self, msg):
        self.fails.append(msg)

    def warn(self, msg):
        self.warns.append(msg)

    @property
    def status(self):
        return "SKIP" if self.skipped else "FAIL" if self.fails else "WARN" if self.warns else "PASS"


def wakes(tl):
    """(first event, events, sleep event or None) per wake."""
    out = []
    for g in range(len(tl.groups)):
        evs = tl.group_events(g)
        out.append((evs[0], evs, next((e for e in evs if e.kind == "sleep"), None)))
    return out


def check_effect(fx, evs, sleep):
    kind = fx[0]
    periph = [e.info for e in evs if e.kind == "periph"]
    power = [e.info for e in evs if e.kind == "power"]
    titles = [e.title for e in evs]
    if kind in ("vibrator", "speaker"):
        hits = [p for p in periph if p.get("kind") == kind]
        if fx[1] is not None:
            hits = [p for p in hits if p["pattern"] == fx[1]]
        if not hits:
            return f"no {kind} pattern {fx[1] or ''}".strip()
    elif kind == "radio":
        if not any(p.get("kind") == "radio" and abs(p["mhz"] - fx[1]) < 0.02 for p in periph):
            return f"no radio burst at {fx[1]} MHz"
    elif kind == "wifi":
        need = ["WIFI START", "WIFI STOP"] + (["WIFI CONNECTED", "WIFI GOT IP", "NTP SYNCED"] if fx[1] else [])
        missing = [t for t in need if t not in titles]
        if missing:
            return f"WiFi events missing: {', '.join(missing)}"
        if titles.index("WIFI STOP") < max(titles.index(t) for t in need if t != "WIFI STOP"):
            return "WiFi stopped before the sync finished"
    elif kind in ("lock", "unlock"):
        if not any(p["flag"] == fx[1] and bool(p["locked"]) == (kind == "lock") for p in power):
            return f"no {kind.upper()} {lt.flags_name(fx[1])}"
    elif kind == "light_flash":
        if not any(p["flag"] == FLAG["Light"] and p["locked"] for p in power):
            return "light never turned on"
        if sleep and sleep.info.get("mask", 0) & FLAG["Light"]:
            return "light still on at sleep"
    return None


def check_trace(test, tl, timed_out, res, wifi, golden_mode, outdir):
    steps = test.nav.steps
    if timed_out:
        res.fail("timed out before the test finished")
    for e in tl.events:
        if e.kind == "log" and any(p in e.detail for p in PANIC):
            res.fail(f"crash in log: {e.detail.strip()[:120]}")
    bad = sum(1 for e in tl.events if e.kind == "bad")
    if bad:
        res.warn(f"{bad} corrupt trace frame(s)")
    ws = wakes(tl)
    if not ws:
        res.fail("no trace received (trace image flashed? port right?)")
        return
    # A cold boot after the first sleep is a crash or a brownout (opening the port may
    # reset the chip once more before that)
    first_sleep = next((e.idx for e in tl.events if e.kind == "sleep"), None)
    reboots = [w for w in ws if w[0].kind == "wake" and w[0].info.get("cause") == "UNDEFINED"
               and first_sleep is not None and w[0].idx > first_sleep]
    if reboots:
        res.fail(f"{len(reboots)} unexpected reboot(s)")

    # Every finished app wake ends in a sleep, menu wakes put a frame on the panel
    for i, (head, evs, sleep) in enumerate(ws):
        last = i == len(ws) - 1
        if head.kind == "wake" and not sleep and not last:
            res.fail(f"wake {i} ({head.title} {head.detail}) never reached sleep")
        if not sleep:
            continue
        info = sleep.info
        frames = sum(1 for e in evs if e.kind == "cmd" and e.frame is not None)
        if info.get("depth", -1) >= 0 and frames == 0:
            res.fail(f"wake {i}: menu wake without a display refresh")
        for w in info.get("warnings", []):
            if w.startswith("locks held"):
                continue  # Checked below on the mask
            if w.startswith("trace UART stalled"):
                res.warn(f"wake {i}: {w}")
            elif not any(a in w for a in test.allow):
                res.fail(f"wake {i}: {w}")
        held = info.get("mask", 0) & ~(FLAG["Light"] | FLAG["GPS"])
        if held:
            res.fail(f"wake {i}: sleeping with {lt.flags_name(held)} locked")

    # The scripted steps against the model
    scripted = {w[0].info["step"]: w for w in ws if w[0].kind == "wake" and w[0].info.get("scripted")}
    if steps:
        if not any(e.kind == "script_end" for e in tl.events):
            res.fail("script never ended")
        if len(scripted) != len(steps):
            res.fail(f"{len(scripted)} scripted wakes, script has {len(steps)} steps")
    for n, st in enumerate(steps, 1):
        if n not in scripted:
            continue
        head, evs, sleep = scripted[n]
        where = f"step {n} '{st.key}' ({st.note})"
        if not sleep:
            continue
        info = sleep.info
        if "state" in info:
            got_depth, got_state = info["depth"], info["state"]
            k = max(0, st.depth + (1 if st.menu else 0))  # Indexes that mean something
            if got_depth != st.depth or got_state[:k] != st.state[:k]:
                res.fail(f"{where}: ui depth {got_depth} {got_state[:k]}, model {st.depth} {st.state[:k]}")
                break  # The rest of the script is off course
        if bool(info.get("mask", 0) & FLAG["Light"]) != st.light:
            res.fail(f"{where}: light {'on' if not st.light else 'off'}, expected {'on' if st.light else 'off'}")
        if "listening" in info and info["listening"] != st.listening:
            res.fail(f"{where}: LoRa listening {info['listening']}, expected {st.listening}")
        if "rail" in info and not info.get("mask") and info["rail"] != (not st.low_voltage):
            res.fail(f"{where}: rail {'high' if info['rail'] else 'low'} in sleep with Low Voltage "
                     f"{'on' if st.low_voltage else 'off'}")
        for fx in st.effects:
            err = check_effect(fx, evs, sleep)
            if err:
                res.fail(f"{where}: {err}")
        # No phone is paired in a test: only pairing may turn the radio on
        if ("lock", FLAG["BT"]) not in st.effects and any(
                e.kind == "power" and e.info["flag"] == FLAG["BT"] and e.info["locked"] for e in evs):
            res.fail(f"{where}: Bluetooth on with no phone paired")

    # Wake stub after the script, on the watchface
    if test.stubs:
        ends = [e.idx for e in tl.events if e.kind in ("script_end",)] or \
               [next((e.idx for e in tl.events if e.kind == "sleep"), 0)]
        stubs = [w for w in ws if w[0].kind == "stub" and w[0].idx > ends[0]]
        if len(stubs) < test.stubs:
            res.fail(f"{len(stubs)} wake stub run(s), expected {test.stubs}")
        elif not any(e.kind == "cmd" and e.frame is not None for w in stubs for e in w[1]):
            res.fail("the wake stub never refreshed the display")
    final = next((w[2] for w in reversed(ws) if w[2]), None)
    if final:
        if final.info.get("mask", 0) & FLAG["GPS"]:
            res.fail("GPS still on at the last sleep")
        res.metrics["battery_mV"] = final.info.get("battery")
    stats = tl.awake_stats()
    for kind, (avg, n) in stats.items():
        res.metrics[f"awake_{'app' if kind == 'wake' else 'stub'}_ms"] = round(avg / 1000, 2)
    res.metrics["wakes"] = len(ws)
    res.metrics["frames"] = len(tl.frames)

    # Frames: always saved, compared with the golden ones for menu tests
    fdir = os.path.join(outdir, "frames")
    os.makedirs(fdir, exist_ok=True)
    pngs = {}
    for fr in tl.frames:
        pngs[fr.n] = lt.frame_png(fr, 180)
        with open(os.path.join(fdir, f"{fr.n:03d}.png"), "wb") as f:
            f.write(pngs[fr.n])
    # Last refresh of each scripted menu step, in step order (the watchface shows the time)
    menu_frames = []
    for n, st in enumerate(steps, 1):
        if n not in scripted or st.depth < 0 or st.chain in test.golden_skip:
            continue
        frs = [fr for fr in tl.frames if fr.group == scripted[n][0].group]
        if frs:
            menu_frames.append(pngs[frs[-1].n])
    if test.golden and golden_mode != "off":
        compare_golden(test, menu_frames, golden_mode, res)


def compare_golden(test, frames, mode, res):
    d = os.path.join(GOLDEN, test.name)
    if mode == "update":
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        for i, png in enumerate(frames, 1):
            with open(os.path.join(d, f"{i:03d}.png"), "wb") as f:
                f.write(png)
        res.notes.append(f"golden frames updated ({len(frames)})")
        return
    if not os.path.isdir(d):
        res.notes.append("no golden frames yet (--golden update)")
        return
    gold = sorted(f for f in os.listdir(d) if f.endswith(".png"))
    if len(gold) != len(frames):
        res.fail(f"{len(frames)} menu frames, golden has {len(gold)}")
    for name, png in zip(gold, frames):
        _, _, want = lt.png_decode_bits(open(os.path.join(d, name), "rb").read())
        _, _, got = lt.png_decode_bits(png)
        diff = sum(1 for a, b in zip(want, got) if a != b)
        if diff:
            res.fail(f"menu frame {name}: {diff} px differ from golden")


def dump_events(tl, path):
    with open(path, "w") as f:
        for ev in tl.events:
            t = f"{ev.at / 1000:9.1f}ms" if ev.at is not None else f"{ev.t:9.3f}s "
            if ev.kind in ("wake", "stub"):
                f.write(f"\n[{t}] ==== {ev.title} {ev.detail}\n")
            elif ev.kind == "cmd":
                if ev.frame is not None:
                    f.write(f"[{t}]   {ev.detail}  -> frame {ev.frame + 1:03d}\n")
            elif ev.kind == "log":
                f.write(f"[{t}]   | {ev.detail}\n")
            else:
                f.write(f"[{t}]   {ev.title} {ev.detail}\n")


# ---------------------------------------------------------------------------
# Power: Monsoon capture and analysis
# ---------------------------------------------------------------------------

class Capture:
    """Main channel current (mA) at a fixed rate, with named marks (sample index)."""

    def __init__(self, samples=None, rate=5000.0, marks=None, meta=None):
        self.x = samples if samples is not None else array.array("f")
        self.rate = rate
        self.marks = marks or {}
        self.meta = meta or {}

    def t(self, name):
        return self.marks[name] / self.rate

    def save(self, prefix):
        with open(prefix + ".f32", "wb") as f:
            self.x.tofile(f)
        with open(prefix + ".json", "w") as f:
            json.dump({"rate": self.rate, "marks": self.marks, "meta": self.meta, "unit": "mA"}, f, indent=1)
        step = max(1, int(self.rate / 100))  # 10 ms summary for plotting
        with open(prefix + "_10ms.csv", "w") as f:
            f.write("t_s,mean_mA,max_mA\n")
            for i in range(0, len(self.x), step):
                c = self.x[i:i + step]
                f.write(f"{i / self.rate:.2f},{sum(c) / len(c):.5f},{max(c):.4f}\n")

    @classmethod
    def load(cls, prefix):
        with open(prefix + ".json") as f:
            meta = json.load(f)
        x = array.array("f")
        with open(prefix + ".f32", "rb") as f:
            x.frombytes(f.read())
        return cls(x, meta["rate"], meta["marks"], meta.get("meta"))


class Monsoon:
    RATE = 5000  # HVPM and LVPM sample rate

    def __init__(self, model, serial, vout, limit_a):
        import importlib
        try:
            pm = importlib.import_module(f"Monsoon.{model.upper()}")
            op = importlib.import_module("Monsoon.Operations")
            se = importlib.import_module("Monsoon.sampleEngine")
        except ImportError:
            sys.exit("Monsoon library missing: pip install monsoon")
        self.se, self.op, self.vout = se, op, vout
        self.mon = pm.Monsoon()
        self.mon.setup_usb(serial)
        for setter in ("setPowerUpCurrentLimit", "setRunTimeCurrentLimit"):
            if hasattr(self.mon, setter):
                getattr(self.mon, setter)(limit_a)
        self.mon.setVout(vout)
        self.engine = se.SampleEngine(self.mon)
        self.engine.ConsoleOutput(False)
        self.engine.enableChannel(se.channels.MainCurrent)
        self.thread = None

    def usb(self, on):
        P = self.op.USB_Passthrough
        self.mon.setUSBPassthroughMode(P.On if on else P.Off)

    def power_cycle(self, off_s=1.5):
        """Cold boot: every RTC memory gone, like a new battery."""
        try:
            self.mon.setVout(0)
        except Exception as e:  # Some firmware refuses 0 V: the lowest the HVPM sets
            log(f"  setVout(0) refused ({e}), using 0.8 V")
            self.mon.setVout(0.8)
        time.sleep(off_s)
        self.mon.setVout(self.vout)

    def start(self):
        self.cap = Capture(rate=self.RATE, meta={"vout": self.vout})
        self.stop_flag = threading.Event()
        self.error = None
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        ch = self.se.channels.MainCurrent
        try:
            first = self.engine.periodicStartSampling()
            if first is False:
                raise RuntimeError("Monsoon did not start sampling")
            self.cap.x.extend(first[ch])
            t0 = time.monotonic()
            while not self.stop_flag.is_set():
                self.cap.x.extend(self.engine.periodicCollectSamples(1000)[ch])
            self.cap.meta["wall_s"] = time.monotonic() - t0
            self.engine.periodicStopSampling()
        except Exception as e:  # Reported by stop()
            self.error = e

    def mark(self, name):
        self.cap.marks[name] = len(self.cap.x)

    def stop(self):
        self.stop_flag.set()
        self.thread.join(timeout=10)
        if self.error:
            raise self.error
        wall = self.cap.meta.get("wall_s")
        if wall:
            measured = len(self.cap.x) / wall
            self.cap.meta["measured_rate"] = round(measured, 1)
            if abs(measured / self.RATE - 1) > 0.1:
                self.cap.rate = measured  # Dropped samples: use the real rate for times
        return self.cap

    def close(self):
        try:
            self.usb(True)
            self.mon.closeDevice()
        except Exception:
            pass


@dataclass
class Burst:
    start: int
    end: int
    peak: float
    charge_uC: float


def bursts(x, rate, lo, hi, thr=BURST_MA, merge_ms=MERGE_MS):
    merge = int(rate * merge_ms / 1000)
    out, start, last, peak, q = [], None, None, 0.0, 0.0
    for i in range(lo, hi):
        v = x[i]
        if v > thr:
            if start is None or i - last > merge:
                if start is not None:
                    out.append(Burst(start, last + 1, peak, q * 1000 / rate))
                start, peak, q = i, 0.0, 0.0
            last = i
            peak = max(peak, v)
        if start is not None:
            q += v  # Charge includes the merged gaps
    if start is not None:
        out.append(Burst(start, last + 1, peak, q * 1000 / rate))
    return out


def window_stats(cap, t0, t1):
    x, r = cap.x, cap.rate
    lo, hi = max(0, int(t0 * r)), min(len(x), int(t1 * r))
    if hi - lo < r:
        return None
    bs = bursts(x, r, lo, hi)
    guard = int(r * 0.02)
    quiet, i = [], lo
    for b in bs + [Burst(hi, hi, 0, 0)]:
        quiet.extend(x[i:max(i, b.start - guard)])
        i = b.end + guard
    quiet.sort()
    minutes = (hi - lo) / r / 60
    ms = sorted((b.end - b.start) * 1000 / r for b in bs)
    qs = sorted(b.charge_uC for b in bs)
    return {
        "window_s": round((hi - lo) / r, 1),
        "floor_uA": round(quiet[len(quiet) // 2] * 1000, 3) if quiet else None,
        "avg_uA": round(sum(x[lo:hi]) / (hi - lo) * 1000, 3),
        "wakes": len(bs),
        "wakes_per_min": round(len(bs) / minutes, 2),
        "wake_ms_median": round(ms[len(ms) // 2], 2) if ms else None,
        "wake_uC_median": round(qs[len(qs) // 2], 2) if qs else None,
        "max_burst_ms": round(ms[-1], 1) if ms else 0,
        "max_mA": round(max(x[lo:hi]), 3),
    }


def sustained_ms(cap, t0, t1, min_ma):
    """Longest stretch at or above min_ma between t0 and t1 (gaps < 2 ms tolerated)."""
    x, r = cap.x, cap.rate
    gap = max(1, int(r * 0.002))
    best, run_start, last = 0, 0, -10**9
    for i in range(max(0, int(t0 * r)), min(len(x), int(t1 * r))):
        if x[i] >= min_ma:
            if i - last > gap:
                run_start = i
            last = i
            best = max(best, i - run_start + 1)
    return best * 1000 / r


def check_power(test, cap, res, baseline, results):
    idle = window_stats(cap, cap.t("idle"), cap.t("end"))
    if not idle:
        res.fail("idle window too short")
        return
    res.metrics.update(idle)
    script = window_stats(cap, cap.t("boot"), cap.t("idle"))
    if script:
        res.metrics["script_max_mA"] = script["max_mA"]
        res.metrics["script_avg_uA"] = script["avg_uA"]
    floor = idle["floor_uA"]
    if test.floor_mA:
        lo, hi = test.floor_mA
        if not lo * 1000 <= floor <= hi * 1000:
            res.fail(f"idle floor {floor:.1f} uA, expected {lo}-{hi} mA")
    else:
        if floor > LIMITS["floor_uA"]:
            res.fail(f"idle floor {floor:.2f} uA > {LIMITS['floor_uA']} uA")
        if idle["avg_uA"] > LIMITS["avg_uA"]:
            res.fail(f"idle average {idle['avg_uA']:.2f} uA > {LIMITS['avg_uA']} uA")
        if idle["max_burst_ms"] > LIMITS["max_burst_ms"]:
            res.fail(f"{idle['max_burst_ms']:.0f} ms awake while idle: something stays on")
        lo, hi = LIMITS["wakes_per_min"]
        if not lo <= idle["wakes_per_min"] <= hi:
            res.warn(f"{idle['wakes_per_min']} wakes/min while idle (watchface steps every minute "
                     f"at a full battery, check --vout)")
    for label, ma, ms in test.signatures:
        got = sustained_ms(cap, cap.t("boot"), cap.t("idle"), ma)
        res.metrics[f"sig_{label.replace(' ', '_')}_ms"] = round(got, 1)  # Time at >= ma mA
        if got < ms:
            res.warn(f"signature '{label}' not seen: {got:.0f} ms at >= {ma} mA, expected {ms} ms "
                     "(estimate, tune after a run)")
    # Not above the reference test (nothing left on) nor above its own baseline (regression)
    refs = []
    if test.compare:
        run_ref = results.get(f"power/{test.compare}", {})
        if run_ref.get("status") in ("PASS", "WARN") and run_ref.get("metrics"):
            refs.append((f"'{test.compare}' in this run", run_ref["metrics"]))
        elif test.compare in baseline:
            refs.append((f"'{test.compare}' baseline", baseline[test.compare]))
    if test.name in baseline:
        refs.append(("own baseline", baseline[test.name]))
    for key in ("floor_uA", "avg_uA"):
        for label, ref in refs:
            base = ref.get(key)
            if base is None:
                continue
            allowed = base * (1 + LIMITS["baseline_tol"]) + LIMITS["baseline_abs_uA"]
            if idle[key] > allowed:
                res.fail(f"{key} {idle[key]:.2f} uA above {label} {base:.2f} uA "
                         f"(+{LIMITS['baseline_tol']:.0%} + {LIMITS['baseline_abs_uA']} uA allowed)")


def capture_power(mon, test, args, port, script):
    n = len(script)
    script_s = 25 + n * (test.step_ms / 1000 + 2) + test.extra_s
    idle_s = 60 * (test.idle_minutes or args.idle_minutes)
    if args.usb == "monsoon":
        mon.usb(False)
    else:
        input("  unplug the watch USB, then press Enter ")
    mon.power_cycle()
    mon.start()
    mon.mark("boot")
    log(f"  script ~{script_s:.0f} s, settle {args.settle:.0f} s, idle {idle_s:.0f} s")
    time.sleep(script_s)
    mon.mark("script_end")
    time.sleep(args.settle)
    mon.mark("idle")
    time.sleep(idle_s)
    mon.mark("end")
    cap = mon.stop()
    if args.usb == "monsoon":
        mon.usb(True)
    else:
        input("  plug the watch USB back, then press Enter ")
    find_port(port, timeout=15)
    return cap


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def select(args, all_tests):
    picked = [t for t in all_tests if args.suite in ("all", t.suite)]
    if args.k:
        picked = [t for t in picked if any(k in f"{t.suite}/{t.name}" for k in args.k)]
    elif not args.slow:
        picked = [t for t in picked if not t.slow]
    return picked


def cmd_list(args):
    for t in tests():
        est = 40 + len(t.nav.script) * 2.5 + t.extra_s + t.stubs * 65
        if t.suite == "power":
            est += 60 * (t.idle_minutes or 3) + 90
        flags = (" slow" if t.slow else "") + (" golden" if t.golden else "")
        log(f"{t.suite:5} {t.name:20} {t.image:7} {len(t.nav.script):3} steps ~{est / 60:4.1f} min{flags}  {t.doc}")


def cmd_build(args):
    for v in args.variant:
        build(v)


def cmd_run(args):
    all_tests = tests(wifi=not args.no_wifi)
    picked = select(args, all_tests)
    if not picked:
        sys.exit("no test selected")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    root = os.path.join(BUILD, "results", stamp)
    os.makedirs(root)
    log(f"{len(picked)} test(s), results in {os.path.relpath(root, FW)}")

    mon = None
    if any(t.suite == "power" for t in picked) and args.monsoon != "none":
        mon = Monsoon(args.monsoon, args.monsoon_serial, args.vout, args.current_limit)
        mon.usb(True)
    images = sorted({t.image for t in picked if t.suite == "trace" or mon})
    if not args.no_build:
        for v in images:
            build(v)
    port = find_port(args.port)
    baseline = json.load(open(BASELINE)) if os.path.exists(BASELINE) else {}
    results, flashed = {}, None
    info = run_info(args)

    def save_summary():  # After every test: an interrupted run can still be compared
        with open(os.path.join(root, "summary.json"), "w") as f:
            json.dump({"run": info, "tests": results}, f, indent=1)

    try:
        for t in picked:
            started = time.time()
            res = Result(f"{t.suite}/{t.name}")
            out = os.path.join(root, f"{t.suite}_{t.name}")
            os.makedirs(out)
            log(f"\n=== {res.name}: {t.doc}")
            log(f"  script ({len(t.nav.script)}): {t.nav.script or '(none)'}")
            if t.suite == "power" and not mon:
                res.skipped = "no Monsoon (--monsoon hvpm|lvpm)"
            else:
                try:
                    port = find_port(port)
                    if t.image != flashed and not args.no_flash:
                        flash_image(t.image, port)
                        flashed = t.image
                    if t.image == "release":
                        write_script(port, "")  # Ignored by the release image, keeps the sector clean
                    else:
                        write_script(port, t.nav.script, t.step_ms)
                    if t.suite == "trace":
                        tl, timed_out = capture_trace(port, t, out, t.nav.script)
                        dump_events(tl, os.path.join(out, "events.txt"))
                        check_trace(t, tl, timed_out, res, not args.no_wifi, args.golden, out)
                    else:
                        cap = capture_power(mon, t, args, port, t.nav.script)
                        cap.save(os.path.join(out, "power"))
                        check_power(t, cap, res, baseline, results)
                except KeyboardInterrupt:
                    raise
                except Exception as e:
                    res.fail(f"{e.__class__.__name__}: {e}")
            if not res.skipped:
                res.metrics["duration_s"] = round(time.time() - started, 1)
            results[res.name] = dataclass_dict(res)
            with open(os.path.join(out, "result.json"), "w") as f:
                json.dump(dataclass_dict(res) | {"script": t.nav.script,
                          "steps": [s.__dict__ for s in t.nav.steps]}, f, indent=1, default=str)
            save_summary()
            report(res)
    finally:
        if mon:
            mon.close()
        info["duration_s"] = round(sum(r["metrics"].get("duration_s", 0) for r in results.values()), 1)
        save_summary()
    log("")
    print_table({"run": info, "tests": results})
    log(f"\nresults: {os.path.relpath(root, FW)}   compare with: ./test.py compare <older run> {stamp}")
    if args.save_baseline:
        base = baseline | {k.removeprefix("power/"): v["metrics"] for k, v in results.items()
                           if k.startswith("power/") and v["status"] != "SKIP" and v["metrics"]}
        with open(BASELINE, "w") as f:
            json.dump(base, f, indent=1, sort_keys=True)
        log(f"baseline saved to {os.path.relpath(BASELINE, FW)}")
    if args.erase:
        write_script(find_port(port), "")
    return 1 if any(r["status"] == "FAIL" for r in results.values()) else 0


def run_info(args):
    """What was tested: the code version matters most when comparing runs."""
    def git(*a):
        p = subprocess.run(["git", "-C", FW, *a], capture_output=True, text=True)
        return p.stdout.strip() if p.returncode == 0 else "?"
    return {
        "date": datetime.datetime.now().isoformat(timespec="seconds"),
        "commit": git("rev-parse", "--short", "HEAD"),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": bool(git("status", "--porcelain", "--untracked-files=no", "--", "main")),
        "subject": git("log", "-1", "--format=%s"),
        "suite": args.suite, "monsoon": args.monsoon, "vout": args.vout,
        "wifi": not args.no_wifi, "idle_minutes": args.idle_minutes,
    }


# Table columns: (header, metric, format). Trace tests can't measure current, the
# awake time per wake is their power figure
COLUMNS = [
    ("Time", "duration_s", lambda v: f"{v / 60:.1f}m" if v >= 60 else f"{v:.0f}s"),
    ("Floor uA", "floor_uA", "{:.2f}"),
    ("Avg uA", "avg_uA", "{:.2f}"),
    ("Wakes/min", "wakes_per_min", "{:.2f}"),
    ("Wake ms", "wake_ms_median", "{:.1f}"),
    ("Wake uC", "wake_uC_median", "{:.1f}"),
    ("Peak mA", "script_max_mA", "{:.1f}"),
    ("App awake ms", "awake_app_ms", "{:.0f}"),
    ("Stub awake ms", "awake_stub_ms", "{:.2f}"),
]


def fmt(v, f):
    if v is None or isinstance(v, bool) or not isinstance(v, (int, float)):
        return "-"
    return f(v) if callable(f) else f.format(v)


def print_table(summary, out=log):
    tests_ = summary["tests"]
    run_ = summary.get("run", {})
    cols = [c for c in COLUMNS if any(c[1] in r["metrics"] for r in tests_.values())]
    header = ["Test", "Result"] + [c[0] for c in cols] + ["Issues"]
    rows = []
    for r in tests_.values():
        issues = []
        if r["fails"]:
            issues.append(f"{len(r['fails'])} fail")
        if r["warns"]:
            issues.append(f"{len(r['warns'])} warn")
        if r["skipped"]:
            issues.append(r["skipped"])
        rows.append([r["name"], r["status"]] + [fmt(r["metrics"].get(c[1]), c[2]) for c in cols]
                    + [", ".join(issues)])
    counts = {s: sum(1 for r in tests_.values() if r["status"] == s) for s in ("PASS", "WARN", "FAIL", "SKIP")}
    total = run_.get("duration_s") or sum(r["metrics"].get("duration_s", 0) for r in tests_.values())
    rows.append(["total", " ".join(f"{n} {s}" for s, n in counts.items() if n), fmt(total, COLUMNS[0][2])]
                + [""] * (len(header) - 3))
    widths = [max(len(str(x[i])) for x in [header] + rows) for i in range(len(header))]
    numeric = range(2, 2 + len(cols))

    def line(cells):
        return "  ".join(str(c).rjust(w) if i in numeric else str(c).ljust(w)
                         for i, (c, w) in enumerate(zip(cells, widths))).rstrip()
    if run_:
        out(f"run {run_.get('date', '?')}  {run_.get('branch', '?')} {run_.get('commit', '?')}"
            f"{' +changes' if run_.get('dirty') else ''}  {run_.get('subject', '')}")
    out(line(header))
    out("  ".join("-" * w for w in widths))
    for row in rows[:-1]:
        out(line(row))
    out("  ".join("-" * w for w in widths))
    out(line(rows[-1]))
    for r in tests_.values():
        for m in r["fails"]:
            out(f"  FAIL {r['name']}: {m}")


def runs_dir():
    d = os.path.join(BUILD, "results")
    return sorted(x for x in os.listdir(d) if os.path.exists(os.path.join(d, x, "summary.json"))) \
        if os.path.isdir(d) else []


def load_run(ref):
    """A run by directory, summary.json, run name (date) or latest / previous."""
    runs = runs_dir()
    if ref in ("latest", "previous"):
        if len(runs) < (1 if ref == "latest" else 2):
            sys.exit(f"not enough runs in {os.path.relpath(os.path.join(BUILD, 'results'), FW)}")
        ref = runs[-1 if ref == "latest" else -2]
    path = ref
    for cand in (ref, os.path.join(BUILD, "results", ref)):
        if os.path.isdir(cand):
            path = os.path.join(cand, "summary.json")
            break
    if not os.path.exists(path):
        sys.exit(f"no run {ref!r} (runs: {', '.join(runs[-5:]) or 'none'})")
    with open(path) as f:
        data = json.load(f)
    if "tests" not in data:  # Bare {name: result}
        data = {"run": {}, "tests": data}
    data["run"].setdefault("name", os.path.basename(os.path.dirname(os.path.abspath(path))))
    return data


def compare_runs(a, b, min_pct=0.0, only=None):
    """Rows (test, metric, old, new, pct) sorted by the size of the change, and status changes."""
    rows, status = [], []
    for name in sorted(set(a["tests"]) | set(b["tests"])):
        ra, rb = a["tests"].get(name), b["tests"].get(name)
        if not ra or not rb:
            status.append((name, ra["status"] if ra else "-", rb["status"] if rb else "-"))
            continue
        if ra["status"] != rb["status"]:
            status.append((name, ra["status"], rb["status"]))
        for key in sorted(set(ra["metrics"]) & set(rb["metrics"])):
            va, vb = ra["metrics"][key], rb["metrics"][key]
            if only and not any(o in key for o in only):
                continue
            if isinstance(va, bool) or isinstance(vb, bool) or not isinstance(va, (int, float)) \
                    or not isinstance(vb, (int, float)) or va == vb:
                continue
            pct = (vb - va) / abs(va) * 100 if va else float("inf") if vb > 0 else float("-inf")
            if abs(pct) >= min_pct:
                rows.append((name, key, va, vb, pct))
    rows.sort(key=lambda r: -abs(r[4]))
    return rows, status


def cmd_table(args):
    print_table(load_run(args.run))


def cmd_compare(args):
    a, b = load_run(args.old), load_run(args.new)
    for tag, r in (("old", a["run"]), ("new", b["run"])):
        log(f"{tag}: {r.get('name')}  {r.get('branch', '?')} {r.get('commit', '?')}"
            f"{' +changes' if r.get('dirty') else ''}  {r.get('subject', '')}")
    rows, status = compare_runs(a, b, args.min_pct, args.metric)
    if status:
        log("\nresult changes:")
        for name, sa, sb in status:
            log(f"  {name:28} {sa:>4} -> {sb}")
    if not rows:
        log(f"\nno metric changed by {args.min_pct}% or more")
        return 0
    log(f"\n{len(rows)} metric change(s), biggest first (+ = higher in the new run):")
    head = ("Test", "Metric", "Old", "New", "Change", "%")
    cells = [head] + [(n, k, f"{va:g}", f"{vb:g}", f"{vb - va:+.4g}",
                       "new" if abs(p) == float("inf") else f"{p:+.1f}%") for n, k, va, vb, p in rows]
    widths = [max(len(c[i]) for c in cells) for i in range(len(head))]
    for i, c in enumerate(cells):
        log("  ".join(x.ljust(w) if j < 2 else x.rjust(w) for j, (x, w) in enumerate(zip(c, widths))))
        if i == 0:
            log("  ".join("-" * w for w in widths))
    return 0


def dataclass_dict(res):
    return {"name": res.name, "status": res.status, "fails": res.fails, "warns": res.warns,
            "notes": res.notes, "metrics": res.metrics, "skipped": res.skipped}


def report(res):
    log(f"  {res.status}" + (f" ({res.skipped})" if res.skipped else ""))
    for m in res.fails:
        log(f"    FAIL {m}")
    for m in res.warns:
        log(f"    warn {m}")
    for m in res.notes:
        log(f"    note {m}")
    if res.metrics:
        log("    " + ", ".join(f"{k}={v}" for k, v in res.metrics.items()))


def cmd_analyze(args):
    cap = Capture.load(args.capture.removesuffix(".json").removesuffix(".f32"))
    name = os.path.basename(os.path.dirname(os.path.abspath(args.capture))).split("_", 1)[-1]
    test = next((t for t in tests() if t.suite == "power" and t.name == name), None)
    if test is None:
        test = Test(name, "power", "", Nav())
    res = Result(f"power/{name}")
    baseline = json.load(open(BASELINE)) if os.path.exists(BASELINE) else {}
    check_power(test, cap, res, baseline, {})
    report(res)
    bs = bursts(cap.x, cap.rate, 0, len(cap.x))
    log(f"\n{len(bs)} bursts over {len(cap.x) / cap.rate:.0f} s:")
    for b in bs[:200]:
        log(f"  {b.start / cap.rate:8.3f} s  {(b.end - b.start) * 1000 / cap.rate:8.1f} ms  "
            f"peak {b.peak:7.2f} mA  {b.charge_uC:9.1f} uC")


def cmd_erase(args):
    write_script(find_port(args.port), "")
    log("script sector cleared")


def cmd_selftest(args):
    ok = True

    def expect(name, got, want):
        nonlocal ok
        good = got == want
        ok &= good
        log(f"  {'ok  ' if good else 'FAIL'} {name}: {got!r}" + ("" if good else f" (want {want!r})"))

    log("navigation (paths measured on the watch, see the trace notes):")
    expect("Clock/NTP", Nav().select("Clock/NTP").script, "MMUUM")
    expect("Test/Tetris (shortest)", Nav().select("Test/Tetris Sound").script, "MUUMDDDM")
    n = Nav().press("MDDDDDDDMDDDM")  # The long way down, Test is the 8th menu
    expect("Test/Tetris (long path)", n.steps[-1].effects, [("speaker", None)])
    n = Nav().select("Clock/NTP")
    expect("popup after NTP", n.m.popup, True)
    expect("dismiss then back", n.dismiss().home().script, "MMUUMBBB")
    n = Nav().go("Display/Border").idle(2)
    expect("watchdog", (n.m.depth, n.script), (-1, "MDDMD.."))
    n = Nav().select("Clock/Alarms/Alarm 0")
    expect("alarm removed", n.m.alarms[0], False)
    expect("add alarm item", [i.name for i in n.m.current().items][0], "Add Alarm")
    n.select("Clock/Alarms/Add Alarm")
    expect("alarm back", n.m.alarms, [True] * 5)
    n = Nav().select("Test/Light toggle").home()
    expect("light off on the watchface", n.steps[-1].light, False)
    n = Nav().go("Clock/Hour Beep/Vibrate")
    expect("hidden entry skipped", (n.script, n.m.state[2]), ("MMDDDMD", 2))
    n = Nav().select("Clock/Hour Beep/Beep").select("Clock/Hour Beep/Sound")
    expect("sound entry with Beep on", (n.script, n.m.s["sound"], n.steps[-1].effects),
           ("MMDDDMMDM", 1, [("speaker", None)]))
    n = Nav().select("Touch/Beep").press("D")
    expect("button beep from the next press", [s.effects for s in n.steps[-2:]], [[], [("speaker", [4096, 20])]])
    n = Nav().select("Bluetooth/Sync now")
    expect("Sync now unpaired", (n.m.popup, n.steps[-1].effects), (True, []))
    try:
        Nav().select("Test/Display Restore")
        expect("forbidden item", "allowed", "refused")
    except ValueError:
        expect("forbidden item", "refused", "refused")
    for t in tests():
        if len(t.nav.script) > MAX_SCRIPT:
            expect(f"{t.name} script length", len(t.nav.script), f"<= {MAX_SCRIPT}")
    expect("all scripts fit", max(len(t.nav.script) for t in tests()) <= MAX_SCRIPT, True)

    log("script sector:")
    blob = script_blob("MMUUM", 1500)
    expect("header", blob[:7], b"LIS1" + struct.pack("<HB", 1500, 5))
    expect("size", len(blob), SECTOR)

    log("power analysis on a synthetic capture (2 uA floor, 1 ms 20 mA stub/min, 400 ms 2 mA refresh):")
    r = 5000
    x = array.array("f", [0.002] * (r * 300))
    for m in range(5):
        s = int((30 + 60 * m) * r)
        for i in range(s, s + 5):
            x[i] = 20.0
        for i in range(s + 5, s + 5 + 2000):
            x[i] = 2.0
    cap = Capture(x, r, {"boot": 0, "idle": 10 * r, "end": 300 * r})
    st = window_stats(cap, 10, 300)
    expect("floor", st["floor_uA"], 2.0)
    expect("wakes", st["wakes"], 5)
    expect("wake ms", st["wake_ms_median"], 401.0)
    expect("signature", sustained_ms(cap, 0, 300, 1.5), 401.0)
    res = Result("power/idle")
    check_power(Test("idle", "power", "", Nav()), cap, res, {}, {})
    expect("verdict", res.status, "PASS")

    log("trace checks on a synthetic trace:")
    tl = synthetic_trace()
    t = Test("synthetic", "trace", "", Nav().select("Test/Vib 200ms"))
    res = Result("trace/synthetic")
    os.makedirs(os.path.join(BUILD, "selftest"), exist_ok=True)
    check_trace(t, tl, False, res, True, "off", os.path.join(BUILD, "selftest"))
    expect("verdict", (res.status, res.fails), ("PASS", []))
    t = Test("synthetic", "trace", "", Nav().select("Test/Vib 200ms"))
    t.nav.steps[-1].effects = [("vibrator", [75, 75, 75])]
    res = Result("trace/synthetic")
    check_trace(t, tl, False, res, True, "off", os.path.join(BUILD, "selftest"))
    expect("wrong pattern caught", res.fails, [f"step {len(t.nav.steps)} 'M' (Test/Vib 200ms): "
                                               "no vibrator pattern [75, 75, 75]"])
    t = Test("synthetic", "trace", "", Nav().select("Test/Vib 200ms"))
    t.nav.steps[2].state[0] = 5
    res = Result("trace/synthetic")
    check_trace(t, tl, False, res, True, "off", os.path.join(BUILD, "selftest"))
    expect("wrong focus caught", [f.split(":")[0] for f in res.fails], ["step 3 'U' (to Test)"])
    log("table and compare:")

    def result(status, **metrics):
        return {"name": "", "status": status, "fails": ["x"] if status == "FAIL" else [], "warns": [],
                "notes": [], "metrics": metrics, "skipped": ""}
    a = {"run": {"commit": "aaa"}, "tests": {
        "power/idle": result("PASS", floor_uA=2.0, avg_uA=5.0, duration_s=400),
        "trace/ntp": result("PASS", awake_app_ms=900, duration_s=80)}}
    b = {"run": {"commit": "bbb"}, "tests": {
        "power/idle": result("PASS", floor_uA=3.0, avg_uA=5.04, duration_s=400),
        "trace/ntp": result("FAIL", awake_app_ms=600, duration_s=80)}}
    for d in (a, b):
        for k, v in d["tests"].items():
            v["name"] = k
    rows, status = compare_runs(a, b, 1.0)
    expect("sorted by |%|", [(r[1], round(r[4], 1)) for r in rows], [("floor_uA", 50.0), ("awake_app_ms", -33.3)])
    expect("status change", status, [("trace/ntp", "PASS", "FAIL")])
    lines = []
    print_table(b, out=lines.append)
    expect("table total", lines[-2].split()[:4], ["total", "1", "PASS", "1"])
    expect("table columns", lines[1].split()[:4], ["Test", "Result", "Time", "Floor"])
    log("PASS" if ok else "FAIL")
    return 0 if ok else 1


def synthetic_trace():
    """Frames as the firmware sends them for 'Test/Vib 200ms' (M U U M D M)."""
    nav = Nav().select("Test/Vib 200ms")
    out = bytearray()

    def frame(typ, payload):
        body = bytes([typ]) + struct.pack("<H", len(payload)) + payload
        s1 = s2 = 0
        for b in body:
            s1 = (s1 + b) % 255
            s2 = (s2 + s1) % 255
        out.extend(b"\x00\xa5\x5a" + body + bytes([s1, s2]))

    def wake(t, cause, scripted, btn, step, steps, depth, state, extra=()):
        frame(1, struct.pack("<IBBBBB", t, cause, scripted, btn, step, steps))
        for typ, p in extra:
            frame(typ, p)
        frame(7, struct.pack("<I", t + 100) + b"\x20")  # Refresh
        frame(5, struct.pack("<IBBbBBBBHB", t + 200, step < steps, step == steps and depth < 0, depth,
                             0, 0, 0xFF, 0xFF, 3900, 0) + bytes([0]) + bytes(state))

    keys = {"M": 4, "U": 2, "D": 1, "B": 8, ".": 0}
    wake(1000, 0, 0, 0, 0, 0, -1, [0, 0, 0, 0])
    for i, st in enumerate(nav.steps, 1):
        extra = []
        if st.effects:
            extra.append((9, struct.pack("<IBB", 5000, 1, 1) + struct.pack("<H", 200)))
        wake(1000 * i, 5, 1, keys[st.key], i, len(nav.steps), st.depth, st.state, extra)
    frame(6, struct.pack("<I", 99999))
    tl = lt.Timeline()
    tl.feed(bytes(out), now=0)
    tl.close_open()
    return tl


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n\n".join(__doc__.split("\n\n")[1:]))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="list the tests")
    sub.add_parser("selftest", help="check the models and the analysis, no hardware")
    b = sub.add_parser("build", help="build the test images")
    b.add_argument("--variant", nargs="+", default=list(VARIANTS), choices=list(VARIANTS))
    r = sub.add_parser("run", help="flash, run and check tests")
    r.add_argument("--suite", default="all", choices=("trace", "power", "all"))
    r.add_argument("-k", nargs="+", help="only tests whose suite/name contains one of these")
    r.add_argument("--slow", action="store_true", help="include the slow tests (tetris, parallel)")
    r.add_argument("--port", default="auto", help="watch serial port (default: find the CP2104)")
    r.add_argument("--no-build", action="store_true")
    r.add_argument("--no-flash", action="store_true", help="the right image is already on the watch")
    r.add_argument("--no-wifi", action="store_true", help="no known WiFi here: expect NTP to fail cleanly")
    r.add_argument("--golden", default="check", choices=("check", "update", "off"))
    r.add_argument("--monsoon", default="none", choices=("hvpm", "lvpm", "none"))
    r.add_argument("--monsoon-serial", type=int, default=None)
    r.add_argument("--vout", type=float, default=4.0, help="Monsoon output, a charged LiPo")
    r.add_argument("--current-limit", type=float, default=1.0, help="Monsoon current limit (A)")
    r.add_argument("--usb", default="monsoon", choices=("monsoon", "manual"),
                   help="how USB is cut for power tests: Monsoon passthrough or by hand")
    r.add_argument("--idle-minutes", type=float, default=3)
    r.add_argument("--settle", type=float, default=90, help="seconds after the script before idle")
    r.add_argument("--save-baseline", action="store_true", help=f"write {os.path.relpath(BASELINE, FW)}")
    r.add_argument("--erase", action="store_true", help="clear the script sector at the end")
    a = sub.add_parser("analyze", help="recompute the power checks of a saved capture")
    a.add_argument("capture", help=".../power (or power.json / power.f32)")
    e = sub.add_parser("erase-script", help="clear the script sector")
    e.add_argument("--port", default="auto")
    t = sub.add_parser("table", help="print the result table of a run")
    t.add_argument("run", nargs="?", default="latest", help="run dir/name, or latest / previous")
    c = sub.add_parser("compare", help="differences between two runs, biggest change first")
    c.add_argument("old", nargs="?", default="previous", help="run dir/name, or latest / previous")
    c.add_argument("new", nargs="?", default="latest")
    c.add_argument("--min-pct", type=float, default=1.0, help="hide changes smaller than this (%%)")
    c.add_argument("--metric", nargs="+", help="only metrics containing one of these (floor_uA avg_uA ...)")
    args = ap.parse_args()
    return {"list": cmd_list, "selftest": cmd_selftest, "build": cmd_build, "run": cmd_run,
            "analyze": cmd_analyze, "erase-script": cmd_erase, "table": cmd_table,
            "compare": cmd_compare}[args.cmd](args) or 0


if __name__ == "__main__":
    sys.exit(main())
