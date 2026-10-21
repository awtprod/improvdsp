// improvdsp - single-mic speech reinforcement processor for a Raspberry Pi
// with a USB audio interface (Behringer UMC22 or similar).
//
//   improvdsp [-c CONFIG]                       run live (ALSA + control page)
//   improvdsp --offline IN.wav OUT.wav [...]    process a recording, print what it caught
//   improvdsp --list-devices                    show ALSA cards
//   --set name=value                            override any parameter (repeatable)
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(__SSE__)
#include <xmmintrin.h>
#endif

#include "audio_alsa.hpp"
#include "config.hpp"
#include "detector.hpp"
#include "dsp.hpp"
#include "params.hpp"
#include "recorder.hpp"
#include "wav.hpp"
#include "web.hpp"

using namespace imp;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running = false; }

static void flush_denormals() {
#if defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ull << 24);  // FZ
    asm volatile("msr fpcr, %0" ::"r"(fpcr));
#elif defined(__x86_64__) || defined(__SSE__)
    _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ + DAZ
#endif
}

static bool apply_set(Params& P, const std::string& kv) {
    size_t eq = kv.find('=');
    if (eq == std::string::npos) return false;
    int id = Params::find(kv.substr(0, eq));
    if (id < 0) return false;
    std::string v = kv.substr(eq + 1);
    char* end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    if (v.empty() || *end != '\0') return false;
    return P.set((ParamId)id, f);
}

static void usage() {
    std::printf(
        "improvdsp - speech reinforcement with AGC and feedback control\n\n"
        "  improvdsp [-c CONFIG] [--set name=value ...]\n"
        "  improvdsp --offline IN.wav OUT.wav [--channel N] [-c CONFIG] [--set name=value ...]\n"
        "  improvdsp --list-devices\n"
        "  improvdsp --repair IN.wav OUT.wav   fix the header of an interrupted recording\n"
        "  improvdsp --write-config PATH   write a config file with all defaults\n"
        "  improvdsp --params            list every parameter with its range\n\n"
        "Default config: /boot/firmware/improvdsp.conf if it exists, else ./improvdsp.conf\n");
}

static int run_offline(Params& P, Status& S, const StartupSettings& st, const std::string& in_path,
                       const std::string& out_path, int channel) {
    WavData in;
    std::string err;
    if (!wav_read(in_path, in, &err)) { std::fprintf(stderr, "%s: %s\n", in_path.c_str(), err.c_str()); return 1; }
    if (in.recovered)
        std::printf("note: %s was cut off (header said %.1f s); recovered %.1f s\n", in_path.c_str(),
                    in.header_seconds, in.frames() / (double)in.sample_rate);
    if (channel < 0 || channel >= in.channels) channel = 0;
    const int fs = in.sample_rate;
    const int block = std::max(16, std::min(st.period, 4096));
    Engine eng(P, S);
    FeedbackDetector det(P, S);
    eng.init(fs, block);
    det.init(fs);
    eng.analysis = &det.ring;

    WavData out;
    out.sample_rate = fs;
    out.channels = 2;
    out.ch.assign(2, std::vector<float>(in.frames(), 0.f));
    std::vector<float> zero(block, 0.f);
    double sum_agc = 0, max_int = 0;
    size_t blocks = 0;
    for (size_t pos = 0; pos < in.frames(); pos += block) {
        int n = (int)std::min<size_t>(block, in.frames() - pos);
        eng.process(&in.ch[channel][pos], &out.ch[0][pos], &out.ch[1][pos], n);
        det.poll();
        sum_agc += S.agc_db.load();
        max_int = std::max(max_int, (double)S.interlock_db.load());
        ++blocks;
    }
    if (!wav_write(out_path, out)) { std::fprintf(stderr, "cannot write %s\n", out_path.c_str()); return 1; }

    std::printf("processed %.1f s at %d Hz (channel %d) -> %s\n", in.frames() / (double)fs, fs, channel, out_path.c_str());
    auto ev = det.recent_events();
    std::printf("feedback events: %llu\n", (unsigned long long)det.total_detections());
    for (auto& e : ev)
        std::printf("  t=%7.2fs  %7.1f Hz  level %6.1f dBFS  -> slot %d %s %.1f dB%s\n", e.t_s, e.hz, e.level_db,
                    e.slot + 1, e.deepened ? "deepened to" : "new notch", e.gain_db,
                    e.external ? "  (steady outside tone: undone, ignored)" : "");
    std::printf("mean AGC %.1f dB, max interlock %.1f dB, final total gain %.1f dB\n",
                blocks ? sum_agc / blocks : 0.0, max_int, S.total_gain_db.load());
    return 0;
}

