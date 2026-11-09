// ALSA audio output and ALSA sequencer MIDI input (Linux only).
#pragma once
#ifndef XO_NO_ALSA

#include <alsa/asoundlib.h>

#include <cstdint>
#include <string>
#include <vector>

namespace xo {

class Synth;

class AlsaAudio {
public:
    ~AlsaAudio() { close(); }
    bool open(const std::string& device, unsigned rate, unsigned period, unsigned periods, std::string& err);
    void close();
    bool write(const float* left, const float* right, int n);  // blocks until written

    unsigned rate() const { return rate_; }
    unsigned period() const { return period_; }
    unsigned bufferSize() const { return buffer_; }
    const char* formatName() const;
    uint64_t xruns() const { return xruns_; }

    static void listDevices();

private:
    snd_pcm_t* pcm_ = nullptr;
    snd_pcm_format_t format_ = SND_PCM_FORMAT_UNKNOWN;
    unsigned rate_ = 0, period_ = 0, buffer_ = 0;
    std::vector<uint8_t> buf_;
    uint64_t xruns_ = 0;
};

class AlsaMidi {
public:
    ~AlsaMidi() { close(); }
    bool open(const char* clientName, std::string& err);
    void close();
    // Non-blocking: drains pending events into the synth. channel 0 = omni, 1-16.
    int poll(Synth& synth, int channel);  // returns number of MIDI events

private:
    void connectAll();
    void tryConnect(int client, int port);

    snd_seq_t* seq_ = nullptr;
    int client_ = -1, port_ = -1;
};

}  // namespace xo

#endif
