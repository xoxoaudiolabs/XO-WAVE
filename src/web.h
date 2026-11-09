// XO-Wave's web app: a page for a phone or computer on the same network
// (http://xo-wave.local), with every parameter, presets, master volume and
// tempo. Plain HTTP, no password (like XOXO Studio's): anyone on the network
// can use it.
//
// Runs on its own thread; changes reach the engine through their own
// lock-free queue (WebServer is that queue's only producer).
#pragma once
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "engine_link.h"
#include "wavetable.h"

namespace xo {

struct WebConfig {
    CommandQueue* cmds = nullptr;   // the web's own queue (drained by the audio thread)
    EngineStatus* status = nullptr;
    const PresetBank* bank = nullptr;
    const std::vector<Wavetable>* tables = nullptr;
    std::string fontsDir;
    std::string version;
    int port = 80;
};

class WebServer {
public:
    ~WebServer() { stop(); }
    bool start(const WebConfig& cfg);   // false if no port could be opened
    void stop();
    int port() const { return port_; }

private:
    void run();
    void handle(int fd);
    std::string stateJson() const;

    WebConfig cfg_;
    int listen_ = -1, port_ = 0;
    std::thread thread_;
    std::atomic<bool> quit_{false};
};

}  // namespace xo
