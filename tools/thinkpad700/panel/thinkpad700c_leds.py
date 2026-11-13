#!/usr/bin/env python3
"""Drive the ThinkPad 700C-style LED panel through the ESP32 controller."""

import argparse
import fcntl
import json
import logging
import math
import os
import random
import re
import select
import signal
import struct
import subprocess
import termios
import time
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import psutil


PIN_POWER_Z = 13
PIN_SLEEP = 27
PIN_SCROLL = 17
PIN_CAPS = 14
PIN_NUM = 22
PIN_HDD = 23
PIN_FLOPPY = 5
PIN_TRI_CHG = 18
PIN_BATT_Y = 19
PIN_BATT_G = 16
PIN_VOL = 15

ALL_PINS = [
    PIN_POWER_Z,
    PIN_SLEEP,
    PIN_SCROLL,
    PIN_CAPS,
    PIN_NUM,
    PIN_HDD,
    PIN_FLOPPY,
    PIN_TRI_CHG,
    PIN_BATT_Y,
    PIN_BATT_G,
    PIN_VOL,
]

DEFAULT_PORT = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
DEFAULT_UPS_STATUS = "/run/ups-monitor/status.json"
DEFAULT_SLIDER_STATUS = "/run/thinkpad700c-leds/slider-status.json"
DEFAULT_SWITCH_STATUS = "/run/thinkpad700c-leds/switch-status.json"
DEFAULT_EMULATOR_STATUS = "/run/user/1000/thinkpad700c-emulator/panel-status.json"
BUTTON_RE = re.compile(r"BTN MUTE=([01]) MAX=([01]) INVERT=([01]) VALID=([01])")
CTRL_RE = re.compile(
    r"CTRL B=(\d+) C=(\d+) RAW34=(\d+) RAW35=(\d+) MV34=(\d+) MV35=(\d+)"
)
BRIGHTNESS_MIN_MV = 142
BRIGHTNESS_MAX_MV = 3160
CONTRAST_MIN_MV = 3154
CONTRAST_MAX_MV = 142
DISPLAY_MINIMUM_PERCENT = 10
CONTRAST_GAMMA_MIN = 0.65
CONTRAST_GAMMA_MAX = 1.55


def lamp_to_gpio_level(lamp_on):
    return 0 if lamp_on else 1


def calibrated_percent(millivolts, minimum, maximum):
    value = round((millivolts - minimum) * 100 / (maximum - minimum))
    return max(0, min(100, value))


def contrast_gamma(percent):
    percent = max(0, min(100, percent))
    if percent <= 50:
        return CONTRAST_GAMMA_MIN + (1.0 - CONTRAST_GAMMA_MIN) * percent / 50
    return 1.0 + (CONTRAST_GAMMA_MAX - 1.0) * (percent - 50) / 50


def build_lamp_states(
    num_on,
    caps_on,
    scroll_on,
    hdd_on,
    vol_on,
    charge,
    network_on=False,
    power_on=False,
    floppy_on=None,
):
    tri_on, yellow_on, green_on = charge
    # Preserve the accepted Windows mapping: yellow follows tri_on.
    if floppy_on is None:
        floppy_on = network_on
    return {
        PIN_POWER_Z: power_on,
        PIN_SLEEP: False,
        PIN_NUM: num_on,
        PIN_CAPS: caps_on,
        PIN_SCROLL: scroll_on,
        PIN_HDD: hdd_on,
        PIN_VOL: vol_on,
        PIN_TRI_CHG: tri_on,
        PIN_BATT_Y: tri_on,
        PIN_BATT_G: green_on,
        PIN_FLOPPY: floppy_on,
    }, yellow_on


