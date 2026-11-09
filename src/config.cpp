#include "config.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace xo {

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool readKeyValues(const std::string& path, KeyValues& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (!k.empty()) out[k] = v;
    }
    return true;
}

std::string kvGet(const KeyValues& kv, const std::string& key, const std::string& def) {
    auto it = kv.find(key);
    return it == kv.end() ? def : it->second;
}

int kvGetInt(const KeyValues& kv, const std::string& key, int def) {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return def;
    return std::atoi(it->second.c_str());
}

Preset presetFromKeyValues(const std::string& name, const KeyValues& kv, const std::vector<Wavetable>& tables,
                           std::vector<std::string>& warnings) {
    Preset p;
    p.name = kvGet(kv, "name", name);
    for (int i = 0; i < P_COUNT; ++i) p.values[i] = kParams[i].def;
    for (const auto& [key, value] : kv) {
        if (key == "name") continue;
        int id = findParam(key);
        if (id < 0) { warnings.push_back("unknown parameter '" + key + "'"); continue; }
        if (id == P_A_TABLE || id == P_B_TABLE) {
            char* end = nullptr;
            long idx = std::strtol(value.c_str(), &end, 10);
            if (end && *end == '\0' && !value.empty()) { p.values[id] = static_cast<float>(idx); continue; }
            bool found = false;
            for (size_t t = 0; t < tables.size(); ++t) {
                if (strcasecmp(tables[t].name.c_str(), value.c_str()) == 0) {
                    p.values[id] = static_cast<float>(t);
                    found = true;
                    break;
                }
            }
            if (!found) warnings.push_back("unknown wavetable '" + value + "'");
            continue;
        }
        p.values[id] = static_cast<float>(std::atof(value.c_str()));
    }
    return p;
}

// Writes a file so that it survives a power cut: write to a temp file,
// fsync it, rename over the target, then fsync the directory.
bool writeFileDurable(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const char* p = content.data();
    size_t left = content.size();
    while (left > 0) {
        ssize_t w = ::write(fd, p, left);
        if (w <= 0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
        p += w;
        left -= static_cast<size_t>(w);
    }
    bool ok = ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    std::string dir = path.substr(0, path.find_last_of('/') + 1);
    int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    return true;
}

std::string presetText(const std::string& name, const float* values, const std::vector<Wavetable>& tables) {
    std::ostringstream f;
    f << "name = " << name << "\n";
    for (int i = 0; i < P_COUNT; ++i) {
        f << kParams[i].name << " = ";
        if ((i == P_A_TABLE || i == P_B_TABLE) && static_cast<size_t>(values[i]) < tables.size())
            f << tables[static_cast<size_t>(values[i])].name;
        else if (kParams[i].integer)
            f << static_cast<int>(std::lround(values[i]));
        else
            f << values[i];
        f << "\n";
    }
    return f.str();
}

bool savePresetFile(const std::string& path, const std::string& name, const float* values,
                    const std::vector<Wavetable>& tables) {
    return writeFileDurable(path, presetText(name, values, tables));
}

bool writeKeyValues(const std::string& path, const KeyValues& kv) {
    std::string out;
    for (const auto& [k, v] : kv) out += k + " = " + v + "\n";
    return writeFileDurable(path, out);
}

static std::vector<std::string> listDir(const std::string& dir, const char* ext) {
    std::vector<std::string> files;
    DIR* d = opendir(dir.c_str());
    if (!d) return files;
    size_t el = std::char_traits<char>::length(ext);
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > el && n[0] != '.' && strcasecmp(n.c_str() + n.size() - el, ext) == 0)
            files.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<Preset> loadPresetDir(const std::string& dir, const std::vector<Wavetable>& tables) {
    std::vector<Preset> out;
    for (const std::string& path : listDir(dir, ".xowave")) {
        KeyValues kv;
        if (!readKeyValues(path, kv)) continue;
        std::string base = path.substr(path.find_last_of('/') + 1);
        base = base.substr(0, base.find_last_of('.'));
        std::vector<std::string> warnings;
        out.push_back(presetFromKeyValues(base, kv, tables, warnings));
        out.back().file = path;
        for (const auto& w : warnings) std::fprintf(stderr, "xowave: preset %s: %s\n", base.c_str(), w.c_str());
    }
    return out;
}

void loadWavetableDir(const std::string& dir, std::vector<Wavetable>& tables) {
    // Each frame costs ~90 KB once band-limited; keep user tables within ~150 MB of the
    // Pi's RAM and the wavetable parameter's range.
    constexpr int kFrameBudget = 1700, kMaxTables = 250;
    int frames = 0;
    for (const auto& t : tables) frames += t.numFrames();
    for (const std::string& path : listDir(dir, ".wav")) {
        if (static_cast<int>(tables.size()) >= kMaxTables) {
            std::fprintf(stderr, "xowave: skipping %s: too many wavetables (%d)\n", path.c_str(), kMaxTables);
            continue;
        }
        Wavetable w;
        std::string err;
        if (loadWavetableWav(path, w, err) && frames + w.numFrames() > kFrameBudget + 320) {
            std::fprintf(stderr, "xowave: skipping %s: wavetable memory full\n", path.c_str());
            continue;
        }
        if (!err.empty() || w.numFrames() == 0) {
            std::fprintf(stderr, "xowave: skipping %s: %s\n", path.c_str(), err.empty() ? "no frames" : err.c_str());
            continue;
        }
        frames += w.numFrames();
        {
            std::fprintf(stderr, "xowave: wavetable %d: %s (%d frames)\n", static_cast<int>(tables.size()),
                         w.name.c_str(), w.numFrames());
            tables.push_back(std::move(w));
        }
    }
}

}  // namespace xo
