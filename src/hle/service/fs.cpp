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
// the game's own SDK parses); SaveData/ExtSaveData/SDMC are host directories under
// saves/ next to game_data/.

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hle/ipc.h"
#include "hle/service/services.h"

namespace hle::service {

namespace {

namespace stdfs = std::filesystem;

constexpr Result ERR_NOT_FOUND = 0xC8804478;       // file not found
constexpr Result ERR_PATH_NOT_FOUND = 0xC8804470;  // directory not found
constexpr Result ERR_ALREADY_EXISTS = 0xC82044BE;
constexpr Result ERR_NOT_FORMATTED = 0xC8A04554;
constexpr Result ERR_INVALID_PATH = 0xE0E046BE;

stdfs::path g_game_data, g_saves;

struct Archive {
    u32 id;
    stdfs::path root;  // empty for SelfNCCH
};
std::mutex g_fs_lock;
std::map<u64, Archive> g_archives;
u64 g_next_archive = 1;

struct HostFile {
    std::FILE* f = nullptr;
    u64 base = 0;  // file offset presented to the guest as offset 0
    bool writable = false;
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
        std::fseek(file->f, long(file->base + offset), SEEK_SET);
        const u32 n = u32(std::fread(mem.host(addr), 1, size, file->f));
        c.reply(2, 2);
        c.set(2, n), c.set(3, size << 4 | 0xC), c.set(4, addr);
        return;
    }
    case 0x0803: {  // Write(offset u64, size, flush flags, mapped buffer desc, address) -> bytes written
        const u64 offset = u64(c[1]) | u64(c[2]) << 32;
        const u32 size = c[3], addr = c[6];
        u32 n = 0;
        if (file->writable) {
            std::fseek(file->f, long(file->base + offset), SEEK_SET);
            n = u32(std::fwrite(mem.host(addr), 1, size, file->f));
            std::fflush(file->f);
        }
        c.reply(2, 2, file->writable ? RESULT_OK : 0xC8804477);  // 0xC8804477: not writable
        c.set(2, n), c.set(3, size << 4 | 0xA), c.set(4, addr);
        return;
    }
    case 0x0805: {  // SetSize(u64)
        const u64 size = u64(c[1]) | u64(c[2]) << 32;
        std::fflush(file->f);
        const int fd = fileno(file->f);
        c.reply(1, 0, file->writable && ftruncate(fd, off_t(size)) == 0 ? RESULT_OK : 0xC8804477);
        return;
    }
    case 0x0804: {  // GetSize -> u64
        std::fseek(file->f, 0, SEEK_END);
        const u64 size = u64(std::ftell(file->f)) - file->base;
        c.reply(3, 0);
        c.set(2, u32(size)), c.set(3, u32(size >> 32));
        return;
    }
    case 0x080C:  // OpenLinkFile: another session on the same open file
        c.reply(1, 2);
        c.set(2, ipc::move_handles(1));
        c.set(3, kernel::open_session("fs:file", [file](core::Memory& m, u32 b) { file_command(file, m, b); }));
        return;
    case 0x0808:  // Close
    case 0x0809:  // Flush
    case 0x080A:  // SetPriority
        c.reply(1, 0);
        return;
    default:
        ipc::unimplemented("fs file", c);
    }
}

Handle open_file_session(const stdfs::path& path, const std::string& label, u64 base = 0, bool writable = false) {
    auto file = std::make_shared<HostFile>();
    file->f = std::fopen(path.c_str(), writable ? "r+b" : "rb");
    if (!file->f) return 0;
    file->base = base;
    file->writable = writable;
    return kernel::open_session(label, [file](core::Memory& mem, u32 base) { file_command(file, mem, base); });
}

