#!/usr/bin/env python3
"""Walking-LED and paced SDK tests for the Razer Laptop Cooling Pad.

Requires openrgb-python for hardware tests; --self-test needs only Python.
SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import colorsys
import math
import time


def pattern_colors(pattern, frame, count=18):
    if pattern == "walk":
        return [(255, 0, 0) if i == frame % count else (0, 0, 0) for i in range(count)]
    if pattern == "alternating":
        return [(255, 0, 0) if i % 2 == 0 else (0, 0, 255) for i in range(count)]
    if pattern == "rgb":
        return [((255, 0, 0), (0, 255, 0), (0, 0, 255))[i % 3] for i in range(count)]
    return [tuple(round(c * 255) for c in colorsys.hsv_to_rgb((i / count + frame / 180) % 1, 1, 1))
            for i in range(count)]


def self_test():
    for index in range(18):
        colors = pattern_colors("walk", index)
        assert colors[index] == (255, 0, 0)
        assert sum(c != (0, 0, 0) for c in colors) == 1
    assert pattern_colors("walk", 18) == pattern_colors("walk", 0)
    assert pattern_colors("alternating", 0)[:2] == [(255, 0, 0), (0, 0, 255)]
    assert pattern_colors("rgb", 0)[:3] == [(255, 0, 0), (0, 255, 0), (0, 0, 255)]
    assert len(set(pattern_colors("rainbow", 0))) == 18
    for name in ("walk", "alternating", "rgb", "rainbow"):
        for frame in (0, 17, 18, 179, 180):
            colors = pattern_colors(name, frame)
            assert len(colors) == 18 and all(0 <= c <= 255 for rgb in colors for c in rgb)
    print("PASS: walking, alternating, RGB and rainbow patterns")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6742)
    parser.add_argument("--pattern", choices=("walk", "alternating", "rgb", "rainbow"), default="walk")
    parser.add_argument("--fps", type=float, nargs="+", default=[1])
    parser.add_argument("--seconds", type=float, default=18, help="Duration at each requested FPS")
    parser.add_argument("--api", choices=("device", "zone", "single"), default="device")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not math.isfinite(args.seconds) or args.seconds <= 0 or any(
            not math.isfinite(fps) or not 0 < fps <= 60 for fps in args.fps):
        parser.error("Use a positive duration and FPS values above 0 and at most 60")
    if args.api == "single" and max(args.fps) > 5:
        parser.error("Single-LED tests are limited to 5 FPS; use device/zone batching for performance tests")

    from openrgb import OpenRGBClient
    from openrgb.utils import ModeColors, ModeFlags, RGBColor, ZoneType

    client = OpenRGBClient(args.host, args.port, name="Cooling pad validation")
    device = None
    started = False
    try:
        matches = [d for d in client.devices if d.name == "Razer Laptop Cooling Pad"]
        if len(matches) != 1:
            raise RuntimeError("Expected exactly one Razer Laptop Cooling Pad; no lighting was changed")
        device = matches[0]
        if len(device.leds) != 18 or len(device.zones) != 1 or len(device.zones[0].leds) != 18:
            raise RuntimeError("Unexpected LED topology; no lighting was changed")
        if device.zones[0].type != ZoneType.LINEAR:
            raise RuntimeError("Expected a linear zone; no lighting was changed")
        direct = next((m for m in device.modes if m.name == "Direct"), None)
        if direct is None or direct.color_mode != ModeColors.PER_LED or not (
                direct.flags & ModeFlags.HAS_PER_LED_COLOR):
            raise RuntimeError("Direct per-LED capability is missing; no lighting was changed")
        print(f"{device.name}: 18 LEDs, one linear zone, SDK protocol {client.protocol_version}")
        device.set_mode("Direct", save=False, force=True)
        started = True
        time.sleep(0.25)
        for fps in args.fps:
            start = time.perf_counter()
            cpu_start = time.process_time()
            sent = missed = 0
            next_frame = start
            latencies = []
            while time.perf_counter() - start < args.seconds:
                delay = next_frame - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
                if time.perf_counter() - start >= args.seconds:
                    break
                colors = [RGBColor(*c) for c in pattern_colors(args.pattern, sent)]
                if args.pattern == "walk" and fps <= 5:
                    print(f"LED index {sent % 18} (UI LED {sent % 18 + 1}) red", flush=True)
                before = time.perf_counter()
                if args.api == "device":
                    device.set_colors(colors, fast=True)
                elif args.api == "zone":
                    device.zones[0].set_colors(colors, fast=True)
                else:
                    for led, color in zip(device.leds, colors):
                        led.set_color(color, fast=True)
                now = time.perf_counter()
                latencies.append(now - before)
                sent += 1
                next_frame += 1 / fps
                if now > next_frame:
                    skipped = int((now - next_frame) * fps) + 1
                    missed += skipped
                    next_frame += skipped / fps
            elapsed = time.perf_counter() - start
            print(f"Requested {fps:g} FPS: submitted {sent / elapsed:.2f} FPS, "
                  f"missed deadlines={missed}, max send={max(latencies, default=0) * 1000:.2f} ms, "
                  f"client CPU={time.process_time() - cpu_start:.3f} s")
            print("Submission timing only: verify displayed FPS and USB errors with observation/capture.")
        device.update()
        if device.modes[device.active_mode].name != "Direct":
            raise RuntimeError("SDK mode changed during the test")
    finally:
        if started and device is not None:
            # Leave a known, non-persistent state, including on Ctrl+C.
            device.set_mode("Direct", save=False, force=True)
            device.set_colors([RGBColor(0, 0, 0)] * 18, fast=True)
            time.sleep(0.25)


if __name__ == "__main__":
    main()
