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

#include "core/memory.h"
#include "core/types.h"

// HLE of the DSP audio firmware: the shared-memory protocol nn::snd talks to, the 24-voice
// mixer and the host output (SDL3). The dsp::DSP service (hle/service/dsp.cpp) hands out the
// structure addresses below and calls process_frame() once per audio frame.
namespace hle::dsp {

constexpr u32 SAMPLE_RATE = 32728;
constexpr u32 FRAME_SAMPLES = 160;
constexpr u32 NUM_VOICES = 24;

// Two copies ("regions") of the 15 structures. nn::snd::Initialize (0x1631c8) converts each
// address the pipe returns twice: `addr` and `addr | 0x10000` (DSP words), so region 1 sits
// 0x20000 bytes after region 0. The app fills the region whose frame counter it bumped last;
// the DSP reads that one and writes statuses into the other (Citra's protocol).
constexpr u32 DSP_DRAM_VA = core::Memory::DSP_BASE + 0x40000;  // DSP word address 0
constexpr u32 REGION0_WORD = 0x8000, REGION1_WORD = 0x18000;
constexpr u32 region_va(int r) { return DSP_DRAM_VA + (r ? REGION1_WORD : REGION0_WORD) * 2; }

// Byte offsets of the 15 structures inside a region (our packing: the pipe hands them out,
// so only each structure's inner layout and the region stride are fixed by the SDK).
enum Struct {
    FRAME_COUNTER, VOICE_CONFIG, VOICE_STATUS, ADPCM_COEFFS, DSP_CONFIG, DSP_STATUS,
    FINAL_SAMPLES, INTERMEDIATE_MIX, COMPRESSOR, DSP_DEBUG, UNKNOWN10, NUM_STRUCTS = 15
};
constexpr u16 STRUCT_OFFSET[NUM_STRUCTS] = {
    0x0000, 0x0100, 0x1300, 0x1500, 0x1800, 0x1C00, 0x1D00, 0x2100,
    0x3500, 0x3700, 0x3B00, 0x3F00, 0x4300, 0x4700, 0x4B00,
};

// Voice configuration (0xC0 bytes, stride confirmed in nn::snd::Initialize; fields per Citra's
// shared_memory.h, checked against live data). "u32_dsp" fields store the high half first.
namespace cfg {
constexpr u32 STRIDE = 0xC0;
constexpr u32 DIRTY = 0x00;           // u32, bits below
constexpr u32 GAIN = 0x04;            // float[3 mixes][4 channels]: FL FR BL BR
constexpr u32 RATE = 0x34;            // float, input samples per output sample
constexpr u32 BUFFERS_DIRTY = 0x4A;   // u16 bitmap over BUFFERS
constexpr u32 BUFFERS = 0x4C;         // 4 x Buffer (0x14 bytes)
constexpr u32 ENABLE = 0xA0;          // u8
constexpr u32 SYNC_COUNT = 0xA2;      // u16
constexpr u32 PLAY_POSITION = 0xA4;   // u32_dsp
constexpr u32 EMB_ADDRESS = 0xAC;     // u32_dsp, physical
constexpr u32 EMB_LENGTH = 0xB0;      // u32_dsp, samples
constexpr u32 FLAGS1 = 0xB4;          // bits 0-1 mono(1)/stereo(2), bits 2-3 format
constexpr u32 EMB_ADPCM_YN = 0xB8;    // s16[2]
constexpr u32 FLAGS2 = 0xBC;          // bit 0 adpcm_dirty, bit 1 looping
constexpr u32 EMB_BUFFER_ID = 0xBE;   // u16
// Buffer
constexpr u32 BUF_STRIDE = 0x14, BUF_ADDRESS = 0x0, BUF_LENGTH = 0x4, BUF_ADPCM_YN = 0xA,
              BUF_ADPCM_DIRTY = 0xE, BUF_LOOPING = 0xF, BUF_ID = 0x10;
// DIRTY bits
constexpr u32 FORMAT_DIRTY = 1u << 0, MONO_STEREO_DIRTY = 1u << 1, COEFFS_DIRTY = 1u << 2,
              PARTIAL_RESET = 1u << 4, ENABLE_DIRTY = 1u << 16, RATE_DIRTY = 1u << 18,
              QUEUE_DIRTY = 1u << 19, PLAY_POSITION_DIRTY = 1u << 21, GAIN0_DIRTY = 1u << 25,
              SYNC_DIRTY = 1u << 28, RESET = 1u << 29, EMBEDDED_DIRTY = 1u << 30;
}  // namespace cfg

// Voice status (0x0C bytes).
namespace status {
constexpr u32 STRIDE = 0x0C, ENABLED = 0x0, ID_DIRTY = 0x1, SYNC_COUNT = 0x2, POSITION = 0x4,
              BUFFER_ID = 0x8, PREVIOUS_ID = 0xA;
}

// DSP configuration: dirty u32 then master volume (float).
constexpr u32 MASTER_VOLUME = 0x04, MASTER_VOLUME_DIRTY = 1u << 17;

enum Format : u8 { PCM8 = 0, PCM16 = 1, ADPCM = 2 };

struct AdpcmState {
    s16 yn1 = 0, yn2 = 0;
};

// DSP-ADPCM: 8-byte frames of {header (predictor index << 4 | scale), 14 nibbles}. Decodes
// `samples` samples starting at a frame boundary.
void decode_adpcm(const u8* data, u32 samples, const s16 coeffs[16], AdpcmState& state, s16* out);

// One audio frame: parse the voice configurations, mix 160 stereo samples into `out`,
// write the voice statuses. Does not touch the frame counter (the app owns it).
void process_frame(core::Memory& mem, s16 out[FRAME_SAMPLES * 2]);
void reset();  // all voices off (DSP shutdown/initialize)
// Savestates: voice state (queued buffers, decoder history, play position), master volume.
void save_state(core::StateWriter& w);
void load_state(core::StateReader& r);

// Host output. Initialize() needs SDL_INIT_AUDIO; without a device, frames are dropped.
void Initialize();
void Shutdown();
void SetVolume(float gain);  // host output volume, 0..1 (settings menu)
void output(const s16* samples, u32 frames);

}  // namespace hle::dsp
