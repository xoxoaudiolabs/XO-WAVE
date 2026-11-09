// Secondary audio output for Bluetooth (A2DP via BlueALSA).
//
// The 3.5 mm jack is the master clock and runs on the real-time thread.
// Bluetooth runs on its own normal-priority thread, fed through a lock-free
// ring buffer, so a stalled or disconnected Bluetooth device can never cause
// a glitch on the jack. The reader resamples with a slowly adapting ratio to
// absorb the sample-rate difference / clock drift between the two devices,
// and reconnects automatically when the device comes back.
#pragma once
#ifndef XO_NO_ALSA

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "engine_link.h"

namespace xo {


class BtOutput {
public:
    ~BtOutput() { stop(); }
    // device: a BlueALSA PCM name, or just the speaker's MAC address.
    void start(const std::string& device, unsigned sourceRate, unsigned latencyMs);
    void stop();
    bool connected() const { return active_.load(std::memory_order_relaxed); }
    // Called from the real-time thread after each rendered period.
    void push(const float* l, const float* r, size_t n) {
        if (active_.load(std::memory_order_relaxed)) ring_.push(l, r, n);
    }

private:
    void run();

    std::string device_;
    unsigned sourceRate_ = 48000, latencyMs_ = 150;
    std::thread thread_;
    std::atomic<bool> quit_{false}, active_{false};
    StereoRing ring_;
};

}  // namespace xo

#endif