class EspLink:
    TIOCMBIS = 0x5416
    TIOCMBIC = 0x5417
    DTR = 0x002
    RTS = 0x004

    def __init__(self, port=DEFAULT_PORT):
        self.port = port
        self.fd = None
        self.last_sent = {}
        self.rx_buffer = bytearray()

    @property
    def connected(self):
        return self.fd is not None

    def _set_line(self, mask, enabled):
        request = self.TIOCMBIS if enabled else self.TIOCMBIC
        fcntl.ioctl(self.fd, request, struct.pack("I", mask))

    def _configure(self):
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
        attrs[3] = 0
        attrs[4] = termios.B115200
        attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 1
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def _reset_to_run_mode(self):
        self._set_line(self.DTR, False)
        self._set_line(self.RTS, False)
        time.sleep(0.1)
        self._set_line(self.RTS, True)
        time.sleep(0.15)
        self._set_line(self.RTS, False)

    def connect(self):
        self.close()
        self.fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            self._configure()
            self._reset_to_run_mode()
            self.last_sent.clear()
            self.rx_buffer.clear()
            time.sleep(0.6)
            lines = self.poll()
            logging.info("ESP32 connected on %s; startup=%s", self.port, lines)
            self.send_line("CTRL STREAM=1")
            self.send_line("BTN STREAM=1")
            self.send_line("VERSION?")
        except Exception:
            self.close()
            raise

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
        self.fd = None
        self.last_sent.clear()
        self.rx_buffer.clear()

    def send_line(self, command):
        if self.fd is None:
            raise OSError("ESP32 serial link is not connected")
        data = (command.strip() + "\n").encode("ascii")
        sent = 0
        try:
            while sent < len(data):
                sent += os.write(self.fd, data[sent:])
        except OSError:
            self.close()
            raise

    def heartbeat(self):
        self.send_line("HB")

    def set_pin_levels(self, levels, force=False):
        changed = {}
        for pin, value in levels.items():
            value = 1 if value else 0
            if force or self.last_sent.get(pin) != value:
                changed[pin] = value
        if not changed:
            return False
        payload = ",".join(f"{pin}={value}" for pin, value in changed.items())
        self.send_line("SET " + payload)
        self.last_sent.update(changed)
        return True

    def poll(self):
        if self.fd is None:
            return []
        lines = []
        try:
            while select.select([self.fd], [], [], 0)[0]:
                chunk = os.read(self.fd, 4096)
                if not chunk:
                    break
                self.rx_buffer.extend(chunk)
        except BlockingIOError:
            pass
        except OSError:
            self.close()
            raise

        while b"\n" in self.rx_buffer:
            raw, _, remainder = self.rx_buffer.partition(b"\n")
            self.rx_buffer = bytearray(remainder)
            line = raw.rstrip(b"\r").decode("utf-8", "replace")
            if line:
                lines.append(line)
        return lines


