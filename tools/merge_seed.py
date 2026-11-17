#!/usr/bin/env python3
# DawnOfTermina — native static recompilation of Majora's Mask 3D.
# Copyright (C) 2026 DawnOfTermina contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Merges pipeline_seed.bin files (cache/pipeline_seed.bin of test runs) into one, without
duplicates: python3 tools/merge_seed.py resources/pipeline_seed.bin <run>/cache/pipeline_seed.bin ...
The first path is the output (and an input if it exists). Records are generator inputs only
(register values, shader blob hashes), see Renderer::SeedRecord."""
import sys
from pathlib import Path

MAGIC = b"SSD1"
RECORD = 3 * 8 + 4 * (1 + 7 + 4 + 1 + 45 + 8)  # must match sizeof(SeedRecord)


def records(path: Path):
    data = path.read_bytes() if path.is_file() else b""
    if data[:4] != MAGIC:
        return []
    return [data[i:i + RECORD] for i in range(4, len(data) - RECORD + 1, RECORD)]


def main():
    out, *inputs = map(Path, sys.argv[1:])
    seen, merged = set(), []
    for p in [out, *inputs]:
        for r in records(p):
            if r not in seen:
                seen.add(r)
                merged.append(r)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(MAGIC + b"".join(merged))
    print(f"{out}: {len(merged)} pipelines")


if __name__ == "__main__":
    main()
