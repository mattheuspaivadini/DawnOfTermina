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

// fs:USER — archives and files. RomFS is served from game_data/romfs.bin (the raw image
// the game's own SDK parses).

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hle/ipc.h"
#include "hle/service/services.h"

namespace hle::service {

namespace {

namespace stdfs = std::filesystem;

constexpr Result ERR_NOT_FOUND = 0xC8804478;
constexpr Result ERR_UNSUPPORTED_ARCHIVE = 0xC8A04554;

stdfs::path g_game_data;

struct HostFile {
    std::FILE* f = nullptr;
    ~HostFile() {
        if (f) std::fclose(f);
    }
};

std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

// Session handler for one open file.
void file_command(const std::shared_ptr<HostFile>& file, core::Memory& mem, u32 base) {
    ipc::Cmd c{mem, base};
    switch (c.id()) {
    case 0x0802: {  // Read(offset u64, size, mapped buffer desc, address) -> bytes read
        const u64 offset = u64(c[1]) | u64(c[2]) << 32;
        const u32 size = c[3], addr = c[5];
        std::fseek(file->f, long(offset), SEEK_SET);
        const u32 n = u32(std::fread(mem.host(addr), 1, size, file->f));
        c.reply(2, 2);
        c.set(2, n), c.set(3, size << 4 | 0xC), c.set(4, addr);
        return;
    }
    case 0x0804: {  // GetSize -> u64
        std::fseek(file->f, 0, SEEK_END);
        const u64 size = u64(std::ftell(file->f));
        c.reply(3, 0);
        c.set(2, u32(size)), c.set(3, u32(size >> 32));
        return;
    }
    case 0x0808:  // Close
    case 0x0809:  // Flush
    case 0x080A:  // SetPriority
        c.reply(1, 0);
        return;
    default:
        ipc::unimplemented("fs file", c);
    }
}

Handle open_file_session(const stdfs::path& path, const std::string& label) {
    auto file = std::make_shared<HostFile>();
    file->f = std::fopen(path.c_str(), "rb");
    if (!file->f) return 0;
    return kernel::open_session(label, [file](core::Memory& mem, u32 base) { file_command(file, mem, base); });
}

// Opens a file inside an archive. Returns 0 if not found or unsupported (with `err`).
Handle open_in_archive(core::Memory& mem, u32 archive_id, u32 file_path_type, u32 file_path, u32 file_path_size,
                       Result* err) {
    if (archive_id == 0x3 && file_path_type == 2 && file_path_size >= 4 && mem.read<u32>(file_path) == 0) {
        // SelfNCCH, binary path {type 0 = RomFS}
        const Handle h = open_file_session(g_game_data / "romfs.bin", "fs:romfs");
        *err = h ? RESULT_OK : ERR_NOT_FOUND;
        if (!h) std::fprintf(stderr, "fs: %s missing (re-run tools/extract_cia.py)\n",
                             (g_game_data / "romfs.bin").c_str());
        return h;
    }
    throw kernel::Unimplemented("fs: open file in archive " + hex(archive_id) + " path type " +
                                std::to_string(file_path_type) + " size " + std::to_string(file_path_size));
}

void fs_command(core::Memory& mem, u32 base) {
    ipc::Cmd c{mem, base};
    switch (c.id()) {
    case 0x0801:  // Initialize(process id)
    case 0x0861:  // InitializeWithSdkVersion(version, process id)
    case 0x0862:  // SetPriority
        c.reply(1, 0);
        return;
    case 0x0803: {  // OpenFileDirectly(transaction, archive id, archive path type/size,
                    //   file path type/size, flags, attributes, archive path desc/addr, file path desc/addr)
        Result err = RESULT_OK;
        const Handle h = open_in_archive(mem, c[2], c[5], c[12], c[6], &err);
        c.reply(1, 2, err);
        c.set(2, ipc::move_handles(1)), c.set(3, h);
        return;
    }
    case 0x0863:  // GetPriority
        c.reply(2, 0);
        c.set(2, 0);
        return;
    default:
        ipc::unimplemented("fs:USER", c);
    }
}

}  // namespace

void register_fs(const std::filesystem::path& game_data) {
    g_game_data = game_data;
    kernel::register_service("fs:USER", fs_command);
}

}  // namespace hle::service