// Offset of the IVFC level 3 (the actual RomFS) inside romfs.bin.
u64 romfs_level3_offset() {
    std::FILE* f = std::fopen((g_game_data / "romfs.bin").c_str(), "rb");
    if (!f) return 0;
    u8 ivfc[0x60] = {};
    const size_t n = std::fread(ivfc, 1, sizeof ivfc, f);
    std::fclose(f);
    if (n != sizeof ivfc || std::memcmp(ivfc, "IVFC", 4) != 0) return 0;
    u32 master_hash, block_log2;
    std::memcpy(&master_hash, ivfc + 0x8, 4);
    std::memcpy(&block_log2, ivfc + 0x4C, 4);
    const u64 block = u64(1) << block_log2;
    return (0x60 + master_hash + block - 1) & ~(block - 1);
}

// Opens a file inside an archive. Returns 0 if not found or unsupported (with `err`).
Handle open_in_archive(core::Memory& mem, u32 archive_id, u32 file_path_type, u32 file_path, u32 file_path_size,
                       Result* err) {
    if (archive_id == 0x3 && file_path_type == 2 && file_path_size >= 4 && mem.read<u32>(file_path) == 0) {
        // SelfNCCH, binary path {type 0 = RomFS}: the guest sees the image from the IVFC
        // level 3 header on (the SDK parses level 3 itself; hash levels are not exposed).
        const Handle h = open_file_session(g_game_data / "romfs.bin", "fs:romfs", romfs_level3_offset());
        *err = h ? RESULT_OK : ERR_NOT_FOUND;
        if (!h) std::fprintf(stderr, "fs: %s missing (re-run tools/extract_cia.py)\n",
                             (g_game_data / "romfs.bin").c_str());
        return h;
    }
    throw kernel::Unimplemented("fs: open file in archive " + hex(archive_id) + " path type " +
                                std::to_string(file_path_type) + " size " + std::to_string(file_path_size));
}

// FS path (type 1 empty, 2 binary, 3 ASCII, 4 UTF-16) -> relative host path; nullopt if
// it tries to leave the archive.
std::optional<stdfs::path> read_path(core::Memory& mem, u32 type, u32 addr, u32 size) {
    std::string s;
    if (type == 3) {
        for (u32 i = 0; i < size; ++i) {
            const char ch = char(mem.read<u8>(addr + i));
            if (!ch) break;
            s += ch;
        }
    } else if (type == 4) {
        for (u32 i = 0; i + 1 < size; i += 2) {
            const u16 ch = mem.read<u16>(addr + i);
            if (!ch) break;
            s += ch < 0x80 ? char(ch) : '_';
        }
    } else if (type == 2) {
        for (u32 i = 0; i < size; ++i) s += "0123456789abcdef"[mem.read<u8>(addr + i) >> 4], s += "0123456789abcdef"[mem.read<u8>(addr + i) & 15];
    }
    stdfs::path p;
    for (const auto& part : stdfs::path(s)) {
        if (part == "/" || part.empty()) continue;
        if (part == "..") return std::nullopt;
        p /= part;
    }
    return p;
}

stdfs::path archive_root(u32 id, const std::string& binary_path) {
    switch (id) {
    case 0x4: return g_saves / "savedata";
    case 0x6: case 0x7: return g_saves / "extdata" / binary_path;
    case 0x8: case 0x1234567B: return g_saves / "sysdata" / binary_path;
    case 0x9: return g_saves / "sdmc";
    default: return {};
    }
}

Archive* find_archive(const ipc::Cmd& c, u32 lo_index) {
    auto it = g_archives.find(u64(c[lo_index]) | u64(c[lo_index + 1]) << 32);
    return it == g_archives.end() ? nullptr : &it->second;
}

// Resolves a path inside a directory archive (nullopt: invalid / not a directory archive).
std::optional<stdfs::path> resolve(core::Memory& mem, Archive* a, u32 type, u32 size, u32 addr) {
    if (!a || a->root.empty()) return std::nullopt;
    auto rel = read_path(mem, type, addr, size);
    if (!rel) return std::nullopt;
    return a->root / *rel;
}

