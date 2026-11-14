// DawnOfTermina — native static recompilation of Majora's Mask 3D.
// Copyright (C) 2026 DawnOfTermina contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "hle/fs.h"

#include <cstdio>
#include <vector>

namespace hle::fs {

namespace stdfs = std::filesystem;

static stdfs::path rom_root;
static std::vector<std::FILE*> files;  // handle = index + 1

static std::FILE* lookup(Handle h) { return h && h <= files.size() ? files[h - 1] : nullptr; }

bool MountRom(const stdfs::path& romfs_dir) {
    if (!stdfs::is_directory(romfs_dir)) return false;
    rom_root = romfs_dir;
    return true;
}

Result OpenFile(Handle* out, std::string_view path) {
    if (path.starts_with("rom:")) path.remove_prefix(4);
    while (path.starts_with('/')) path.remove_prefix(1);
    stdfs::path rel(path);
    for (const auto& part : rel)  // keep lookups inside romfs/
        if (part == "..") return ERR_INVALID_PATH;
    if (rom_root.empty() || path.empty()) return ERR_NOT_FOUND;

    std::FILE* f = std::fopen((rom_root / rel).c_str(), "rb");
    if (!f) return ERR_NOT_FOUND;
    for (size_t i = 0; i < files.size(); ++i)
        if (!files[i]) {
            files[i] = f;
            *out = Handle(i + 1);
            return RESULT_OK;
        }
    files.push_back(f);
    *out = Handle(files.size());
    return RESULT_OK;
}

Result GetSize(Handle h, u64* out) {
    std::FILE* f = lookup(h);
    if (!f) return ERR_INVALID_HANDLE;
    std::fseek(f, 0, SEEK_END);
    *out = u64(std::ftell(f));
    return RESULT_OK;
}

Result Read(Handle h, u64 offset, void* dst, u32 size, u32* bytes_read) {
    std::FILE* f = lookup(h);
    if (!f) return ERR_INVALID_HANDLE;
    std::fseek(f, long(offset), SEEK_SET);
    *bytes_read = u32(std::fread(dst, 1, size, f));
    return RESULT_OK;
}

Result Close(Handle h) {
    std::FILE* f = lookup(h);
    if (!f) return ERR_INVALID_HANDLE;
    std::fclose(f);
    files[h - 1] = nullptr;
    return RESULT_OK;
}

}  // namespace hle::fs