class NetworkSpeedBlinker:
    # RX + TX, decimal bytes/second. 1 KiB/s suppresses tiny idle keepalives.
    THRESHOLDS = (1024, 100_000, 1_000_000, 5_000_000)
    FREQUENCIES = (0, 1, 2, 4, 6)
    # Pulse count, inter-burst pause, and intra-burst dark interval by traffic band.
    BURSTS = {
        1: ((1, 2), (3.0, 6.0), (0.09, 0.18)),
        2: ((2, 4), (1.8, 3.5), (0.07, 0.15)),
        3: ((3, 6), (0.8, 1.8), (0.05, 0.12)),
        4: ((4, 8), (0.5, 1.2), (0.04, 0.09)),
    }

    def __init__(self):
        self.next_sample = 0.0
        self.next_interfaces = 0.0
        self.interfaces = set()
        self.previous = {}
        self.last_sample = None
        self.last_activity = float('-inf')
        self.rate_bps = 0.0
        self.band = 0
        self.rng = random.Random()
        self.lit = False
        self.burst_remaining = 0
        self.next_edge = 0.0
        self.previous_band = 0

    @property
    def frequency_hz(self):
        return self.FREQUENCIES[self.band]

    def _sample(self, now):
        try:
            if now >= self.next_interfaces:
                self.interfaces = {
                    name for name, stats in psutil.net_if_stats().items()
                    if stats.isup and (Path('/sys/class/net') / name / 'device').exists()
                }
                self.next_interfaces = now + 2.0
            counters = psutil.net_io_counters(pernic=True, nowrap=False)
            current = {name: (counters[name].bytes_recv, counters[name].bytes_sent)
                       for name in self.interfaces if name in counters}
        except (OSError, RuntimeError):
            self.previous = {}
            self.last_sample = None
            self.rate_bps = 0.0
            self.band = 0
            self.last_activity = float('-inf')
            return
        dt = now - self.last_sample if self.last_sample is not None else 0.0
        # Rebaseline new/reset interfaces and delayed samples without false bursts.
        delta = sum((rx - self.previous[name][0]) + (tx - self.previous[name][1])
                    for name, (rx, tx) in current.items()
                    if name in self.previous and rx >= self.previous[name][0]
                    and tx >= self.previous[name][1])
        self.previous = current
        self.last_sample = now
        if dt <= 0.0 or dt > 2.0 or not current:
            self.rate_bps = 0.0
            self.band = 0
            self.last_activity = float('-inf')
            return
        raw_rate = delta / dt
        self.rate_bps += (1.0 - math.exp(-dt / 0.5)) * (raw_rate - self.rate_bps)
        if raw_rate >= self.THRESHOLDS[0]:
            self.last_activity = now
        if now - self.last_activity > 1.0:
            self.band = 0
            self.rate_bps = 0.0
            return
        while self.band < 4:
            threshold = self.THRESHOLDS[self.band] * (1.15 if self.band else 1.0)
            if self.rate_bps < threshold:
                break
            self.band += 1
        while self.band > 0 and self.rate_bps < self.THRESHOLDS[self.band - 1] * 0.85:
            self.band -= 1

    def lamp_on(self, now):
        if now >= self.next_sample:
            self.next_sample = now + 0.2
            self._sample(now)
        if not self.band:
            self.lit = False
            self.burst_remaining = 0
            self.next_edge = now
            self.previous_band = 0
            return False
        counts, pause, gap = self.BURSTS[self.band]
        if self.band != self.previous_band and not self.lit:
            self.next_edge = min(self.next_edge, now + pause[1])
            self.burst_remaining = min(self.burst_remaining, counts[1])
        self.previous_band = self.band
        if now < self.next_edge:
            return self.lit
        # Schedule only one transition per frame; never replay missed flashes.
        if self.lit:
            self.lit = False
            self.next_edge = now + self.rng.uniform(*(gap if self.burst_remaining else pause))
        else:
            if not self.burst_remaining:
                self.burst_remaining = self.rng.randint(*counts)
            self.burst_remaining -= 1
            self.lit = True
            duration = self.rng.uniform(0.05, 0.18)
            if self.band >= 3 and self.rng.random() < 0.15:
                duration = self.rng.uniform(0.2, 0.5)
            self.next_edge = now + duration
        return self.lit


class DiskSpeedBlinker:
    def __init__(
        self,
        sample_dt=0.1,
        window_s=10.0,
        period_min=0.01,
        period_max=0.1,
        active_hold=0.25,
        min_activity_bps=1024.0,
    ):
        self.sample_dt = sample_dt
        self.window_s = window_s
        self.period_min = period_min
        self.period_max = period_max
        self.active_hold = active_hold
        self.min_activity_bps = min_activity_bps
        self.last = psutil.disk_io_counters()
        self.last_t = time.monotonic()
        self.rate_history = []
        self.max_recent = 1.0
        self.blink = False
        self.last_toggle = self.last_t
        self.last_active = 0.0
        self.cached_period = self.period_max

    def _trim_history(self, now):
        cutoff = now - self.window_s
        self.rate_history = [(stamp, rate) for stamp, rate in self.rate_history if stamp >= cutoff]
        self.max_recent = max((rate for _, rate in self.rate_history), default=1.0)
        self.max_recent = max(1.0, self.max_recent)

    def _map_pct_to_period(self, pct):
        pct = max(0.0, min(1.0, pct))
        return self.period_max - pct * (self.period_max - self.period_min)

    def lamp_on(self, now):
        if now - self.last_t >= self.sample_dt:
            current = psutil.disk_io_counters()
            dt = max(1e-3, now - self.last_t)
            if current is not None and self.last is not None:
                read_bps = max(0.0, (current.read_bytes - self.last.read_bytes) / dt)
            else:
                read_bps = 0.0
            self.last = current
            self.last_t = now
            self.rate_history.append((now, read_bps))
            self._trim_history(now)
            if read_bps >= self.min_activity_bps:
                self.last_active = now
            self.cached_period = self._map_pct_to_period(read_bps / self.max_recent)

        active = now - self.last_active < self.active_hold
        if not active:
            self.blink = False
            return False
        if now - self.last_toggle >= self.cached_period / 2.0:
            self.blink = not self.blink
            self.last_toggle = now
        return self.blink


