#!/usr/bin/env python3
"""After compiling, prepare the browser installer from a real merged ESP32-S3 binary.
Refuses empty/missing/too-small firmware instead of exposing a fake install button.
"""
from pathlib import Path
import json
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / "installer"
MERGED = ROOT / "firmware" / "build" / "pulsepanel-sphere-full.bin"
TARGET = SITE / "pulsepanel-sphere-full.bin"
MANIFEST = SITE / "manifest.json"


def prepare(merged=MERGED, site=SITE):
    if not merged.is_file() or merged.stat().st_size < 100_000:
        raise RuntimeError("Brak skompilowanego firmware .bin. Najpierw idf.py build i idf.py merge-bin")
    with merged.open('rb') as handle:
        if handle.read(1) != b"\xe9":
            raise RuntimeError("Bledny naglowek ESP32 (.bin musi zaczynac sie od 0xE9)")
    if merged.stat().st_size > 16_777_216:
        raise RuntimeError("Obraz jest wiekszy niz flash 16 MB")
    site.mkdir(exist_ok=True, parents=True)
    target = site / TARGET.name
    shutil.copy2(merged, target)
    manifest = {
        "name": "PulsePanel Sphere HYBRID",
        "version": "0.7.0",
        "new_install_prompt_erase": True,
        "builds": [{
            "chipFamily": "ESP32-S3",
            "improv": False,
            "parts": [{"path": target.name, "offset": 0}],
        }],
    }
    (site / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return target


if __name__ == "__main__":
    try:
        print("Gotowe:", prepare())
    except RuntimeError as error:
        print("BLAD:", error, file=sys.stderr)
        sys.exit(1)
