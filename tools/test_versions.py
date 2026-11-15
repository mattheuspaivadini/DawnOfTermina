#!/usr/bin/env python3
# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DawnOfTermina contributors
"""Self-check of update-CIA title ID validation and per-version hook selection (no game data)."""
import sys
from pathlib import Path

root = Path(__file__).parent
sys.path[:0] = [str(root), str(root / "recomp")]
from extract_cia import check_update, version_name  # noqa: E402
from recomp import select_hooks  # noqa: E402

USA, EUR = 0x0004000000125500, 0x0004000000125600
check_update(USA, 0x0004000E00125500)
for base, upd in ((USA, 0x0004000E00125600),   # other region
                  (USA, 0x0004000E000D6E00),   # other game
                  (USA, 0x0004008C00125500),   # DLC, not an update
                  (0x0004000E00125500, 0x0004000E00125500)):  # update passed as base
    try:
        check_update(base, upd)
    except ValueError:
        continue
    raise AssertionError(f"accepted {upd:016X} for {base:016X}")
assert (version_name(0), version_name(1040)) == ("1.0", "1.1")

text = """# comment
[1.0 AAAA]
0x00300154 aeabi_memcpy  # trailing comment
[1.1 bbbb]
0x00300200 aeabi_memcpy
0x00300300 strlen
"""
assert select_hooks(text, "aaaa") == ("1.0", {0x300154: "aeabi_memcpy"})
assert select_hooks(text, "BBBB") == ("1.1", {0x300200: "aeabi_memcpy", 0x300300: "strlen"})
assert select_hooks(text, "cccc") == (None, {})
real = (root / "recomp" / "hooks.txt").read_text()  # the shipped table parses, v1.0 is known
version, hooks = select_hooks(real, "050a58834365187ca67211eef0a29816715e9ab9f7289e8372b06fb906620580")
assert version == "1.0" and len(hooks) == 12, (version, len(hooks))

print("test_versions: OK")
