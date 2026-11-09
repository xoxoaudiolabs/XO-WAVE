// Band-limited, mipmapped wavetables.
//
// A wavetable holds N frames (single-cycle waveforms) of 2048 samples.
// Each frame is stored at 11 mip levels: level m keeps harmonics
// 1..(1024 >> m), so the oscillator can pick a level with no harmonics
// above Nyquist for the note being played -> no aliasing.
#pragma once
#include <complex>
#include <string>
#include <vector>

namespace xo {

class Wavetable {
public:
    static constexpr int kSize = 2048;
    static constexpr int kMips = 11;
    static constexpr int kStride = kSize + 1;  // +1 guard sample for interpolation
    static constexpr int kMaxFrames = 256;

    std::string name;

    void init(const std::string& tableName, int frames);
    int numFrames() const { return frames_; }

    const float* table(int frame, int mip) const {
        return data_.data() + (static_cast<size_t>(frame) * kMips + mip) * kStride;
    }

    // spectrum[k] = complex amplitude of harmonic k (index 0 / DC ignored),
    // size kSize/2. A pure sine of amplitude a is {0, -a}. Frame is
    // normalised to peak 1.0.
    void setFrameSpectrum(int frame, const std::vector<std::complex<double>>& spectrum);
    // One cycle of kSize samples.
    void setFrameSamples(int frame, const float* samples);

private:
    int frames_ = 0;
    std::vector<float> data_;
};

// Mip level for a phase increment (cycles per sample).
inline int mipForIncrement(float inc) {
    float x = inc * Wavetable::kSize;  // highest harmonic of mip0 (1024) * inc * 2
    int m = 0;
    while (x > 1.0f && m < Wavetable::kMips - 1) { x *= 0.5f; ++m; }
    return m;
}

// Built-in tables generated at startup.
std::vector<Wavetable> makeFactoryTables();

// Loads a single-channel (or first channel of multi-channel) WAV containing
// back-to-back cycles. Frame size defaults to 2048, or is read from a Serum
// style "clm " chunk; frames of other sizes are resampled.
bool loadWavetableWav(const std::string& path, Wavetable& out, std::string& err);

}  // namespace xo
