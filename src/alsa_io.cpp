#ifndef XO_NO_ALSA
#include "alsa_io.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "synth.h"

namespace xo {

// ---------------------------------------------------------------------------
// Audio

bool AlsaAudio::open(const std::string& device, unsigned rate, unsigned period, unsigned periods,
                     std::string& err) {
    int e = snd_pcm_open(&pcm_, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (e < 0) { err = "cannot open audio device '" + device + "': " + snd_strerror(e); pcm_ = nullptr; return false; }

    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm_, hw);

    auto fail = [&](const char* what, int code) {
        err = std::string(what) + ": " + snd_strerror(code) + " (try a plughw: device)";
        close();
        return false;
    };

    if ((e = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        return fail("interleaved access unsupported", e);

    const snd_pcm_format_t formats[] = {SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_FLOAT_LE};
    format_ = SND_PCM_FORMAT_UNKNOWN;
    for (snd_pcm_format_t f : formats) {
        if (snd_pcm_hw_params_test_format(pcm_, hw, f) == 0) { format_ = f; break; }
    }
    if (format_ == SND_PCM_FORMAT_UNKNOWN) return fail("no supported sample format", -EINVAL);
    if ((e = snd_pcm_hw_params_set_format(pcm_, hw, format_)) < 0) return fail("set format", e);
    if ((e = snd_pcm_hw_params_set_channels(pcm_, hw, 2)) < 0) return fail("stereo output unsupported", e);

    unsigned r = rate;
    if ((e = snd_pcm_hw_params_set_rate_near(pcm_, hw, &r, nullptr)) < 0) return fail("set rate", e);

    snd_pcm_uframes_t ps = period;
    if ((e = snd_pcm_hw_params_set_period_size_near(pcm_, hw, &ps, nullptr)) < 0) return fail("set period", e);
    snd_pcm_uframes_t bs = ps * (periods < 2 ? 2 : periods);
    if ((e = snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &bs)) < 0) return fail("set buffer", e);
    if ((e = snd_pcm_hw_params(pcm_, hw)) < 0) return fail("apply hw params", e);

    snd_pcm_hw_params_get_period_size(hw, &ps, nullptr);
    snd_pcm_hw_params_get_buffer_size(hw, &bs);
    rate_ = r;
    period_ = static_cast<unsigned>(ps);
    buffer_ = static_cast<unsigned>(bs);

    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm_, sw);
    snd_pcm_sw_params_set_start_threshold(pcm_, sw, bs);
    snd_pcm_sw_params_set_avail_min(pcm_, sw, ps);
    if ((e = snd_pcm_sw_params(pcm_, sw)) < 0) return fail("apply sw params", e);

    if ((e = snd_pcm_prepare(pcm_)) < 0) return fail("prepare", e);
    buf_.assign(static_cast<size_t>(buffer_) * 2 * 4, 0);
    return true;
}

void AlsaAudio::close() {
    if (pcm_) {
        snd_pcm_drop(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
}

const char* AlsaAudio::formatName() const { return snd_pcm_format_name(format_); }

bool AlsaAudio::write(const float* left, const float* right, int n) {
    if (!pcm_) return false;
    size_t bytesPerSample = (format_ == SND_PCM_FORMAT_S16_LE) ? 2 : 4;
    size_t frameBytes = bytesPerSample * 2;
    if (buf_.size() < n * frameBytes) buf_.resize(n * frameBytes);

    if (format_ == SND_PCM_FORMAT_S16_LE) {
        int16_t* o = reinterpret_cast<int16_t*>(buf_.data());
        for (int i = 0; i < n; ++i) {
            o[2 * i] = static_cast<int16_t>(std::lrint(std::fmax(-1.0f, std::fmin(1.0f, left[i])) * 32767.0f));
            o[2 * i + 1] = static_cast<int16_t>(std::lrint(std::fmax(-1.0f, std::fmin(1.0f, right[i])) * 32767.0f));
        }
    } else if (format_ == SND_PCM_FORMAT_S32_LE) {
        int32_t* o = reinterpret_cast<int32_t*>(buf_.data());
        for (int i = 0; i < n; ++i) {
            o[2 * i] = static_cast<int32_t>(std::fmax(-1.0f, std::fmin(1.0f, left[i])) * 2147483520.0f);
            o[2 * i + 1] = static_cast<int32_t>(std::fmax(-1.0f, std::fmin(1.0f, right[i])) * 2147483520.0f);
        }
    } else {
        float* o = reinterpret_cast<float*>(buf_.data());
        for (int i = 0; i < n; ++i) { o[2 * i] = left[i]; o[2 * i + 1] = right[i]; }
    }

    const uint8_t* p = buf_.data();
    snd_pcm_sframes_t remaining = n;
    while (remaining > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(pcm_, p, static_cast<snd_pcm_uframes_t>(remaining));
        if (w == -EAGAIN) continue;
        if (w < 0) {
            ++xruns_;
            int e = snd_pcm_recover(pcm_, static_cast<int>(w), 1);
            if (e < 0) { std::fprintf(stderr, "xowave: audio write failed: %s\n", snd_strerror(e)); return false; }
            continue;
        }
        p += w * frameBytes;
        remaining -= w;
    }
    return true;
}

void AlsaAudio::listDevices() {
    int card = -1;
    std::printf("Playback devices (use as audio.device):\n");
    while (snd_card_next(&card) == 0 && card >= 0) {
        char hw[32];
        std::snprintf(hw, sizeof hw, "hw:%d", card);
        snd_ctl_t* ctl;
        if (snd_ctl_open(&ctl, hw, 0) < 0) continue;
        snd_ctl_card_info_t* info;
        snd_ctl_card_info_alloca(&info);
        if (snd_ctl_card_info(ctl, info) == 0) {
            int dev = -1;
            while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0) {
                snd_pcm_info_t* pi;
                snd_pcm_info_alloca(&pi);
                snd_pcm_info_set_device(pi, static_cast<unsigned>(dev));
                snd_pcm_info_set_subdevice(pi, 0);
                snd_pcm_info_set_stream(pi, SND_PCM_STREAM_PLAYBACK);
                if (snd_ctl_pcm_info(ctl, pi) < 0) continue;
                std::printf("  plughw:CARD=%s,DEV=%d   %s - %s\n", snd_ctl_card_info_get_id(info), dev,
                            snd_ctl_card_info_get_name(info), snd_pcm_info_get_name(pi));
            }
        }
        snd_ctl_close(ctl);
    }
}

// ---------------------------------------------------------------------------
// MIDI (ALSA sequencer: works with USB MIDI, USB-MIDI class devices and
// DIN MIDI interfaces; auto-connects every input, including hot-plugged ones)

bool AlsaMidi::open(const char* clientName, std::string& err) {
    int e = snd_seq_open(&seq_, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK);
    if (e < 0) { err = std::string("cannot open ALSA sequencer: ") + snd_strerror(e); seq_ = nullptr; return false; }
    snd_seq_set_client_name(seq_, clientName);
    client_ = snd_seq_client_id(seq_);
    port_ = snd_seq_create_simple_port(seq_, "MIDI In", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                       SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port_ < 0) { err = std::string("cannot create MIDI port: ") + snd_strerror(port_); close(); return false; }
    // Listen for devices being plugged in.
    snd_seq_connect_from(seq_, port_, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    connectAll();
    return true;
}

void AlsaMidi::close() {
    if (seq_) { snd_seq_close(seq_); seq_ = nullptr; }
}

void AlsaMidi::tryConnect(int client, int port) {
    if (client == SND_SEQ_CLIENT_SYSTEM || client == client_) return;
    snd_seq_client_info_t* ci;
    snd_seq_port_info_t* pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    if (snd_seq_get_any_client_info(seq_, client, ci) < 0) return;
    if (std::strcmp(snd_seq_client_info_get_name(ci), "Midi Through") == 0) return;
    if (snd_seq_get_any_port_info(seq_, client, port, pi) < 0) return;
    unsigned caps = snd_seq_port_info_get_capability(pi);
    const unsigned need = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
    if ((caps & need) != need || (caps & SND_SEQ_PORT_CAP_NO_EXPORT)) return;
    if (snd_seq_connect_from(seq_, port_, client, port) == 0)
        std::fprintf(stderr, "xowave: MIDI connected %d:%d (%s / %s)\n", client, port,
                     snd_seq_client_info_get_name(ci), snd_seq_port_info_get_name(pi));
}

void AlsaMidi::connectAll() {
    snd_seq_client_info_t* ci;
    snd_seq_port_info_t* pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq_, ci) >= 0) {
        int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq_, pi) >= 0) tryConnect(c, snd_seq_port_info_get_port(pi));
    }
}

