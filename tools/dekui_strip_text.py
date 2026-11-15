#!/usr/bin/env python3
# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""Copies DekUI's generated message-text headers into src/dekui/mm3d/ without the game's own text.

DekUI's SingleScreenText swaps messages that mention the touch screen for versions with the new
controls. Its data headers carry, next to DekUI's replacement text, verbatim copies of the
original messages (`original`, used only to undo a replacement when the option is switched off at
runtime) and the Classic Saving owl dialogues. Game text is Nintendo's and never goes into this
repository, so this script empties those strings; sizes and hashes stay (they identify the
original messages in memory). The glue never switches the option off at runtime and does not
port Classic Saving, so the emptied strings are never read. Usage:

    tools/dekui_strip_text.py <DekUI source dir>   # rewrites the five headers below"""
import re
import sys
from pathlib import Path

LITERAL = r'"(?:[^"\\]|\\.)*"'
# Replacement{id, size, hash, {text, n}, {original, n}, edited_hash}: keep the text, empty the original.
REPLACEMENT = re.compile(rf"(\{{0x[0-9a-f]+,\d+,0x[0-9a-f]+ULL,\{{{LITERAL},\d+\}},)\{{{LITERAL},\d+\}}(,0x[0-9a-f]+ULL\}})")
REPLACEMENTS = ("single_screen_text_data.h", "single_screen_localized_data.h")
CLASSIC = ("classic_owl_text_data.h", "classic_localized_text.h", "classic_owl_return_text.h")  # Classic Saving only
NOTE = "// DawnOfTermina: copied by tools/dekui_strip_text.py with the game's own text removed.\n"


def strip(name: str, text: str) -> str:
    if name in REPLACEMENTS:
        text, n = REPLACEMENT.subn(r"\1{}\2", text)
        if not n and "Replacement," in text and "{0x" in text:
            raise SystemExit(f"{name}: no Replacement entry recognised (upstream format changed?)")
    else:
        text = re.sub(rf"\{{{LITERAL},\d+\}}", "{}", text)
        if '#include "' in text:
            raise SystemExit(f"{name}: quoted #include (would be blanked)")
        text = re.sub(LITERAL, '""', text)
    lines = text.split("\n", 1)
    return lines[0] + "\n" + NOTE + lines[1]


def main():
    src = Path(sys.argv[1]) / "src" / "mm3d"
    out = Path(__file__).resolve().parent.parent / "src" / "dekui" / "mm3d"
    for name in REPLACEMENTS + CLASSIC:
        (out / name).write_text(strip(name, (src / name).read_text()))
        print(f"{name}: written")


if __name__ == "__main__":
    main()
