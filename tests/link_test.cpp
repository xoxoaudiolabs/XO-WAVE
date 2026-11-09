// Links StudioLink to a stand-in Studio (tests/fake_studio.py runs XOXO-Studio's
// own netmidi.Session) on 127.0.0.1, streams 2 s of a 440 Hz tone, sends a
// note, and prints the MIDI the Studio sends back.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "../src/studio_link.h"

int main() {
    xo::StudioLink link;
    xo::StudioLink::Config c;
    c.host = "127.0.0.1";
    c.port = 15004;
    c.audioPort = 46030;
    c.name = "XO-Wave on linktest";
    link.start(c);
    for (int i = 0; i < 50 && !link.linked(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("linked=%d  %s\n", link.linked(), link.describe().c_str());
    if (!link.linked()) return 1;
    const uint8_t on[3] = {0x90, 60, 100}, off[3] = {0x80, 60, 0};
    link.pushMidiOut(on, 3);
    float L[480], R[480];
    double ph = 0;
    int got = 0;
    for (int block = 0; block < 200; ++block) {  // 2 s at 48 kHz in 480-frame periods
        for (int i = 0; i < 480; ++i) { L[i] = R[i] = 0.5f * std::sin(ph); ph += 2 * M_PI * 440 / 48000; }
        link.sendAudio(L, R, 480);
        xo::MidiMsg m;
        while (link.popMidiIn(m)) { std::printf("from Studio: %02X %02X %02X\n", m.b[0], m.b[1], m.b[2]); ++got; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    link.pushMidiOut(off, 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("midi from Studio: %d\n", got);
    link.stop();
    return 0;
}