class XLockStatus:
    LED_MASK_RE = re.compile(r"LED mask:\s*([0-9a-fA-F]+)")
    SCROLL_MODIFIER_RE = re.compile(r"(?m)^mod3\s+.*\bScroll_Lock\b")

    def __init__(self, sample_interval=0.1):
        self.sample_interval = sample_interval
        self.next_sample = 0.0
        self.next_modifier_check = 0.0
        self.cached = (False, False, False)
        self.env = os.environ.copy()
        self.env.setdefault("DISPLAY", ":0")
        self.env.setdefault("XAUTHORITY", str(Path.home() / ".Xauthority"))

    def _ensure_scroll_modifier(self, now):
        if now < self.next_modifier_check:
            return
        self.next_modifier_check = now + 5.0
        try:
            result = subprocess.run(
                ["xmodmap", "-pm"],
                check=True,
                capture_output=True,
                text=True,
                timeout=0.4,
                env=self.env,
            )
            if self.SCROLL_MODIFIER_RE.search(result.stdout):
                return
            subprocess.run(
                ["xmodmap", "-e", "add mod3 = Scroll_Lock"],
                check=True,
                capture_output=True,
                text=True,
                timeout=0.4,
                env=self.env,
            )
            logging.info("Added Scroll_Lock to the X11 mod3 modifier")
        except (OSError, subprocess.SubprocessError):
            logging.debug("Unable to configure the X11 Scroll Lock modifier")

    @classmethod
    def parse(cls, text):
        match = cls.LED_MASK_RE.search(text)
        if not match:
            raise ValueError("xset output has no LED mask")
        mask = int(match.group(1), 16)
        caps_on = bool(mask & 0x01)
        num_on = bool(mask & 0x02)
        scroll_on = bool(mask & 0x04)
        return num_on, caps_on, scroll_on

    def read(self, now):
        if now < self.next_sample:
            return self.cached
        self.next_sample = now + self.sample_interval
        self._ensure_scroll_modifier(now)
        try:
            result = subprocess.run(
                ["xset", "q"],
                check=True,
                capture_output=True,
                text=True,
                timeout=0.4,
                env=self.env,
            )
            self.cached = self.parse(result.stdout)
        except (OSError, subprocess.SubprocessError, ValueError):
            pass
        return self.cached


class MuteBlinker:
    def __init__(self, cycle=0.5, sample_interval=0.25):
        self.cycle = cycle
        self.sample_interval = sample_interval
        self.next_sample = 0.0
        self.muted = True
        self.env = os.environ.copy()
        self.env.setdefault("XDG_RUNTIME_DIR", "/run/user/1000")
        self.env.setdefault("PULSE_SERVER", "unix:/run/user/1000/pulse/native")

    @staticmethod
    def parse_muted(text):
        value = text.partition(":")[2].strip().lower()
        if value in {"yes", "true", "1", "on", "shi", "\u662f"}:
            return True
        if value in {"no", "false", "0", "off", "fou", "\u5426"}:
            return False
        raise ValueError("unknown pactl mute value: " + value)

    def is_muted(self, now):
        if now < self.next_sample:
            return self.muted
        self.next_sample = now + self.sample_interval
        try:
            result = subprocess.run(
                ["pactl", "get-sink-mute", "@DEFAULT_SINK@"],
                check=True,
                capture_output=True,
                text=True,
                timeout=0.5,
                env=self.env,
            )
            self.muted = self.parse_muted(result.stdout)
        except (OSError, subprocess.SubprocessError, ValueError):
            self.muted = True
        return self.muted

    def lamp_on(self, now):
        if self.is_muted(now):
            return False
        return now % self.cycle < self.cycle / 2.0