// Session for an open directory: Read(count) returns 0x228-byte entries.
Handle open_directory_session(const stdfs::path& dir) {
    auto entries = std::make_shared<std::vector<stdfs::directory_entry>>();
    for (const auto& e : stdfs::directory_iterator(dir)) entries->push_back(e);
    auto pos = std::make_shared<size_t>(0);
    return kernel::open_session("fs:dir", [entries, pos](core::Memory& mem, u32 base) {
        ipc::Cmd c{mem, base};
        if (c.id() == 0x0801) {  // Read(count, mapped buffer desc, address) -> entries read
            const u32 count = c[1], addr = c[3];
            u32 n = 0;
            for (; n < count && *pos < entries->size(); ++n, ++*pos) {
                const auto& e = (*entries)[*pos];
                const u32 at = addr + n * 0x228;
                std::memset(mem.host(at), 0, 0x228);
                const std::string name = e.path().filename().string();
                for (size_t i = 0; i < name.size() && i < 0x105; ++i) mem.write<u16>(at + u32(i) * 2, u8(name[i]));
                mem.write<u8>(at + 0x21C, e.is_directory());                  // is directory
                mem.write<u64>(at + 0x220, e.is_directory() ? 0 : e.file_size());  // file size
            }
            c.reply(2, 2);
            c.set(2, n), c.set(3, count * 0x228 << 4 | 0xC), c.set(4, addr);
            return;
        }
        c.reply(1, 0);  // Close and anything else
    });
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
    case 0x080C: {  // OpenArchive(archive id, path type, path size, path desc, path address) -> u64
        const u32 id = c[1];
        std::string bin;
        if (c[2] == 2) bin = read_path(mem, 2, c[5], c[3]).value_or(stdfs::path()).string();
        const stdfs::path root = archive_root(id, bin);
        if (id != 0x3 && root.empty())
            throw kernel::Unimplemented("fs: OpenArchive id " + hex(id) + " path type " + std::to_string(c[2]));
        if (id != 0x3 && !stdfs::is_directory(root)) {
            if (id == 0x9) {
                stdfs::create_directories(root);
            } else {
                c.reply(1, 0, ERR_NOT_FORMATTED);
                return;
            }
        }
        std::lock_guard lk(g_fs_lock);
        const u64 h = g_next_archive++;
        g_archives[h] = {id, root};
        c.reply(3, 0);
        c.set(2, u32(h)), c.set(3, u32(h >> 32));
        return;
    }
    case 0x080E: {  // CloseArchive(u64)
        std::lock_guard lk(g_fs_lock);
        g_archives.erase(u64(c[1]) | u64(c[2]) << 32);
        c.reply(1, 0);
        return;
    }
    case 0x0802: {  // OpenFile(transaction, archive u64, path type, path size, flags, attributes, desc, path)
        std::unique_lock lk(g_fs_lock);
        Archive* a = find_archive(c, 2);
        if (a && a->id == 0x3) {
            const u32 id = a->id;
            lk.unlock();
            Result err = RESULT_OK;
            const Handle h = open_in_archive(mem, id, c[4], c[9], c[5], &err);
            c.reply(1, 2, err);
            c.set(2, ipc::move_handles(1)), c.set(3, h);
            return;
        }
        const auto path = resolve(mem, a, c[4], c[5], c[9]);
        const u32 flags = c[6];
        lk.unlock();
        Result r = RESULT_OK;
        Handle h = 0;
        if (!path) {
            r = ERR_INVALID_PATH;
        } else {
            if (!stdfs::exists(*path) && (flags & 4)) std::fclose(std::fopen(path->c_str(), "wb"));
            if (!stdfs::is_regular_file(*path)) r = ERR_NOT_FOUND;
            else h = open_file_session(*path, "fs:file", 0, flags & 2);
            if (!r && !h) r = ERR_NOT_FOUND;
        }
        c.reply(1, 2, r);
        c.set(2, ipc::move_handles(1)), c.set(3, h);
        return;
    }
    case 0x0804: {  // DeleteFile(transaction, archive u64, path type, path size, desc, path)
        std::unique_lock lk(g_fs_lock);
        const auto path = resolve(mem, find_archive(c, 2), c[4], c[5], c[7]);
        lk.unlock();
        c.reply(1, 0, !path ? ERR_INVALID_PATH : stdfs::remove(*path) ? RESULT_OK : ERR_NOT_FOUND);
        return;
    }
    case 0x0808: {  // CreateFile(transaction, archive u64, path type, path size, attributes, u64 size, desc, path)
        std::unique_lock lk(g_fs_lock);
        const auto path = resolve(mem, find_archive(c, 2), c[4], c[5], c[10]);
        lk.unlock();
        Result r = RESULT_OK;
        if (!path) r = ERR_INVALID_PATH;
        else if (stdfs::exists(*path)) r = ERR_ALREADY_EXISTS;
        else if (std::FILE* f = std::fopen(path->c_str(), "wb")) {
            const u64 size = u64(c[7]) | u64(c[8]) << 32;
            if (size) ftruncate(fileno(f), off_t(size));
            std::fclose(f);
        } else {
            r = ERR_PATH_NOT_FOUND;
        }
        c.reply(1, 0, r);
        return;
    }
    case 0x0809: {  // CreateDirectory(transaction, archive u64, path type, path size, attributes, desc, path)
        std::unique_lock lk(g_fs_lock);
        const auto path = resolve(mem, find_archive(c, 2), c[4], c[5], c[8]);
        lk.unlock();
        Result r = !path ? ERR_INVALID_PATH : stdfs::exists(*path) ? ERR_ALREADY_EXISTS : RESULT_OK;
        if (!r) stdfs::create_directory(*path);
        c.reply(1, 0, r);
        return;
    }
    case 0x080B: {  // OpenDirectory(archive u64, path type, path size, desc, path)
        std::unique_lock lk(g_fs_lock);
        const auto path = resolve(mem, find_archive(c, 1), c[3], c[4], c[6]);
        lk.unlock();
        const bool ok = path && stdfs::is_directory(*path);
        c.reply(1, 2, !path ? ERR_INVALID_PATH : ok ? RESULT_OK : ERR_PATH_NOT_FOUND);
        c.set(2, ipc::move_handles(1)), c.set(3, ok ? open_directory_session(*path) : 0);
        return;
    }
    case 0x080D:  // ControlArchive (commit save data)
        c.reply(1, 0);
        return;
    case 0x0812:  // GetFreeBytes(archive u64) -> u64
        c.reply(3, 0);
        c.set(2, 0x10000000), c.set(3, 0);
        return;
    case 0x0817:  // IsSdmcDetected
    case 0x0818:  // IsSdmcWritable
        c.reply(2, 0);
        c.set(2, 1);
        return;
    case 0x0845: {  // GetFormatInfo(archive id, path type, path size, desc, path) -> size, dirs, files, duplicate
        const stdfs::path root = archive_root(c[1], {});
        if (root.empty() || !stdfs::is_directory(root)) {
            c.reply(1, 0, ERR_NOT_FORMATTED);
            return;
        }
        c.reply(5, 0);
        c.set(2, 0x80000), c.set(3, 16), c.set(4, 16), c.set(5, 0);
        return;
    }
    case 0x084C: {  // FormatSaveData(archive id, path type, path size, block size, dirs, files, buckets..., desc, path)
        const stdfs::path root = archive_root(c[1], {});
        if (root.empty()) throw kernel::Unimplemented("fs: FormatSaveData for archive " + hex(c[1]));
        stdfs::remove_all(root);
        stdfs::create_directories(root);
        c.reply(1, 0);
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
    g_saves = stdfs::absolute(game_data).parent_path() / "saves";
    kernel::register_service("fs:USER", fs_command);
}

}  // namespace hle::service
