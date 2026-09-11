#!/usr/bin/env python3
"""Decode copied Razer HID payloads, one hexadecimal payload per line.

Accepts 90-byte USB payloads or 91-byte HIDAPI buffers with report ID zero.
Does not open a device or send commands. SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import functools
import operator
from pathlib import Path


def decode(line):
    report = bytes.fromhex(line.replace(":", ""))
    if len(report) == 91 and report[0] == 0:
        report = report[1:]
    if len(report) != 90:
        raise ValueError("expected exactly 90 bytes, or 91 bytes with report ID zero")
    size, command_class, command = report[5:8]
    if size > 80:
        raise ValueError("invalid argument length")
    checksum = functools.reduce(operator.xor, report[2:88], 0)
    args = report[8:8 + size]
    text = (f"status={report[0]:02X} transaction={report[1]:02X} "
            f"command={command_class:02X}/{command:02X} size={size} "
            f"CRC={'OK' if checksum == report[88] else 'BAD'} args={args.hex(' ')}")
    if command_class == 0x0F and command == 0x03 and size >= 5:
        count = args[4] - args[3] + 1
        text += f" FRAME row={args[2]} columns={args[3]}..{args[4]}"
        if count <= 0 or count * 3 > size - 5:
            text += " INVALID RANGE/LENGTH"
        else:
            text += " RGB=" + ",".join(args[i:i + 3].hex() for i in range(5, 5 + count * 3, 3))
    elif command_class == 0x0F and command == 0x02 and size >= 3:
        effects = {0: "Off", 1: "Static", 2: "Breathing", 3: "Spectrum Cycle",
                   4: "Wave", 5: "Reactive", 7: "Starlight", 8: "Custom frame apply",
                   10: "Wheel (known on other Razer devices)"}
        text += f" EFFECT={effects.get(args[2], 'Unknown')} storage={args[0]:02X} led={args[1]:02X}"
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        payload = bytearray(90)
        payload[1] = 0x1F
        payload[5:8] = bytes([59, 15, 3])
        payload[12] = 17
        payload[13:16] = bytes([255, 0, 0])
        payload[88] = functools.reduce(operator.xor, payload[2:88], 0)
        decoded = decode(payload.hex())
        assert "CRC=OK" in decoded and "columns=0..17" in decoded
        assert decode((bytes([0]) + payload).hex()) == decoded
        payload[88] ^= 1
        assert "CRC=BAD" in decode(payload.hex())
        print("PASS: payload sizes, frame range and CRC decoding")
        return
    if args.input is None:
        parser.error("supply a text file containing one copied HID payload per line")
    for number, line in enumerate(args.input.read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line.strip():
            continue
        try:
            print(f"{number}: {decode(line)}")
        except ValueError as error:
            print(f"{number}: {error}")


if __name__ == "__main__":
    main()
