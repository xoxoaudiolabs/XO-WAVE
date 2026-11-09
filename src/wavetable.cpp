#include "wavetable.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>

namespace xo {

using cd = std::complex<double>;
static const double kTau = 6.283185307179586;

// In-place iterative radix-2 FFT. inverse = no scaling.
static void fft(std::vector<cd>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = kTau / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j) {
                cd u = a[i + j];
                cd v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

void Wavetable::init(const std::string& tableName, int frames) {
    name = tableName;
    frames_ = std::clamp(frames, 1, kMaxFrames);
    data_.assign(static_cast<size_t>(frames_) * kMips * kStride, 0.0f);
}

void Wavetable::setFrameSpectrum(int frame, const std::vector<cd>& spectrum) {
    if (frame < 0 || frame >= frames_) return;
    const int half = kSize / 2;
    std::vector<cd> buf(kSize);
    float gain = 1.0f;

    for (int m = 0; m < kMips; ++m) {
        const int limit = std::min((half >> m), half - 1);  // drop Nyquist bin
        std::fill(buf.begin(), buf.end(), cd(0.0, 0.0));
        for (int k = 1; k <= limit && k < static_cast<int>(spectrum.size()); ++k) {
            buf[k] = spectrum[k];
            buf[kSize - k] = std::conj(spectrum[k]);
        }
        fft(buf, true);

        float* dst = data_.data() + (static_cast<size_t>(frame) * kMips + m) * kStride;
        for (int i = 0; i < kSize; ++i) dst[i] = static_cast<float>(buf[i].real());

        if (m == 0) {  // normalise every mip by the full-bandwidth peak
            float peak = 0.0f;
            for (int i = 0; i < kSize; ++i) peak = std::max(peak, std::fabs(dst[i]));
            gain = peak > 1e-9f ? 1.0f / peak : 0.0f;
        }
        for (int i = 0; i < kSize; ++i) dst[i] *= gain;
        dst[kSize] = dst[0];
    }
}

void Wavetable::setFrameSamples(int frame, const float* samples) {
    std::vector<cd> buf(kSize);
    for (int i = 0; i < kSize; ++i) buf[i] = cd(samples[i], 0.0);
    fft(buf, false);
    buf.resize(kSize / 2);
    setFrameSpectrum(frame, buf);
}

// ---------------------------------------------------------------------------
// Factory tables

static const int kFactoryFrames = 32;
static const int kHalf = Wavetable::kSize / 2;

using SpectrumFn = std::function<void(double t, std::vector<cd>& spec)>;
using SampleFn = std::function<double(double phase, double t)>;

static Wavetable spectral(const char* name, const SpectrumFn& fn) {
    Wavetable w;
    w.init(name, kFactoryFrames);
    std::vector<cd> spec(kHalf);
    for (int f = 0; f < kFactoryFrames; ++f) {
        std::fill(spec.begin(), spec.end(), cd(0, 0));
        fn(static_cast<double>(f) / (kFactoryFrames - 1), spec);
        w.setFrameSpectrum(f, spec);
    }
    return w;
}

static Wavetable timeDomain(const char* name, const SampleFn& fn) {
    Wavetable w;
    w.init(name, kFactoryFrames);
    std::vector<float> cycle(Wavetable::kSize);
    for (int f = 0; f < kFactoryFrames; ++f) {
        double t = static_cast<double>(f) / (kFactoryFrames - 1);
        for (int i = 0; i < Wavetable::kSize; ++i)
            cycle[i] = static_cast<float>(fn(static_cast<double>(i) / Wavetable::kSize, t));
        w.setFrameSamples(f, cycle.data());
    }
    return w;
}

static cd sinAmp(double a) { return cd(0.0, -a); }  // sine component
static cd cosAmp(double a) { return cd(a, 0.0); }   // cosine component

static void shapeSpectrum(int shape, std::vector<cd>& s, double w) {
    for (int k = 1; k < kHalf; ++k) {
        double a = 0.0;
        switch (shape) {
            case 0: a = (k == 1) ? 1.0 : 0.0; break;                                    // sine
            case 1: a = (k & 1) ? (((k - 1) / 2) & 1 ? -1.0 : 1.0) / (double(k) * k) : 0; break;  // triangle
            case 2: a = 1.0 / k; break;                                                 // saw
            case 3: a = (k & 1) ? 1.0 / k : 0.0; break;                                 // square
        }
        s[k] += sinAmp(a * w);
    }
}

std::vector<Wavetable> makeFactoryTables() {
    std::vector<Wavetable> t;

    // 0: sine -> triangle -> saw -> square
    t.push_back(spectral("Basic Shapes", [](double x, std::vector<cd>& s) {
        double seg = x * 3.0;
        int i = std::min(2, static_cast<int>(seg));
        double fr = seg - i;
        // triangle and square are quieter than saw/sine at equal peak;
        // the per-frame normalisation evens that out.
        shapeSpectrum(i, s, 1.0 - fr);
        shapeSpectrum(i + 1, s, fr);
    }));

    // 1: pulse width 50% -> 3%
    t.push_back(spectral("PWM", [](double x, std::vector<cd>& s) {
        double w = 0.5 - 0.47 * x;
        for (int k = 1; k < kHalf; ++k) s[k] = cosAmp(std::sin(k * M_PI * w) / k);
    }));

    // 2: hard-synced saw, slave ratio 1 -> 8
    t.push_back(timeDomain("Sync Saw", [](double p, double x) {
        double r = 1.0 + 7.0 * x;
        double q = p * r;
        return 2.0 * (q - std::floor(q)) - 1.0;
    }));

    // 3: saw through two moving vocal-ish formants
    t.push_back(spectral("Formant", [](double x, std::vector<cd>& s) {
        double c1 = 2.0 + 20.0 * x, c2 = 6.0 + 50.0 * x, bw = 2.5 + 4.0 * x;
        for (int k = 1; k < kHalf; ++k) {
            double g = 0.08 + std::exp(-std::pow((k - c1) / bw, 2.0)) +
                       0.6 * std::exp(-std::pow((k - c2) / (bw * 1.5), 2.0));
            s[k] = sinAmp(g / std::sqrt(static_cast<double>(k)));
        }
    }));

    // 4: two-operator FM, ratio 3, index 0 -> 8
    t.push_back(timeDomain("FM", [](double p, double x) {
        return std::sin(kTau * p + 8.0 * x * std::sin(kTau * 3.0 * p));
    }));

    // 5: sine wavefolder
    t.push_back(timeDomain("Wavefold", [](double p, double x) {
        return std::sin((1.0 + 7.0 * x) * (M_PI / 2.0) * std::sin(kTau * p));
    }));

    // 6: drawbar organ, harmonics fade in one by one
    t.push_back(spectral("Organ", [](double x, std::vector<cd>& s) {
        static const int harm[] = {1, 2, 3, 4, 6, 8, 10, 12, 16};
        for (int i = 0; i < 9; ++i) {
            double a = (i == 0) ? 1.0 : std::clamp(x * 8.0 - (i - 1), 0.0, 1.0) * 0.7;
            s[harm[i]] += cosAmp(a);
        }
    }));

    // 7: bit-reduced sine, 32 levels -> 1 level
    t.push_back(timeDomain("Digital", [](double p, double x) {
        double levels = std::round(32.0 * std::pow(1.0 - x, 2.0)) + 1.0;
        double v = std::sin(kTau * p);
        return std::round(v * levels) / levels;
    }));

    // 8: single bright harmonic sweeping up over a fundamental
    t.push_back(spectral("Harmonic Sweep", [](double x, std::vector<cd>& s) {
        double c = 2.0 + 30.0 * x;
        s[1] = sinAmp(0.6);
        for (int k = 2; k < 40; ++k) s[k] += sinAmp(std::max(0.0, 1.0 - std::fabs(k - c)));
    }));

    // 9: deterministic random spectra getting brighter and rougher
    t.push_back(spectral("Grit", [](double x, std::vector<cd>& s) {
        uint32_t seed = 12345u;
        auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
        double tilt = 2.0 - 1.4 * x;
        for (int k = 1; k < kHalf; ++k) {
            double a = std::pow(static_cast<double>(k), -tilt) * (1.0 - x + x * 2.0 * rnd());
            double ph = kTau * rnd() * x;
            s[k] = std::polar(a, ph - M_PI / 2);
        }
    }));

    return t;
}

// ---------------------------------------------------------------------------
// WAV loading

static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
static uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool loadWavetableWav(const std::string& path, Wavetable& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open"; return false; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) {
        err = "not a RIFF/WAVE file";
        return false;
    }

    int format = 0, channels = 0, bits = 0;
    const uint8_t* pcm = nullptr;
    size_t pcmBytes = 0;
    int frameSize = Wavetable::kSize;

    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        const uint8_t* ck = d.data() + pos;
        uint32_t len = rd32(ck + 4);
        const uint8_t* body = ck + 8;
        size_t avail = std::min<size_t>(len, d.size() - pos - 8);
        if (!std::memcmp(ck, "fmt ", 4) && avail >= 16) {
            format = rd16(body);
            channels = rd16(body + 2);
            bits = rd16(body + 14);
            if (format == 0xFFFE && avail >= 26) format = rd16(body + 24);  // extensible
        } else if (!std::memcmp(ck, "data", 4)) {
            pcm = body;
            pcmBytes = avail;
        } else if (!std::memcmp(ck, "clm ", 4) && avail > 3) {  // Serum: "<!>2048 ..."
            std::string s(reinterpret_cast<const char*>(body), avail);
            if (s.rfind("<!>", 0) == 0) {
                int n = std::atoi(s.c_str() + 3);
                if (n >= 16 && n <= 65536) frameSize = n;
            }
        }
        pos += 8 + len + (len & 1);
    }

