# Razer Laptop Cooling Pad: local validation notes

Local working material; exclude this document and the scripts/tests helpers
from the upstream device-support PR. Original development notes and source
snapshot are preserved at:
`C:\Users\Ryan\AppData\Local\Temp\openrgb-cooling-pad-20260910-101504`.

## Implementation

USB 1532:0F43, interface 0, usage page 0x0C / usage 0x01. One fixed LINEAR
zone, 18 LEDs; protocol row 0, columns 0..17, UI labels 1..18. Physical origin
and direction are unknown until a walking test.

Modes: Direct, Off, Static, Breathing, Spectrum Cycle, Wave, Starlight.
Reactive is disabled. Software effects belong in plugins/SDK/Visual Map.

Direct sends all 18 RGB values through the existing extended matrix path:
0F/03 frame followed by 0F/02 effect 08, with no-save storage. Existing waits
and per-frame allocation are unchanged. Selecting Direct sends current colors;
color updates while a native effect is selected do not replace that effect.
Device, zone and single-LED updates share the full color buffer.

One RGBController mutex serializes mode/frame calls because the core invokes
zone/single updates synchronously under a shared lock while its device thread
can also send modes/frames. No additional transport/frame-buffer mutex is used.

## ORION hardware checks

Confirm detection and 18 LEDs, then walk all indices and record physical order.
Exercise device, zone and single-LED updates, checking unchanged neighbors.
Test every native mode, including Starlight one/two/random colors and speeds
1..3, Wave direction and brightness. Test native -> Direct -> native transitions
with queued color updates, and retention of the last Direct frame when idle.

With the SDK server running and openrgb-python available:

```
python scripts/razer-cooling-pad-test.py --pattern walk --fps 1 --seconds 18
python scripts/razer-cooling-pad-test.py --pattern walk --api zone
python scripts/razer-cooling-pad-test.py --pattern walk --api single
python scripts/razer-cooling-pad-test.py --pattern rainbow --fps 5 10 20 30 60 --seconds 30
```

The helper leaves Direct black on exit. Reported timing is client submission
rate, not measured displayed FPS. Test Effects Plugin rainbow/gradient,
Ambient/Ambilight, audio visualization, fast effects and synchronization with
another device. Watch for flicker, dropped frames, delays and disconnects.

## Local tools

- `scripts/razer-cooling-pad-test.py`: optional SDK hardware validation.
- `scripts/razer-report-decode.py`: offline reference decoder; no USB access.
- `scripts/tests/razer-direct.cpp` and `.bat`: existing local mock-HID checks,
  not a proposed permanent upstream framework. No generic HID logging assertions.

Captures remain local evidence and must not be staged. No further Synapse
software-effect reverse engineering is needed for this implementation.
