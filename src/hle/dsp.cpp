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

// DSP audio firmware HLE. Voice semantics follow Citra's audio_core/hle (buffer queue
// ordered by buffer id, embedded buffer, looping, status reporting).
// shortcut: linear interpolation only, mix 0 only (aux buses 1/2, reverb/delay, filters and
// the compressor are ignored); add them if effects are audibly missing.

#include "hle/dsp.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace hle::dsp {

namespace {

struct Buffer {
    u32 address = 0, length = 0, play_position = 0;
    AdpcmState adpcm;
    bool adpcm_dirty = false, looping = false, from_queue = false, has_played = false;
    u16 id = 0;
    u8 channels = 1, format = PCM16;
};

struct Voice {
    bool enabled = false, id_dirty = false, played = false;
    u16 sync = 0, id = 0, previous_id = 0;
    float rate = 1.0f, gain_l = 0, gain_r = 0;
    u8 format = PCM16, channels = 1;
    s16 coeffs[16] = {};
    AdpcmState adpcm;
    std::vector<Buffer> queue;
    std::vector<s16> samples;  // current buffer, decoded to stereo frames
    double pos = 0;            // in frames of `samples`
};

Voice g_voices[NUM_VOICES];
float g_master_volume = 1.0f;
const bool g_trace = std::getenv("DOT_TRACE_AUDIO") != nullptr;
std::atomic<SDL_AudioStream*> g_stream{nullptr};

u32 u32_dsp(core::Memory& mem, u32 va) {  // high half first
    return u32(mem.read<u16>(va)) << 16 | mem.read<u16>(va + 2);
}
void put_u32_dsp(core::Memory& mem, u32 va, u32 v) {
    mem.write<u16>(va, u16(v >> 16));
    mem.write<u16>(va + 2, u16(v));
}

// The region the app filled last: higher frame counter, minding wrap-around.
int read_region(core::Memory& mem) {
    const u16 c0 = mem.read<u16>(region_va(0)), c1 = mem.read<u16>(region_va(1));
    if (c0 == 0xFFFF && c1 != 0xFFFE) return 1;
    if (c1 == 0xFFFF && c0 != 0xFFFE) return 0;
    return c0 > c1 ? 0 : 1;
}

void parse_config(core::Memory& mem, Voice& v, u32 c, u32 coeffs_va) {
    const u32 dirty = mem.read<u32>(c + cfg::DIRTY);
    if (!dirty) return;
    if (dirty & cfg::RESET) v = Voice{};
    if (dirty & cfg::PARTIAL_RESET) v.queue.clear();
    if (dirty & cfg::ENABLE_DIRTY) v.enabled = mem.read<u8>(c + cfg::ENABLE) != 0;
    if (dirty & cfg::SYNC_DIRTY) v.sync = mem.read<u16>(c + cfg::SYNC_COUNT);
    if (dirty & cfg::RATE_DIRTY) v.rate = mem.read<float>(c + cfg::RATE);
    if (dirty & cfg::COEFFS_DIRTY)
        for (int i = 0; i < 16; ++i) v.coeffs[i] = mem.read<s16>(coeffs_va + i * 2);
    if (dirty & cfg::GAIN0_DIRTY) {  // quad -> stereo: front + back
        auto g = [&](int ch) { return mem.read<float>(c + cfg::GAIN + ch * 4); };
        v.gain_l = g(0) + g(2), v.gain_r = g(1) + g(3);
    }
    const u16 flags1 = mem.read<u16>(c + cfg::FLAGS1);
    if (dirty & (cfg::FORMAT_DIRTY | cfg::EMBEDDED_DIRTY)) v.format = (flags1 >> 2) & 3;
    if (dirty & (cfg::MONO_STEREO_DIRTY | cfg::EMBEDDED_DIRTY)) v.channels = (flags1 & 3) == 2 ? 2 : 1;
    if (dirty & cfg::EMBEDDED_DIRTY) {
        const u16 flags2 = mem.read<u16>(c + cfg::FLAGS2);
        Buffer b;
        b.address = u32_dsp(mem, c + cfg::EMB_ADDRESS), b.length = u32_dsp(mem, c + cfg::EMB_LENGTH);
        b.adpcm = {mem.read<s16>(c + cfg::EMB_ADPCM_YN), mem.read<s16>(c + cfg::EMB_ADPCM_YN + 2)};
        b.adpcm_dirty = flags2 & 1, b.looping = flags2 & 2;
        b.id = mem.read<u16>(c + cfg::EMB_BUFFER_ID);
        b.channels = v.channels, b.format = v.format;
        if (dirty & cfg::PLAY_POSITION_DIRTY) b.play_position = u32_dsp(mem, c + cfg::PLAY_POSITION);
        v.queue.push_back(b);
    }
    if (dirty & cfg::QUEUE_DIRTY) {
        const u16 bits = mem.read<u16>(c + cfg::BUFFERS_DIRTY);
        for (u32 i = 0; i < 4; ++i) {
            const u32 q = c + cfg::BUFFERS + i * cfg::BUF_STRIDE;
            if (!(bits >> i & 1)) continue;
            Buffer b;
            b.address = u32_dsp(mem, q + cfg::BUF_ADDRESS), b.length = u32_dsp(mem, q + cfg::BUF_LENGTH);
            if (!b.length) continue;
            b.adpcm = {mem.read<s16>(q + cfg::BUF_ADPCM_YN), mem.read<s16>(q + cfg::BUF_ADPCM_YN + 2)};
            b.adpcm_dirty = mem.read<u8>(q + cfg::BUF_ADPCM_DIRTY), b.looping = mem.read<u8>(q + cfg::BUF_LOOPING);
            b.id = mem.read<u16>(q + cfg::BUF_ID);
            b.channels = v.channels, b.format = v.format, b.from_queue = true;
            v.queue.push_back(b);
        }
        mem.write<u16>(c + cfg::BUFFERS_DIRTY, 0);
    }
    mem.write<u32>(c + cfg::DIRTY, 0);
}

// Pops the lowest buffer id and decodes it into v.samples.
bool dequeue(core::Memory& mem, Voice& v, int index) {
    if (v.queue.empty()) return false;
    const auto it = std::min_element(v.queue.begin(), v.queue.end(),
                                     [](const Buffer& a, const Buffer& b) { return a.id < b.id; });
    Buffer b = *it;
    v.queue.erase(it);
    if (b.adpcm_dirty) v.adpcm = b.adpcm;

    v.samples.assign(size_t(b.length) * 2, 0);
    const u32 va = core::Memory::pa_to_va(b.address);
    const u32 bytes = b.format == ADPCM ? (b.length + 13) / 14 * 8 : b.length * b.channels * (b.format == PCM8 ? 1 : 2);
    if (va && b.length < (16u << 20) && va + bytes - core::Memory::LINEAR_BASE <= core::Memory::FCRAM_SIZE) {
        const u8* p = mem.host(va);
        if (b.format == ADPCM) {
            std::vector<s16> mono(b.length);
            decode_adpcm(p, b.length, v.coeffs, v.adpcm, mono.data());
            for (u32 i = 0; i < b.length; ++i) v.samples[i * 2] = v.samples[i * 2 + 1] = mono[i];
        } else {
            for (u32 i = 0; i < b.length * 2; ++i) {
                const u32 s = b.channels == 2 ? i : i / 2;
                s16 x;
                if (b.format == PCM8) x = s16(s8(p[s]) * 256);
                else std::memcpy(&x, p + s * 2, 2);
                v.samples[i] = x;
            }
        }
    } else {
        std::fprintf(stderr, "dsp: voice %d buffer at bad address %#x (+%u samples)\n", index, b.address, b.length);
    }
    if (g_trace && !b.has_played)
        std::fprintf(stderr, "dsp: voice %2d buffer id %u fmt %u ch %u pa %#x len %u rate %.3f gain %.2f/%.2f%s\n",
                     index, b.id, b.format, b.channels, b.address, b.length, v.rate, v.gain_l, v.gain_r,
                     b.looping ? " loop" : "");
    v.pos = b.has_played ? 0 : std::min(b.play_position, b.length);
    v.previous_id = v.id;
    v.id = b.id;
    v.id_dirty = b.from_queue && !b.has_played;
    v.played = true;
    if (b.looping) {
        b.has_played = true;
        v.queue.push_back(b);
    }
    return true;
}

void mix_voice(core::Memory& mem, Voice& v, int index, float* out) {
    if (!v.enabled) return;
    for (u32 n = 0; n < FRAME_SAMPLES; ++n) {
        while (v.samples.empty() || v.pos >= double(v.samples.size() / 2)) {
            v.pos -= double(v.samples.size() / 2);
            if (v.pos < 0) v.pos = 0;
            if (dequeue(mem, v, index)) continue;
            v.samples.clear();
            if (v.played) {  // ran dry: the voice stops (the app reuses it)
                v.enabled = false, v.played = false;
                v.previous_id = v.id, v.id = 0, v.id_dirty = true;
            }
            return;
        }
        const size_t frames = v.samples.size() / 2, i = size_t(v.pos);
        const float f = float(v.pos - double(i));
        const size_t j = i + 1 < frames ? i + 1 : i;
        const float l = v.samples[i * 2] + (v.samples[j * 2] - v.samples[i * 2]) * f;
        const float r = v.samples[i * 2 + 1] + (v.samples[j * 2 + 1] - v.samples[i * 2 + 1]) * f;
        out[n * 2] += l * v.gain_l;
        out[n * 2 + 1] += r * v.gain_r;
        v.pos += v.rate;
    }
}

}  // namespace