class EmulatorGuestStatus:
    """Read guest-owned ThinkPad panel signals exported by the SDL frontend."""

    def __init__(self, status_path=DEFAULT_EMULATOR_STATUS, sample_interval=0.05):
        self.status_path = Path(status_path)
        self.sample_interval = sample_interval
        self.next_sample = 0.0
        self.cached = None

    def read(self, now):
        if now < self.next_sample:
            return self.cached
        self.next_sample = now + self.sample_interval
        try:
            with self.status_path.open("r", encoding="utf-8") as stream:
                data = json.load(stream)
            timestamp = float(data["timestamp_unix"])
            fresh = -5.0 <= time.time() - timestamp <= 0.75
            if not fresh or data.get("machine") not in ("ibmtp700", "ibmtp700c") or not data.get("running"):
                raise ValueError("stale or non-ThinkPad emulator status")
            self.cached = {
                key: bool(data.get(key, False))
                for key in ("num", "caps", "scroll", "hdd", "floppy")
            }
        except (OSError, TypeError, ValueError, KeyError, json.JSONDecodeError):
            self.cached = None
        return self.cached


class ChargingStatus:
    def __init__(self, status_path=DEFAULT_UPS_STATUS, sample_interval=0.25):
        self.status_path = Path(status_path)
        self.sample_interval = sample_interval
        self.next_sample = 0.0
        self.cached = (False, False, False)

    @staticmethod
    def map_status(data):
        state = str(data.get("state", "")).lower()
        current = data.get("filtered_current_a", data.get("current_a"))
        try:
            current = float(current)
        except (TypeError, ValueError):
            current = None
        ac_online = state in {"charging", "charged", "full"}
        if state == "idle" and current is not None:
            ac_online = current >= 0.0
        if not ac_online:
            return False, False, False
        percent = data.get("improved_soc_percent", data.get("waveshare_soc_percent"))
        tri_on = True
        try:
            if percent is not None and float(percent) >= 99.0:
                return tri_on, False, True
        except (TypeError, ValueError):
            pass
        return tri_on, True, True

    def lamps(self, now):
        if now < self.next_sample:
            return self.cached
        self.next_sample = now + self.sample_interval
        try:
            with self.status_path.open("r", encoding="utf-8") as stream:
                data = json.load(stream)
            if not data.get("data_valid", False):
                raise ValueError("UPS data is not valid")
            self.cached = self.map_status(data)
        except (OSError, ValueError, json.JSONDecodeError):
            self.cached = (False, False, False)
        return self.cached


