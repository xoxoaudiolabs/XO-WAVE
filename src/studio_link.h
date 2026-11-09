// XOXO Studio link: XO-Wave as a box linked to a XOXO Studio, over Ethernet
// (a cable straight to the Studio, link-local 169.254.x.x) or a network.
//
// It works the way a Pocket or a following Studio links (XOXO-Studio's
// netmidi.py / bridge.py):
//   - finds the Studio's music session by DNS-SD (_apple-midi._udp), or uses
//     link.studio = <address>;
//   - joins it as a network MIDI peer named "XO-Wave on <host>" (AppleMIDI:
//     IN on the control port, then the data port; a clock sync, CK, every
//     10 s; BY when leaving), MIDI both ways in RTP-MIDI packets;
//   - streams its sound to the Studio as plain RTP on UDP 46030: L16 (S16BE),
//     48 kHz, stereo, payload type 127, 310-frame packets (PipeWire's rtp-sink
//     at net.mtu 1280, as a Studio sends). The Studio shows it as XO-WAVE.
//
// Threads: the link runs on its own thread; the audio thread only calls
// sendAudio / popMidiIn / pushMidiOut, which never block or allocate.
#pragma once
#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "engine_link.h"

namespace xo {

struct MidiMsg {
    uint8_t len = 0;
    uint8_t b[3] = {};
    double sent = -1;   // sender's clock (s) from the RTP-MIDI timestamp; -1 when unknown
};

class StudioLink {
public:
    struct Config {
        bool enabled = true;
        std::string host = "auto";  // "auto" = find by DNS-SD, else a Studio's address
        int port = 5004;            // the Studio's network MIDI control port (with host)
        int audioPort = 46030;      // the Studio's XO-WAVE receiver
        int returnPort = 46016;     // where the Studio sends its mix back (less XO-WAVE)
        std::string name;           // our session name ("XO-Wave on <host>")
    };

    enum State { Off = 0, Searching = 1, Linked = 2 };

    ~StudioLink() { stop(); }
    void start(const Config& cfg);
    void stop();

    void setEnabled(bool on) { enabled_.store(on); }
    bool enabled() const { return enabled_.load(); }
    bool linked() const { return state_.load(std::memory_order_relaxed) == Linked; }
    // Linked to a Studio that plays XO-Wave's sound (its /api/status has "xowave").
    // An older Studio links for MIDI only, so XO-Wave keeps its own outputs.
    bool audioAccepted() const { return linked() && audioOk_.load(std::memory_order_relaxed); }
    int state() const { return state_.load(std::memory_order_relaxed); }
    std::string describe() const;   // one line for the UI: who, where, or why not

    // Audio thread.
    void sendAudio(const float* left, const float* right, int n);
    bool popMidiIn(MidiMsg& m) { return in_.pop(m); }
    void pushMidiOut(const uint8_t* msg, int len);
    // Adds the Studio's mix (mix-minus) to left/right at gain; for the jack only.
    void mixReturn(float* left, float* right, int n, float gain);
    bool returnPlaying() const { return retPlaying_; }

private:
    void run();
    bool connect();
    bool findStudio(std::string& name, sockaddr_in& control);
    bool invite(int sock, const sockaddr_in& to, std::string& peerName);
    void received(const uint8_t* p, int len, bool dataPort);
    void sendSync(int count, uint64_t t1, uint64_t t2);
    void flushMidiOut();
    void receiveReturn();
    void leave(const char* why);
    void setStatus(const std::string& s);
    void checkStudio();   // GET /api/status: does this Studio take XO-Wave's sound?

    Config cfg_;
    std::thread thread_;
    std::atomic<bool> running_{false}, enabled_{true};
    std::atomic<int> state_{Off};
    std::atomic<bool> audioOk_{false};
    double nextCheck_ = 0;

    mutable std::mutex mu_;
    std::string peerName_, peerAddr_, status_ = "OFF";

    int ctl_ = -1, data_ = -1, audio_ = -1, ret_ = -1;
    sockaddr_in peerCtl_{}, peerData_{};
    uint32_t ssrc_ = 0, peerSsrc_ = 0;
    uint16_t midiSeq_ = 0;
    double heard_ = 0;

    // Audio packetiser (audio thread only). audioDest_: the Studio's IPv4
    // address in network order while linked, else 0.
    static constexpr int kFrames = 310;
    std::atomic<uint32_t> audioDest_{0};
    uint8_t packet_[12 + kFrames * 4] = {};
    int fill_ = 0;
    uint16_t rtpSeq_ = 0;
    uint32_t rtpTs_ = 0, audioSsrc_ = 0;

    // The Studio's mix back: link thread -> ring -> audio thread (drift-corrected).
    StereoRing retRing_{16384};
    bool retPlaying_ = false;
    double retFrac_ = 0, retErr_ = 0;

    SpscQueue<MidiMsg, 512> in_;    // Studio -> synth
    SpscQueue<MidiMsg, 512> out_;   // local playing -> Studio
};

// Packet parsers (exposed for the fuzz tests).
// RTP-MIDI -> up to maxOut messages; returns how many.
int decodeRtpMidi(const uint8_t* p, int len, MidiMsg* out, int maxOut);
// RTP L16 stereo (S16BE) -> up to maxFrames frames; returns how many (0 for anything else).
int parseRtpL16(const uint8_t* p, int len, float* left, float* right, int maxFrames);

// Plays one MIDI message on the synth (notes, CC, pitch bend, program).
// channel: 0 = omni, 1..16. System messages (clock etc.) are ignored.
class Synth;
void applyMidi(Synth& synth, const MidiMsg& m, int channel);

}  // namespace xo