void decode_adpcm(const u8* data, u32 samples, const s16 coeffs[16], AdpcmState& state, s16* out) {
    s32 yn1 = state.yn1, yn2 = state.yn2;
    for (u32 n = 0; n < samples; ++n) {
        const u8* frame = data + n / 14 * 8;
        const u32 k = n % 14;
        const s32 scale = 1 << (frame[0] & 0xF), idx = (frame[0] >> 4) & 7;
        const u8 byte = frame[1 + k / 2];
        const s32 nibble = s32((k & 1 ? byte : byte >> 4) & 0xF) << 28 >> 28;  // sign-extend
        s32 val = ((nibble * scale) << 11) + 0x400 + coeffs[idx * 2] * yn1 + coeffs[idx * 2 + 1] * yn2;
        val = std::clamp(val >> 11, -32768, 32767);
        yn2 = yn1, yn1 = val;
        out[n] = s16(val);
    }
    state.yn1 = s16(yn1), state.yn2 = s16(yn2);
}

void reset() {
    for (auto& v : g_voices) v = Voice{};
    g_master_volume = 1.0f;
}

void process_frame(core::Memory& mem, s16 out[FRAME_SAMPLES * 2]) {
    const int rr = read_region(mem);
    const u32 rd = region_va(rr), wr = region_va(rr ^ 1);
    const u32 dsp_cfg = rd + STRUCT_OFFSET[DSP_CONFIG];
    if (mem.read<u32>(dsp_cfg) & MASTER_VOLUME_DIRTY) {
        g_master_volume = mem.read<float>(dsp_cfg + MASTER_VOLUME);
        if (g_trace) std::fprintf(stderr, "dsp: master volume %f\n", g_master_volume);
    }
    mem.write<u32>(dsp_cfg, 0);
    float mix[FRAME_SAMPLES * 2] = {};
    for (u32 i = 0; i < NUM_VOICES; ++i) {
        Voice& v = g_voices[i];
        parse_config(mem, v, rd + STRUCT_OFFSET[VOICE_CONFIG] + i * cfg::STRIDE,
                     rd + STRUCT_OFFSET[ADPCM_COEFFS] + i * 32);
        const u32 start = u32(v.pos);
        mix_voice(mem, v, int(i), mix);
        const u32 s = wr + STRUCT_OFFSET[VOICE_STATUS] + i * status::STRIDE;
        mem.write<u8>(s + status::ENABLED, v.enabled);
        mem.write<u8>(s + status::ID_DIRTY, v.id_dirty);
        mem.write<u16>(s + status::SYNC_COUNT, v.sync);
        put_u32_dsp(mem, s + status::POSITION, start);
        mem.write<u16>(s + status::BUFFER_ID, v.id);
        mem.write<u16>(s + status::PREVIOUS_ID, v.previous_id);
        v.id_dirty = false;
    }
    for (u32 n = 0; n < FRAME_SAMPLES * 2; ++n)
        out[n] = s16(std::clamp(std::lround(mix[n] * g_master_volume), -32768L, 32767L));
}

void Initialize() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "dsp: no audio (%s)\n", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(SAMPLE_RATE)};
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s) {
        std::fprintf(stderr, "dsp: no audio device (%s)\n", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(s);
    g_stream = s;
}

void Shutdown() {
    if (SDL_AudioStream* s = g_stream.exchange(nullptr)) SDL_DestroyAudioStream(s);
}

void output(const s16* samples, u32 frames) {
    SDL_AudioStream* s = g_stream.load();
    if (!s) return;
    // Faster-than-real-time guest (DOT_SPEED, catch-up bursts): drop instead of piling up latency.
    constexpr int MAX_QUEUED = SAMPLE_RATE * 4 / 8;  // 125 ms of S16 stereo
    if (SDL_GetAudioStreamQueued(s) > MAX_QUEUED) return;
    SDL_PutAudioStreamData(s, samples, int(frames * 4));
}

}  // namespace hle::dsp