class SliderDisplay:
    def __init__(
        self,
        output="DP-1",
        window_size=11,
        apply_interval=0.15,
        hysteresis_percent=2,
        status_path=DEFAULT_SLIDER_STATUS,
        status_interval=0.2,
    ):
        self.output = output
        self.apply_interval = apply_interval
        self.hysteresis_percent = hysteresis_percent
        self.brightness = deque(maxlen=window_size)
        self.contrast = deque(maxlen=window_size)
        history_size = max(50, window_size)
        self.brightness_history = deque(maxlen=history_size)
        self.contrast_history = deque(maxlen=history_size)
        self.last_applied = None
        self.last_sample = None
        self.next_apply = 0.0
        self.status_path = Path(status_path)
        self.status_interval = status_interval
        self.next_status = 0.0
        self.env = os.environ.copy()
        self.env.setdefault("DISPLAY", ":0")
        self.env.setdefault("XAUTHORITY", str(Path.home() / ".Xauthority"))

    @staticmethod
    def parse(line):
        match = CTRL_RE.fullmatch(line)
        if not match:
            return None
        _, _, raw34, raw35, mv34, mv35 = map(int, match.groups())
        return {
            "raw34": raw34,
            "raw35": raw35,
            "mv34": mv34,
            "mv35": mv35,
            "brightness": calibrated_percent(
                mv35, BRIGHTNESS_MIN_MV, BRIGHTNESS_MAX_MV
            ),
            "contrast": calibrated_percent(
                mv34, CONTRAST_MIN_MV, CONTRAST_MAX_MV
            ),
        }

    def ingest(self, line):
        sample = self.parse(line)
        if sample is None:
            return False
        self.last_sample = sample
        self.brightness.append(sample["brightness"])
        self.contrast.append(sample["contrast"])
        self.brightness_history.append(sample["brightness"])
        self.contrast_history.append(sample["contrast"])
        return True

    @staticmethod
    def _channel_status(samples, history, applied):
        if not samples:
            return {
                "filtered_percent": None,
                "applied_percent": applied,
                "recent_min_percent": None,
                "recent_max_percent": None,
                "recent_span_percent": None,
            }
        ordered = sorted(samples)
        filtered = round(ordered[len(ordered) // 2])
        recent_min = min(history)
        recent_max = max(history)
        return {
            "filtered_percent": filtered,
            "applied_percent": applied,
            "recent_min_percent": recent_min,
            "recent_max_percent": recent_max,
            "recent_span_percent": recent_max - recent_min,
        }

    def write_status(self, now, connected, error=None, host_controls_enabled=True):
        if now < self.next_status:
            return
        self.next_status = now + self.status_interval
        sample = self.last_sample or {}
        brightness_applied = self.last_applied[0] if self.last_applied else None
        contrast_applied = self.last_applied[1] if self.last_applied else None
        payload = {
            "timestamp_unix": time.time(),
            "connected": connected,
            "error": error,
            "host_controls_enabled": bool(host_controls_enabled),
            "filter_window_samples": self.brightness.maxlen,
            "hysteresis_percent": self.hysteresis_percent,
            "raw": {
                "gpio34": sample.get("raw34"),
                "gpio35": sample.get("raw35"),
                "mv34": sample.get("mv34"),
                "mv35": sample.get("mv35"),
            },
            "brightness": {
                "pin": 35,
                "raw_percent": sample.get("brightness"),
                **self._channel_status(
                    self.brightness, self.brightness_history, brightness_applied
                ),
            },
            "contrast": {
                "pin": 34,
                "raw_percent": sample.get("contrast"),
                **self._channel_status(
                    self.contrast, self.contrast_history, contrast_applied
                ),
            },
        }
        temporary = self.status_path.with_suffix(".json.tmp")
        try:
            self.status_path.parent.mkdir(parents=True, exist_ok=True)
            temporary.write_text(
                json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            os.replace(temporary, self.status_path)
        except OSError as exc:
            logging.debug("Cannot update slider status: %s", exc)

    def _stable_value(self, samples, previous):
        value = round(sorted(samples)[len(samples) // 2])
        if previous is not None and abs(value - previous) <= self.hysteresis_percent:
            return previous
        return value

    def apply_if_due(self, now, host_controls_enabled=True):
        # While the TP700 emulator is active, sliders are consumed inside its
        # renderer.  Do not also change the desktop-wide DP-1 properties.
        if not host_controls_enabled:
            return False
        if (
            now < self.next_apply
            or len(self.brightness) < self.brightness.maxlen
            or len(self.contrast) < self.contrast.maxlen
        ):
            return False
        self.next_apply = now + self.apply_interval
        previous_brightness = self.last_applied[0] if self.last_applied else None
        previous_contrast = self.last_applied[1] if self.last_applied else None
        brightness = max(
            DISPLAY_MINIMUM_PERCENT,
            self._stable_value(self.brightness, previous_brightness),
        )
        contrast = max(
            DISPLAY_MINIMUM_PERCENT,
            self._stable_value(self.contrast, previous_contrast),
        )
        requested = (brightness, contrast)
        if requested == self.last_applied:
            return False
        gamma = contrast_gamma(contrast)
        gamma_value = f"{gamma:.3f}:{gamma:.3f}:{gamma:.3f}"
        subprocess.run(
            [
                "xrandr",
                "--output",
                self.output,
                "--set",
                "brightness",
                str(brightness),
                "--set",
                "contrast",
                str(contrast),
                "--gamma",
                gamma_value,
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=1.0,
            env=self.env,
        )
        self.last_applied = requested
        logging.info(
            "DISPLAY brightness=%d contrast=%d gamma=%.3f",
            brightness,
            contrast,
            gamma,
        )
        return True


class ButtonSwitches:
    def __init__(self, status_path=DEFAULT_SWITCH_STATUS, executor=None):
        self.executor = executor or ThreadPoolExecutor(max_workers=1)
        self.status_path = Path(status_path)
        self.env = os.environ.copy()
        self.env.setdefault("XDG_RUNTIME_DIR", "/run/user/1000")
        self.env.setdefault("PULSE_SERVER", "unix:/run/user/1000/pulse/native")
        self.env["LC_ALL"] = "C"
        self.future = None
        self.future_mode = None
        self.future_epoch = None
        self.reset()

    def reset(self):
        self.epoch = str(time.time_ns())
        self.inputs = None
        self.last_received = float("-inf")
        self.applied_volume = None
        self.error = None
        self.next_attempt = 0.0
        self.next_status = 0.0

    @staticmethod
    def parse(line):
        match = BUTTON_RE.fullmatch(line)
        if not match:
            return None
        mute, maximum, invert, valid = map(int, match.groups())
        if valid != int(not (mute and maximum)):
            return None
        return {"mute": mute, "maximum": maximum, "invert": invert,
                "volume": (None if not valid else
                           "mute" if mute else "maximum" if maximum else "middle")}

    def ingest(self, line, now):
        sample = self.parse(line)
        if sample is None:
            return False
        if sample != self.inputs:
            self.next_status = 0.0
            self.next_attempt = 0.0
            logging.info("SWITCH inputs=%s", sample)
        self.inputs = sample
        self.last_received = now
        return True

    def _apply_volume(self, mode):
        # This worker never owns the serial port or blocks lamp heartbeats.
        if mode == "mute":
            commands = [("set-sink-mute", "1"), ("set-sink-volume", "0%")]
        else:
            commands = [("set-sink-volume", "50%" if mode == "middle" else "100%"),
                        ("set-sink-mute", "0")]
        for action, value in commands:
            subprocess.run(["pactl", action, "@DEFAULT_SINK@", value],
                           check=True, capture_output=True, text=True,
                           timeout=0.8, env=self.env)

    def tick(self, now, connected, host_controls_enabled=True):
        if self.future is not None and self.future.done():
            try:
                self.future.result()
                if self.future_epoch == self.epoch:
                    self.applied_volume = self.future_mode
                    self.error = None
                    logging.info("SWITCH volume applied=%s", self.applied_volume)
            except Exception as exc:
                self.error = str(exc)
                self.next_attempt = now + 2.0
                logging.warning("Switch volume update failed: %s", exc)
            self.future = None
            self.next_status = 0.0
        fresh = connected and self.inputs is not None and now - self.last_received < 2.0
        # Keep the desktop sink unchanged while 86Box consumes this switch.
        desired = self.inputs["volume"] if fresh and host_controls_enabled else None
        if (desired is not None and desired != self.applied_volume
                and self.future is None and now >= self.next_attempt):
            self.future_mode = desired
            self.future_epoch = self.epoch
            self.future = self.executor.submit(self._apply_volume, desired)
        if now >= self.next_status:
            self.next_status = now + 0.5
            payload = {"timestamp_unix": time.time(), "connected": bool(fresh),
                       "host_controls_enabled": bool(host_controls_enabled),
                       "epoch": self.epoch, "inputs": self.inputs,
                       "applied_volume": self.applied_volume, "error": self.error}
            temporary = self.status_path.with_suffix(".json.tmp")
            try:
                self.status_path.parent.mkdir(parents=True, exist_ok=True)
                temporary.write_text(json.dumps(payload) + "\n", encoding="utf-8")
                os.replace(temporary, self.status_path)
            except OSError as exc:
                logging.debug("Cannot write switch status: %s", exc)

    def close(self):
        self.executor.shutdown(wait=True, cancel_futures=True)
        self.next_status = 0.0
        self.tick(time.monotonic(), connected=False)


class Controller:
    def __init__(self, port, ups_status, output="DP-1"):
        self.link = EspLink(port)
        self.hdd = DiskSpeedBlinker(period_min=0.01, period_max=0.1)
        self.network = NetworkSpeedBlinker()
        self.emulator = EmulatorGuestStatus()
        self.volume = MuteBlinker(cycle=0.5)
        self.locks = XLockStatus()
        self.charging = ChargingStatus(ups_status)
        self.display = SliderDisplay(output)
        self.switches = ButtonSwitches()
        self.stop_requested = False
        self.next_connect = 0.0
        self.next_heartbeat = 0.0
        self.next_status_log = 0.0

    def stop(self, *_args):
        self.stop_requested = True

    def _connect_if_due(self, now):
        if self.link.connected or now < self.next_connect:
            return
        self.next_connect = now + 1.0
        try:
            self.link.connect()
            self.switches.reset()
            self.link.set_pin_levels({pin: 1 for pin in ALL_PINS}, force=True)
            self.next_heartbeat = 0.0
        except OSError as exc:
            logging.warning("Waiting for ESP32 serial device: %s", exc)

    def run(self, run_seconds=None):
        started = time.monotonic()
        while not self.stop_requested:
            now = time.monotonic()
            if run_seconds is not None and now - started >= run_seconds:
                break
            self._connect_if_due(now)
            if not self.link.connected:
                self.switches.tick(now, connected=False)
                time.sleep(0.1)
                continue

            try:
                if now >= self.next_heartbeat:
                    self.link.heartbeat()
                    self.next_heartbeat = now + 0.1

                guest = self.emulator.read(now)
                if guest is not None:
                    num_on = guest["num"]
                    caps_on = guest["caps"]
                    scroll_on = guest["scroll"]
                    hdd_on = guest["hdd"]
                    floppy_on = guest["floppy"]
                    network_on = False
                    power_on = True
                    panel_source = "86box"
                else:
                    num_on, caps_on, scroll_on = self.locks.read(now)
                    hdd_on = self.hdd.lamp_on(now)
                    network_on = self.network.lamp_on(now)
                    floppy_on = network_on
                    power_on = False
                    panel_source = "host"
                vol_on = self.volume.lamp_on(now)
                charge = self.charging.lamps(now)
                lamps, yellow_requested = build_lamp_states(
                    num_on,
                    caps_on,
                    scroll_on,
                    hdd_on,
                    vol_on,
                    charge,
                    network_on,
                    power_on,
                    floppy_on,
                )
                levels = {pin: lamp_to_gpio_level(on) for pin, on in lamps.items()}
                self.link.set_pin_levels(levels)
                responses = self.link.poll()
                unexpected = [
                    line
                    for line in responses
                    if not self.display.ingest(line)
                    and not self.switches.ingest(line, now)
                    and line not in {"OK", "READY", "PONG"}
                ]
                if unexpected:
                    logging.debug("ESP32 response: %s", unexpected)
                host_controls_enabled = guest is None
                self.display.apply_if_due(now, host_controls_enabled)
                self.display.write_status(
                    now,
                    connected=True,
                    host_controls_enabled=host_controls_enabled,
                )
                self.switches.tick(
                    now,
                    connected=True,
                    host_controls_enabled=host_controls_enabled,
                )

                if now >= self.next_status_log:
                    self.next_status_log = now + 5.0
                    logging.info(
                        "NUM=%d CAPS=%d SCR=%d HDD=%d VOL=%d CHG=%d Ycalc=%d G=%d "
                        "FDD=%d PANEL=%s NET=%d NET_BPS=%.0f NET_BAND=%d "
                        "NET_STYLE=bursty NET_IF=%s",
                        num_on,
                        caps_on,
                        scroll_on,
                        hdd_on,
                        vol_on,
                        charge[0],
                        yellow_requested,
                        charge[2],
                        floppy_on,
                        panel_source,
                        network_on,
                        self.network.rate_bps,
                        self.network.band,
                        ','.join(sorted(self.network.interfaces)) or '-',
                    )
            except OSError as exc:
                logging.warning("ESP32 serial link lost: %s", exc)
                self.display.write_status(
                    time.monotonic(), connected=False, error=str(exc)
                )
                self.link.close()
                self.next_connect = time.monotonic() + 1.0
            time.sleep(0.02)
        self.link.close()
        self.switches.close()


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--ups-status", default=DEFAULT_UPS_STATUS)
    parser.add_argument("--output", default="DP-1")
    parser.add_argument("--run-seconds", type=float)
    parser.add_argument("--log-level", default="INFO")
    return parser.parse_args()


def main():
    args = parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(message)s",
    )
    controller = Controller(args.port, args.ups_status, args.output)
    signal.signal(signal.SIGTERM, controller.stop)
    signal.signal(signal.SIGINT, controller.stop)
    controller.run(args.run_seconds)


if __name__ == "__main__":
    main()
