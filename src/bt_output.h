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

namespace xo {

class StereoRing {
public:
    explicit StereoRing(size_t framesPow2 = 32768) : buf_(framesPow2 * 2), mask_(framesPow2 - 1) {}
    size_t capacity() const { return mask_ + 1; }
    size_t available() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }

    // Producer (real-time thread). Drops what doesn't fit.
    void push(const float* l, const float* r, size_t n) {
        size_t wi = w_.load(std::memory_order_relaxed);
        size_t ri = r_.load(std::memory_order_acquire);
        size_t space = capacity() - (wi - ri);
        if (n > space) n = space;
        for (size_t i = 0; i < n; ++i) {
            size_t idx = (wi + i) & mask_;
            buf_[2 * idx] = l[i];
            buf_[2 * idx + 1] = r[i];
        }
        w_.store(wi + n, std::memory_order_release);
    }

    // Consumer side.
    const float* at(size_t offset) const { return &buf_[2 * ((r_.load(std::memory_order_relaxed) + offset) & mask_)]; }
    void advance(size_t n) { r_.store(r_.load(std::memory_order_relaxed) + n, std::memory_order_release); }
    void clear() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::vector<float> buf_;
    size_t mask_;
    std::atomic<size_t> w_{0}, r_{0};
};

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
