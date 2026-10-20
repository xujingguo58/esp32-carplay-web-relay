#!/usr/bin/env python3
"""Generate the embedded DiPlay-compatible minimal AirPlay /info plist."""

from __future__ import annotations

import plistlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "components/airplay_software_provider/assets/info.plist"
WIDTH = 800
HEIGHT = 480
MAIN_UUID = "b7e6c5a0-1111-4000-8000-000000000001"


def touch_descriptor(x_max: int, y_max: int) -> bytes:
    app = bytes((0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01))
    finger = bytes((
        0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02,
        0x09, 0x38, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
        0x15, 0x00, 0x25, 0x01, 0x09, 0x33, 0x75, 0x01,
        0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03,
    ))
    x_suffix = bytes((0x09, 0x30, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02))
    contacts = bytearray(app)
    for _ in range(2):
        contacts.extend(finger)
        contacts.extend((0x05, 0x01, 0x26, x_max & 0xFF, x_max >> 8))
        contacts.extend(x_suffix)
        contacts.extend((0x26, y_max & 0xFF, y_max >> 8, 0x09, 0x31, 0x81, 0x02, 0xC0))
    contacts.append(0xC0)
    if len(contacts) != 109:
        raise AssertionError(f"unexpected touch descriptor size: {len(contacts)}")
    return bytes(contacts)


def main() -> None:
    # DiPlay CarPlay features with audio-output bits removed; this relay has no audio receiver.
    features = 0x615653AEE2 & ~0x10004540A00
    resource = lambda value: {
        "resourceID": value,
        "transferType": 1,
        "transferPriority": 100,
        "takeConstraint": 100,
        "borrowConstraint": 100,
        "unborrowConstraint": 100,
    }
    info = {
        "sourceVersion": "950.7.1",
        "features": features,
        "statusFlags": 4,
        "model": "ESP32",
        "manufacturer": "xcertplay",
        "deviceID": "00:00:00:00:00:01",
        "bluetoothIDs": ["00:00:00:00:00:02"],
        "name": "ESP32 CarPlay Relay",
        "rightHandDrive": False,
        "keepAliveLowPower": False,
        "keepAliveSendStatsAsBody": False,
        "modes": {
            "resources": [resource(1), resource(2)],
            "appStates": [
                {"appStateID": 2, "state": False},
                {"appStateID": 1, "speechMode": -1},
                {"appStateID": 3, "state": False},
            ],
        },
        "extendedFeatures": ["vocoderInfo", "enhancedRequestCarUI"],
        "displays": [{
            "uuid": MAIN_UUID,
            "type": 110,
            "maxFPS": 30,
            "widthPixels": WIDTH,
            "heightPixels": HEIGHT,
            "widthPhysical": 200,
            "heightPhysical": 120,
            "features": 0x08,
            "primaryInputDevice": 1,
            "uiAppearanceMode": 0,
            "uiAppearanceSetting": 0,
            "mapAppearanceMode": 0,
            "mapAppearanceSetting": 0,
            "viewAreas": [{
                "widthPixels": WIDTH,
                "heightPixels": HEIGHT,
                "originXPixels": 0,
                "originYPixels": 0,
                "safeArea": {
                    "widthPixels": WIDTH,
                    "heightPixels": HEIGHT,
                    "originXPixels": 0,
                    "originYPixels": 0,
                    "drawUIOutsideSafeArea": True,
                },
            }],
            "initialViewArea": 0,
        }],
        "hidDevices": [{
            "hidProductID": 1,
            "hidVendorID": 2,
            "hidCountryCode": 0,
            "uuid": "2a2a2a2a",
            "name": "xcertplay Touchscreen",
            "displayUUID": MAIN_UUID,
            "hidDescriptor": touch_descriptor(WIDTH, HEIGHT),
        }],
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY, sort_keys=True))
    print(f"wrote {OUTPUT.relative_to(ROOT)} ({OUTPUT.stat().st_size} bytes, {WIDTH}x{HEIGHT})")


if __name__ == "__main__":
    main()
