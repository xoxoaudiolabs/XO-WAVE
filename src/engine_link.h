// Lock-free plumbing between the real-time audio thread and the UI thread.
//
//   UI  -> audio : SpscQueue<Cmd>    (parameter changes, notes, preset loads)
//   audio -> UI  : EngineStatus      (atomics the UI polls ~20x per second)
//   shared       : PresetBank        (fixed slots of atomic floats)
//
// Nothing here allocates or locks, so the audio thread never waits on the UI.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "params.h"

namespace xo {

enum class CmdType : uint8_t { SetParam, NoteOn, NoteOff, Program, AllNotesOff };

struct Cmd {
    CmdType type;
    int16_t a;  // param id / note / program
    float b;    // value / velocity
};

template <typename T, size_t N>
class SpscQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    bool push(const T& v) {
        size_t w = w_.load(std::memory_order_relaxed);
        if (w - r_.load(std::memory_order_acquire) >= N) return false;
        buf_[w & (N - 1)] = v;
        w_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& out) {
        size_t r = r_.load(std::memory_order_relaxed);
        if (r == w_.load(std::memory_order_acquire)) return false;
        out = buf_[r & (N - 1)];
        r_.store(r + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<T, N> buf_{};
    std::atomic<size_t> w_{0}, r_{0};
};

using CommandQueue = SpscQueue<Cmd, 1024>;

struct EngineStatus {
    std::array<std::atomic<float>, P_COUNT> param{};  // current target values
    std::atomic<float> peakL{0}, peakR{0};             // max since UI last read
    std::atomic<float> cpu{0};                         // audio thread load 0..1
    std::atomic<float> lfo{0};
    std::atomic<float> livePos[2] = {{0}, {0}};  // modulated wavetable position (osc A/B)
    std::atomic<int> voices{0};
    std::atomic<int> preset{-1};
    std::atomic<uint32_t> midiEvents{0};
    std::atomic<uint32_t> xruns{0};
    std::atomic<uint32_t> paramSerial{0};  // bumps whenever any param changes
    std::atomic<bool> btConnected{false};
    std::atomic<bool> running{true};

    // Settings the UI can change live.
    std::atomic<int> midiChannel{0};   // 0 = omni, 1..16
    std::atomic<float> master{0.8f};
    std::atomic<int> linkState{0};     // XOXO Studio link: 0 off, 1 searching, 2 linked   // master volume 0..1 (fader position), global, not per preset
    std::atomic<int> learnParam{-1};   // UI arms MIDI learn with a param id
    std::atomic<int> learnedCc{-1};    // engine reports the CC it bound
};

// Fixed-size preset storage readable from the audio thread (program change)
// and writable from the UI thread (save). Only one writer at a time: the
// main thread at startup, then only the UI thread.
class PresetBank {
public:
    static constexpr int kMax = 128;
    static constexpr int kNameLen = 40;
    static constexpr int kFileLen = 256;

    int count() const { return count_.load(std::memory_order_acquire); }

    const char* name(int i) const { return slots_[i].name; }
    const char* file(int i) const { return slots_[i].file; }

    void read(int i, float* out) const {
        for (int p = 0; p < P_COUNT; ++p) out[p] = slots_[i].v[p].load(std::memory_order_relaxed);
    }

    int find(const char* n) const {
        for (int i = 0; i < count(); ++i)
            if (std::strcmp(slots_[i].name, n) == 0) return i;
        return -1;
    }

    // Writes slot i (i == count() appends). Returns index or -1 when full.
    int write(int i, const char* n, const char* filePath, const float* values) {
        int c = count();
        if (i < 0 || i > c || i >= kMax) return -1;
        Slot& s = slots_[i];
        std::strncpy(s.name, n, kNameLen - 1);
        s.name[kNameLen - 1] = 0;
        std::strncpy(s.file, filePath ? filePath : "", kFileLen - 1);
        s.file[kFileLen - 1] = 0;
        for (int p = 0; p < P_COUNT; ++p) s.v[p].store(values[p], std::memory_order_relaxed);
        if (i == c) count_.store(c + 1, std::memory_order_release);
        return i;
    }

    // Removes slot i, shifting later presets down (UI thread only).
    void remove(int i) {
        int c = count();
        if (i < 0 || i >= c) return;
        float tmp[P_COUNT];
        for (int j = i; j < c - 1; ++j) {
            read(j + 1, tmp);
            write(j, slots_[j + 1].name, slots_[j + 1].file, tmp);
        }
        count_.store(c - 1, std::memory_order_release);
    }

private:
    struct Slot {
        char name[kNameLen] = {};
        char file[kFileLen] = {};
        std::atomic<float> v[P_COUNT];
    };
    Slot slots_[kMax];
    std::atomic<int> count_{0};
};

}  // namespace xo