int AlsaMidi::poll(Synth& synth, int channel) {
    if (!seq_) return 0;
    int count = 0;
    snd_seq_event_t* ev = nullptr;
    for (;;) {
        int r = snd_seq_event_input(seq_, &ev);
        if (r == -EAGAIN) break;
        if (r == -ENOSPC) continue;  // input overrun, keep draining
        if (r < 0 || !ev) break;

        ++count;
        auto chOk = [&](int ch) { return channel == 0 || ch == channel - 1; };
        switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON:
                if (chOk(ev->data.note.channel)) synth.noteOn(ev->data.note.note, ev->data.note.velocity);
                break;
            case SND_SEQ_EVENT_NOTEOFF:
                if (chOk(ev->data.note.channel)) synth.noteOff(ev->data.note.note);
                break;
            case SND_SEQ_EVENT_CONTROLLER:
                if (chOk(ev->data.control.channel))
                    synth.controlChange(static_cast<int>(ev->data.control.param), ev->data.control.value);
                break;
            case SND_SEQ_EVENT_PITCHBEND:
                if (chOk(ev->data.control.channel)) synth.pitchBend(ev->data.control.value / 8192.0f);
                break;
            case SND_SEQ_EVENT_PGMCHANGE:
                if (chOk(ev->data.control.channel)) synth.programChange(ev->data.control.value);
                break;
            case SND_SEQ_EVENT_PORT_START:
                tryConnect(ev->data.addr.client, ev->data.addr.port);
                break;
            default:
                break;
        }
    }
    return count;
}

}  // namespace xo
#endif