    if (!pcm || channels < 1) { err = "missing fmt/data chunk"; return false; }
    int bytesPer = bits / 8;
    if (!((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32))) {
        err = "unsupported sample format (use 16/24/32-bit PCM or 32-bit float)";
        return false;
    }

    size_t total = pcmBytes / (static_cast<size_t>(bytesPer) * channels);
    std::vector<float> s(total);
    for (size_t i = 0; i < total; ++i) {
        const uint8_t* p = pcm + i * bytesPer * channels;
        float v = 0;
        if (format == 3) { uint32_t u = rd32(p); std::memcpy(&v, &u, 4); }
        else if (bits == 16) v = static_cast<int16_t>(rd16(p)) / 32768.0f;
        else if (bits == 24) v = static_cast<int32_t>((p[0] << 8) | (p[1] << 16) | (uint32_t(p[2]) << 24)) / 2147483648.0f;
        else v = static_cast<int32_t>(rd32(p)) / 2147483648.0f;
        s[i] = v;
    }
    if (total == 0) { err = "no samples"; return false; }

    int frames = static_cast<int>(total / frameSize);
    if (frames == 0) { frames = 1; frameSize = static_cast<int>(total); }  // treat as one short cycle
    frames = std::min(frames, Wavetable::kMaxFrames);

    std::string base = path.substr(path.find_last_of('/') + 1);
    if (base.size() > 4) base = base.substr(0, base.size() - 4);
    out.init(base, frames);

    std::vector<float> cycle(Wavetable::kSize);
    for (int fIdx = 0; fIdx < frames; ++fIdx) {
        const float* src = s.data() + static_cast<size_t>(fIdx) * frameSize;
        for (int i = 0; i < Wavetable::kSize; ++i) {  // linear resample to 2048
            double x = static_cast<double>(i) * frameSize / Wavetable::kSize;
            int i0 = static_cast<int>(x);
            double fr = x - i0;
            float a = src[i0 % frameSize], b = src[(i0 + 1) % frameSize];
            cycle[i] = static_cast<float>(a + (b - a) * fr);
        }
        out.setFrameSamples(fIdx, cycle.data());
    }
    return true;
}

}  // namespace xo