int main(int argc, char** argv) {
    std::string config_path = access("/boot/firmware/improvdsp.conf", F_OK) == 0 ? "/boot/firmware/improvdsp.conf"
                                                                                : "improvdsp.conf";
    std::string off_in, off_out;
    std::vector<std::string> sets;
    int channel = -1;
    bool offline = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-c" || a == "--config") && i + 1 < argc) config_path = argv[++i];
        else if (a == "--offline" && i + 2 < argc) { offline = true; off_in = argv[++i]; off_out = argv[++i]; }
        else if (a == "--set" && i + 1 < argc) sets.push_back(argv[++i]);
        else if (a == "--channel" && i + 1 < argc) channel = std::atoi(argv[++i]);
        else if (a == "--list-devices") { list_alsa_devices(); return 0; }
        else if (a == "--repair" && i + 2 < argc) {
            // Rewrite an interrupted recording with a correct header (no audio is changed).
            WavData w;
            std::string e;
            if (!wav_read(argv[i + 1], w, &e)) { std::fprintf(stderr, "%s: %s\n", argv[i + 1], e.c_str()); return 1; }
            double secs = w.frames() / (double)w.sample_rate;
            if (w.recovered) std::printf("%s was cut off: header said %.1f s, file holds %.1f s\n", argv[i + 1], w.header_seconds, secs);
            else std::printf("%s looks complete (%.1f s); writing a copy anyway\n", argv[i + 1], secs);
            if (!wav_write(argv[i + 2], w)) { std::fprintf(stderr, "cannot write %s\n", argv[i + 2]); return 1; }
            std::printf("wrote %s\n", argv[i + 2]);
            return 0;
        }
        else if (a == "--write-config" && i + 1 < argc) {
            static Params P0;
            StartupSettings s0;
            bool ok = save_config(argv[++i], P0, s0);
            std::printf("%s %s\n", ok ? "wrote" : "could not write", argv[i]);
            return ok ? 0 : 1;
        }
        else if (a == "--params") {
            for (int k = 0; k < P_COUNT; ++k)
                std::printf("%-20s default %-7g range %g..%g  %s\n", kParamInfo[k].name, (double)kParamInfo[k].def,
                            (double)kParamInfo[k].lo, (double)kParamInfo[k].hi, kParamInfo[k].help);
            return 0;
        } else { usage(); return a == "-h" || a == "--help" ? 0 : 2; }
    }

    flush_denormals();
    static Params P;
    static Status S;
    StartupSettings st;
    std::string warn;
    bool have_cfg = load_config(config_path, P, st, &warn);
    if (!warn.empty()) std::fprintf(stderr, "%s", warn.c_str());
    for (auto& kv : sets)
        if (!apply_set(P, kv)) std::fprintf(stderr, "ignored --set %s\n", kv.c_str());

    if (offline) return run_offline(P, S, st, off_in, off_out, channel >= 0 ? channel : st.input_channel);

    std::fprintf(stderr, "improvdsp: config %s%s\n", config_path.c_str(), have_cfg ? "" : " (not found, using defaults)");
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) std::fprintf(stderr, "note: mlockall failed (not fatal)\n");

    Engine eng(P, S);
    FeedbackDetector det(P, S);
    Recorder rec(P, S);
    eng.init(st.sample_rate, 8192);
    det.init(st.sample_rate);
    eng.analysis = &det.ring;
    eng.recorder = &rec.ring;
    rec.start(st.record_dir, st.sample_rate, st.record_segment_min);

    std::thread det_thread([&] {
        flush_denormals();
        while (g_running) {
            if (det.poll() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    });

    WebServer web(P, S, det, st, config_path);
    std::string err;
    if (web.start(err)) std::fprintf(stderr, "control page: http://<pi-address>:%d/\n", st.web_port);
    else std::fprintf(stderr, "web: %s\n", err.c_str());

    sched_param sp{};
    sp.sched_priority = st.rt_priority;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        std::fprintf(stderr, "note: could not get real-time priority (run as the systemd service)\n");

    std::string last_err;
    while (g_running) {
        int rc = run_alsa(st, eng, S, g_running, err);
        if (!g_running) break;
        if (err != last_err) std::fprintf(stderr, "audio: %s - retrying every 2 s\n", err.c_str());
        last_err = err;
        (void)rc;
        for (int i = 0; i < 20 && g_running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::fprintf(stderr, "improvdsp: stopping\n");
    web.stop();
    det_thread.join();
    rec.stop();
    return 0;
}
