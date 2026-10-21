#include "config.hpp"

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <sstream>

namespace imp {

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool load_config(const std::string& path, Params& p, StartupSettings& s, std::string* warn) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    int fixed = 0, lineno = 0;
    auto w = [&](const std::string& m) { if (warn) *warn += path + ":" + std::to_string(lineno) + ": " + m + "\n"; };
    while (std::getline(in, line)) {
        ++lineno;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        size_t eq = line.find('=');
        if (eq == std::string::npos) { if (!trim(line).empty()) w("ignored line"); continue; }
        std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        auto as_int = [&](int lo, int hi) {
            size_t idx = 0;
            int v = std::stoi(val, &idx);
            if (idx != val.size()) throw std::invalid_argument(key);
            if (v < lo || v > hi) {
                w(key + " out of range " + std::to_string(lo) + ".." + std::to_string(hi) + ", clamped");
                v = v < lo ? lo : hi;
            }
            return v;
        };
        try {
            if (key == "device") s.device = val;
            else if (key == "sample_rate") s.sample_rate = as_int(8000, 192000);
            else if (key == "period") s.period = as_int(16, 4096);
            else if (key == "prefill") s.prefill = as_int(1, 8);
            else if (key == "input_channel") s.input_channel = as_int(0, 7);
            else if (key == "rt_priority") s.rt_priority = as_int(1, 95);
            else if (key == "web_port") s.web_port = as_int(1, 65535);
            else if (key == "web_bind") s.web_bind = val;
            else if (key == "record_dir") s.record_dir = val;
            else if (key == "record_segment_min") s.record_segment_min = as_int(0, 600);
            else if (key == "fixed_notch") {
                std::istringstream ss(val);
                float hz, g, q;
                if ((ss >> hz >> g >> q) && std::isfinite(hz) && std::isfinite(g) && std::isfinite(q) && hz > 20 &&
                    hz < 20000 && g <= 0 && g >= -40 && q > 0.1f && q < 200) {
                    if (fixed < kMaxFixedNotches) {
                        p.fixed[fixed].hz.store(hz);
                        p.fixed[fixed].gain_db.store(g);
                        p.fixed[fixed].q.store(q);
                        ++fixed;
                    } else w("too many fixed notches");
                } else w("fixed_notch needs: hz (20-20000) gain_db (-40..0) q (0.1-200)");
            } else {
                int id = Params::find(key);
                size_t idx = 0;
                if (id < 0) w("unknown key '" + key + "'");
                else {
                    float f = std::stof(val, &idx);
                    if (idx != val.size() || !p.set((ParamId)id, f)) w("bad value for '" + key + "', kept default");
                }
            }
        } catch (...) {
            w("bad value for '" + key + "'");
        }
    }
    p.fixed_count.store(fixed);
    p.fixed_version.fetch_add(1);
    return true;
}

bool save_config(const std::string& path, const Params& p, const StartupSettings& s) {
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "# improvdsp settings (written by the control page; safe to edit by hand)\n\n");
    std::fprintf(f, "# --- hardware (read at start-up only)\n");
    std::fprintf(f, "device = %s\nsample_rate = %d\nperiod = %d\nprefill = %d\ninput_channel = %d\n",
                 s.device.c_str(), s.sample_rate, s.period, s.prefill, s.input_channel);
    std::fprintf(f, "rt_priority = %d\nweb_port = %d\nweb_bind = %s\nrecord_dir = %s\nrecord_segment_min = %d\n\n",
                 s.rt_priority, s.web_port, s.web_bind.c_str(), s.record_dir.c_str(), s.record_segment_min);
    std::fprintf(f, "# --- live settings\n");
    for (int i = 0; i < P_COUNT; ++i) {
        if (!kParamInfo[i].persist) continue;
        std::fprintf(f, "%-20s = %g   # %s\n", kParamInfo[i].name, (double)p.get((ParamId)i), kParamInfo[i].help);
    }
    std::fprintf(f, "\n# --- fixed notches from ring-out: hz  gain_db  q\n");
    int n = p.fixed_count.load();
    for (int i = 0; i < n; ++i)
        std::fprintf(f, "fixed_notch = %.1f %.1f %.2f\n", (double)p.fixed[i].hz.load(),
                     (double)p.fixed[i].gain_db.load(), (double)p.fixed[i].q.load());
    std::fflush(f);
    fsync(fileno(f));
    std::fclose(f);
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace imp
