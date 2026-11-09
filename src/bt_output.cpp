#ifndef XO_NO_ALSA
#include "bt_output.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

#include "alsa_io.h"
#include "dsp.h"

namespace xo {

static bool looksLikeMac(const std::string& s) {
    if (s.size() != 17) return false;
    for (size_t i = 0; i < 17; ++i) {
        if (i % 3 == 2) { if (s[i] != ':') return false; }
        else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

void BtOutput::start(const std::string& device, unsigned sourceRate, unsigned latencyMs) {
    if (thread_.joinable() || device.empty()) return;
    device_ = looksLikeMac(device) ? "bluealsa:DEV=" + device + ",PROFILE=a2dp" : device;
    sourceRate_ = sourceRate;
    latencyMs_ = std::clamp(latencyMs, 40u, 500u);
    quit_ = false;
    thread_ = std::thread([this] { run(); });
}

void BtOutput::stop() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
}

void BtOutput::run() {
    using namespace std::chrono_literals;
    enableFlushToZero();
    bool warned = false;

    while (!quit_) {
        AlsaAudio out;
        std::string err;
        if (!out.open(device_, sourceRate_, 1024, 4, err)) {
            if (!warned) {
                std::fprintf(stderr, "xowave: bluetooth audio not available yet (%s); will keep retrying\n", err.c_str());
                warned = true;
            }
            for (int i = 0; i < 30 && !quit_; ++i) std::this_thread::sleep_for(100ms);
            continue;
        }
        warned = false;
        std::fprintf(stderr, "xowave: bluetooth audio connected: %s (%u Hz, %s)\n", device_.c_str(), out.rate(),
                     out.formatName());

        const unsigned period = out.period();
        const double baseRatio = static_cast<double>(sourceRate_) / out.rate();  // source frames per output frame
        const double target = static_cast<double>(sourceRate_) * latencyMs_ / 1000.0;
        std::vector<float> L(period), R(period);

        ring_.clear();
        active_ = true;
        while (!quit_ && ring_.available() < target) std::this_thread::sleep_for(2ms);

        double frac = 0.0, errSmooth = 0.0;
        while (!quit_) {
            const size_t avail = ring_.available();
            double e = std::clamp((static_cast<double>(avail) - target) / target, -1.0, 1.0);
            errSmooth += (e - errSmooth) * 0.02;
            const double ratio = baseRatio * (1.0 + 0.005 * errSmooth);  // at most +/-0.5% (~8 cents)

            const size_t needed = static_cast<size_t>(frac + ratio * period) + 2;
            if (avail < needed) {  // underrun: play silence while the ring refills
                std::fill(L.begin(), L.end(), 0.0f);
                std::fill(R.begin(), R.end(), 0.0f);
            } else {
                for (unsigned i = 0; i < period; ++i) {
                    size_t i0 = static_cast<size_t>(frac);
                    float fr = static_cast<float>(frac - i0);
                    const float* a = ring_.at(i0);
                    const float* b = ring_.at(i0 + 1);
                    L[i] = a[0] + fr * (b[0] - a[0]);
                    R[i] = a[1] + fr * (b[1] - a[1]);
                    frac += ratio;
                }
                size_t adv = static_cast<size_t>(frac);
                ring_.advance(adv);
                frac -= adv;
            }
            if (!out.write(L.data(), R.data(), static_cast<int>(period))) break;
        }
        active_ = false;
        out.close();
        if (!quit_) std::fprintf(stderr, "xowave: bluetooth audio disconnected; waiting for it to return\n");
    }
}

}  // namespace xo
#endif
