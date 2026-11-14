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

#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "hle/dsp.h"

using namespace hle::dsp;

static void test_adpcm() {
    // Predictor 0 = {1.0 (2048 in 5.11), 0}, scale 1: each +1 nibble adds 1 to the last sample.
    // Predictor 1 = {0, 0}, scale 16: nibble -8 decodes to -128 regardless of history.
    const s16 coeffs[16] = {2048, 0, 0, 0};
    const u8 data[16] = {0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                         0x14, 0x80, 0, 0, 0, 0, 0, 0};
    s16 out[16];
    AdpcmState st;
    decode_adpcm(data, 16, coeffs, st, out);
    for (int i = 0; i < 14; ++i) assert(out[i] == i + 1);
    assert(out[14] == -128 && out[15] == 0);
    assert(st.yn1 == 0 && st.yn2 == -128);
}

static void test_shared_memory() {
    core::Memory mem;
    assert(mem.map(core::Memory::LINEAR_BASE, 0x1000));
    for (u32 i = 0; i < 400; ++i) mem.write<s16>(core::Memory::LINEAR_BASE + i * 2, 1000);
    assert(region_va(0) == 0x1FF50000 && region_va(1) == 0x1FF70000);
    assert(STRUCT_OFFSET[VOICE_CONFIG] + NUM_VOICES * cfg::STRIDE <= STRUCT_OFFSET[VOICE_STATUS]);
    assert(STRUCT_OFFSET[VOICE_STATUS] + NUM_VOICES * status::STRIDE <= STRUCT_OFFSET[ADPCM_COEFFS]);

    // The app filled region 0 (higher frame counter): voice 3, embedded PCM16 mono buffer.
    reset();
    const u32 r0 = region_va(0), r1 = region_va(1);
    mem.write<u16>(r0, 5), mem.write<u16>(r1, 4);
    const u32 c = r0 + STRUCT_OFFSET[VOICE_CONFIG] + 3 * cfg::STRIDE;
    mem.write<u32>(c + cfg::DIRTY, cfg::ENABLE_DIRTY | cfg::GAIN0_DIRTY | cfg::RATE_DIRTY | cfg::FORMAT_DIRTY |
                                       cfg::MONO_STEREO_DIRTY | cfg::SYNC_DIRTY | cfg::EMBEDDED_DIRTY);
    mem.write<float>(c + cfg::GAIN, 0.5f), mem.write<float>(c + cfg::GAIN + 4, 0.25f);
    mem.write<float>(c + cfg::RATE, 1.0f);
    mem.write<u8>(c + cfg::ENABLE, 1);
    mem.write<u16>(c + cfg::SYNC_COUNT, 9);
    mem.write<u16>(c + cfg::EMB_ADDRESS, 0x2000), mem.write<u16>(c + cfg::EMB_ADDRESS + 2, 0);  // PA 0x20000000
    mem.write<u16>(c + cfg::EMB_LENGTH, 0), mem.write<u16>(c + cfg::EMB_LENGTH + 2, 400);
    mem.write<u16>(c + cfg::FLAGS1, 1 | PCM16 << 2);
    mem.write<u16>(c + cfg::EMB_BUFFER_ID, 7);

    s16 out[FRAME_SAMPLES * 2];
    const u32 s = r1 + STRUCT_OFFSET[VOICE_STATUS] + 3 * status::STRIDE;  // statuses go to the other region
    auto position = [&] { return u32(mem.read<u16>(s + status::POSITION)) << 16 | mem.read<u16>(s + status::POSITION + 2); };
    process_frame(mem, out);
    assert(out[0] == 500 && out[1] == 250 && out[319] == 250);
    assert(mem.read<u32>(c + cfg::DIRTY) == 0);
    assert(mem.read<u8>(s + status::ENABLED) == 1 && mem.read<u16>(s + status::SYNC_COUNT) == 9);
    assert(mem.read<u16>(s + status::BUFFER_ID) == 7 && position() == 0);
    process_frame(mem, out);
    assert(position() == 160);
    process_frame(mem, out);  // samples 320..399, then the buffer runs dry
    assert(out[79 * 2] == 500 && out[80 * 2] == 0);
    assert(mem.read<u8>(s + status::ENABLED) == 0 && mem.read<u16>(s + status::BUFFER_ID) == 0);
    assert(mem.read<u16>(s + status::PREVIOUS_ID) == 7);
    reset();
}

void test_audio() {
    test_adpcm();
    test_shared_memory();
    std::puts("audio: OK");
}
