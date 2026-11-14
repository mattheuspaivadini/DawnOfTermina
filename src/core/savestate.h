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

#pragma once

#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "core/types.h"

// Savestate serialisation: a flat little-endian byte stream. Each module writes and reads its
// own section in the same order (hle::kernel::save_state drives it).
namespace core {

struct StateError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class StateWriter {
public:
    template <class T> void put(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&v, sizeof v);
    }
    void bytes(const void* p, size_t n) {
        const auto* b = static_cast<const u8*>(p);
        data.insert(data.end(), b, b + n);
    }
    template <class T> void vec(const std::vector<T>& v) {
        put(u64(v.size()));
        bytes(v.data(), v.size() * sizeof(T));
    }
    void str(const std::string& s) {
        put(u64(s.size()));
        bytes(s.data(), s.size());
    }
    void section(const char (&tag)[5]) { bytes(tag, 4); }  // checked on load: catches order mistakes
    std::vector<u8> data;
};

class StateReader {
public:
    explicit StateReader(std::vector<u8> d) : data(std::move(d)) {}
    template <class T> T get() {
        static_assert(std::is_trivially_copyable_v<T>);
        T v;
        bytes(&v, sizeof v);
        return v;
    }
    template <class T> void get(T& v) {  // also C arrays
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&v, sizeof v);
    }
    void bytes(void* p, size_t n) {
        if (n > data.size() - pos) throw StateError("savestate is truncated");
        std::memcpy(p, data.data() + pos, n);
        pos += n;
    }
    template <class T> std::vector<T> vec() {
        std::vector<T> v(size_t(get<u64>()));
        bytes(v.data(), v.size() * sizeof(T));
        return v;
    }
    std::string str() {
        std::string s(size_t(get<u64>()), '\0');
        bytes(s.data(), s.size());
        return s;
    }
    void section(const char (&tag)[5]) {
        char t[4];
        bytes(t, 4);
        if (std::memcmp(t, tag, 4)) throw StateError(std::string("savestate section '") + tag + "' not found");
    }
    std::vector<u8> data;
    size_t pos = 0;
};

}  // namespace core
