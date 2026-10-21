// audio_alsa.cpp - full-duplex ALSA loop.
//
// Capture and playback are opened on the same card (one clock, no drift),
// linked so they start together, and run with a small playback prefill.
// Round-trip latency ~= (prefill + 1) * period / rate, plus converter delay.
#include "audio_alsa.hpp"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace imp {

namespace {

struct Pcm {
    snd_pcm_t* h = nullptr;
    snd_pcm_format_t fmt = SND_PCM_FORMAT_UNKNOWN;
    unsigned channels = 0;
    snd_pcm_uframes_t period = 0, buffer = 0;
    int bytes_per_sample = 0;
    ~Pcm() { if (h) snd_pcm_close(h); }
};

int setup(Pcm& p, unsigned rate, unsigned want_ch, unsigned min_ch, snd_pcm_uframes_t period,
          unsigned nperiods, bool playback, std::string& err) {
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    int rc;
    auto fail = [&](const char* what) { err = std::string(what) + ": " + snd_strerror(rc); return rc; };
    if ((rc = snd_pcm_hw_params_any(p.h, hw)) < 0) return fail("hw_params_any");
    if ((rc = snd_pcm_hw_params_set_access(p.h, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) return fail("set_access");
    snd_pcm_hw_params_set_rate_resample(p.h, hw, 0);
    const snd_pcm_format_t fmts[] = {SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S16_LE,
                                     SND_PCM_FORMAT_FLOAT_LE};
    p.fmt = SND_PCM_FORMAT_UNKNOWN;
    for (auto f : fmts)
        if (snd_pcm_hw_params_test_format(p.h, hw, f) == 0) { p.fmt = f; break; }
    if (p.fmt == SND_PCM_FORMAT_UNKNOWN) { rc = -EINVAL; return fail("no supported sample format"); }
    if ((rc = snd_pcm_hw_params_set_format(p.h, hw, p.fmt)) < 0) return fail("set_format");
    unsigned ch = want_ch;
    if (snd_pcm_hw_params_test_channels(p.h, hw, ch) != 0) {
        snd_pcm_hw_params_get_channels_min(hw, &ch);
        if (ch < min_ch) ch = min_ch;
    }
    if ((rc = snd_pcm_hw_params_set_channels(p.h, hw, ch)) < 0) return fail("set_channels");
    unsigned r = rate;
    if ((rc = snd_pcm_hw_params_set_rate_near(p.h, hw, &r, nullptr)) < 0) return fail("set_rate");
    if (r != rate) { rc = -EINVAL; err = "device cannot run at " + std::to_string(rate) + " Hz (offers " + std::to_string(r) + ")"; return rc; }
    snd_pcm_uframes_t per = period;
    if ((rc = snd_pcm_hw_params_set_period_size_near(p.h, hw, &per, nullptr)) < 0) return fail("set_period_size");
    snd_pcm_uframes_t buf = per * nperiods;
    if ((rc = snd_pcm_hw_params_set_buffer_size_near(p.h, hw, &buf)) < 0) return fail("set_buffer_size");
    if ((rc = snd_pcm_hw_params(p.h, hw)) < 0) return fail("hw_params");
    snd_pcm_hw_params_get_period_size(hw, &p.period, nullptr);
    snd_pcm_hw_params_get_buffer_size(hw, &p.buffer);
    p.channels = ch;
    p.bytes_per_sample = snd_pcm_format_physical_width(p.fmt) / 8;

    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(p.h, sw);
    snd_pcm_uframes_t boundary = 0;
    snd_pcm_sw_params_get_boundary(sw, &boundary);
    snd_pcm_sw_params_set_start_threshold(p.h, sw, boundary);  // we start explicitly
    snd_pcm_sw_params_set_avail_min(p.h, sw, p.period);
    if (playback) snd_pcm_sw_params_set_stop_threshold(p.h, sw, p.buffer);
    if ((rc = snd_pcm_sw_params(p.h, sw)) < 0) return fail("sw_params");
    return 0;
}

inline float read_sample(const uint8_t* q, snd_pcm_format_t fmt) {
    switch (fmt) {
        case SND_PCM_FORMAT_S16_LE: { int16_t v; std::memcpy(&v, q, 2); return v / 32768.f; }
        case SND_PCM_FORMAT_S24_3LE: { int32_t v = (q[0] << 8) | (q[1] << 16) | (q[2] << 24); return (v >> 8) / 8388608.f; }
        case SND_PCM_FORMAT_S32_LE: { int32_t v; std::memcpy(&v, q, 4); return v / 2147483648.f; }
        case SND_PCM_FORMAT_FLOAT_LE: { float v; std::memcpy(&v, q, 4); return v; }
        default: return 0.f;
    }
}

inline void write_sample(uint8_t* q, snd_pcm_format_t fmt, float x) {
    x = std::clamp(x, -1.f, 1.f);
    switch (fmt) {
        case SND_PCM_FORMAT_S16_LE: { int16_t v = (int16_t)std::lround(x * 32767.f); std::memcpy(q, &v, 2); break; }
        case SND_PCM_FORMAT_S24_3LE: { int32_t v = (int32_t)std::lround(x * 8388607.f); q[0] = v & 255; q[1] = (v >> 8) & 255; q[2] = (v >> 16) & 255; break; }
        case SND_PCM_FORMAT_S32_LE: { int32_t v = (int32_t)std::lround((double)x * 2147483647.0); std::memcpy(q, &v, 4); break; }
        case SND_PCM_FORMAT_FLOAT_LE: std::memcpy(q, &x, 4); break;
        default: break;
    }
}

}  // namespace

int run_alsa(const StartupSettings& s, Engine& eng, Status& S, std::atomic<bool>& running, std::string& err) {
    Pcm cap, pb;
    int rc;
    if ((rc = snd_pcm_open(&cap.h, s.device.c_str(), SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        err = "open capture '" + s.device + "': " + snd_strerror(rc);
        if (rc == -EBUSY) err += " (is PipeWire/PulseAudio holding the device?)";
        return rc;
    }
    if ((rc = snd_pcm_open(&pb.h, s.device.c_str(), SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        err = "open playback '" + s.device + "': " + snd_strerror(rc);
        return rc;
    }
    const unsigned rate = (unsigned)s.sample_rate;
    const int prefill = std::max(1, s.prefill);
    if ((rc = setup(cap, rate, 2, (unsigned)s.input_channel + 1, (snd_pcm_uframes_t)s.period, 4, false, err)) < 0) return rc;
    if ((rc = setup(pb, rate, 2, 1, cap.period, (unsigned)prefill + 2, true, err)) < 0) return rc;
    if (pb.period != cap.period) {
        err = "capture/playback period mismatch (" + std::to_string(cap.period) + " vs " + std::to_string(pb.period) + ")";
        return -EINVAL;
    }
    if ((unsigned)s.input_channel >= cap.channels) {
        err = "input_channel " + std::to_string(s.input_channel) + " but device has " + std::to_string(cap.channels) + " capture channels";
        return -EINVAL;
    }
    const snd_pcm_uframes_t P = cap.period;
    if (P > 8192) { err = "period too large (" + std::to_string(P) + ")"; return -EINVAL; }
    std::fprintf(stderr, "audio: %s  %u Hz  period %lu  capture %u ch %s  playback %u ch %s  prefill %d\n",
                 s.device.c_str(), rate, (unsigned long)P, cap.channels, snd_pcm_format_name(cap.fmt), pb.channels,
                 snd_pcm_format_name(pb.fmt), prefill);

    std::vector<uint8_t> inraw(P * cap.channels * cap.bytes_per_sample);
    std::vector<uint8_t> outraw(P * pb.channels * pb.bytes_per_sample);
    std::vector<uint8_t> silence(outraw.size(), 0);
    std::vector<float> in(P), outF(P), outB(P);

    const bool linked = snd_pcm_link(cap.h, pb.h) == 0;
    std::fprintf(stderr, "audio: capture/playback %s\n", linked ? "linked (start together)" : "not linked (started back-to-back)");

    auto start = [&]() -> int {
        // (drop/prepare act on the whole group when linked; repeating them is harmless)
        snd_pcm_drop(cap.h);
        snd_pcm_drop(pb.h);
        int r;
        if ((r = snd_pcm_prepare(cap.h)) < 0) return r;
        if ((r = snd_pcm_prepare(pb.h)) < 0) return r;
        for (int i = 0; i < prefill; ++i)
            if ((r = (int)snd_pcm_writei(pb.h, silence.data(), P)) < 0) return r;
        if ((r = snd_pcm_start(cap.h)) < 0) return r;
        if (!linked && (r = snd_pcm_start(pb.h)) < 0) return r;
        return 0;
    };

    if ((rc = start()) < 0) { err = std::string("start: ") + snd_strerror(rc); return rc; }
    eng.restart(true);  // (re)connected: 1 s fade-in, no stale delay-line audio
    S.device_ok.store(1);
    S.latency_ms.store((float)((prefill + 1) * (double)P / rate * 1000.0));

    const double period_s = (double)P / rate;
    double cpu_avg = 0;
    // Dropout bookkeeping: each one restarts the streams with a 20 ms fade; a
    // burst of them gets logged so you know to raise `period` or `prefill`.
    auto xrun_window_start = std::chrono::steady_clock::now();
    int xruns_in_window = 0;
    auto on_xrun = [&]() -> int {
        S.xruns.fetch_add(1);
        auto now = std::chrono::steady_clock::now();
        if (now - xrun_window_start > std::chrono::seconds(60)) { xrun_window_start = now; xruns_in_window = 0; }
        if (++xruns_in_window == 10)
            std::fprintf(stderr, "audio: 10 dropouts within a minute - raise period (e.g. 256) or prefill (e.g. 3)\n");
        int r = start();
        if (r >= 0) eng.restart(false);
        return r;
    };
    const int in_stride = cap.channels * cap.bytes_per_sample;
    const int out_stride = pb.channels * pb.bytes_per_sample;

    while (running.load(std::memory_order_relaxed)) {
        snd_pcm_sframes_t n = snd_pcm_readi(cap.h, inraw.data(), P);
        if (n == -EPIPE || n == -ESTRPIPE || n == -EAGAIN || (n >= 0 && n < (snd_pcm_sframes_t)P)) {
            if (n == -EAGAIN) continue;
            if ((rc = on_xrun()) < 0) { err = std::string("recover: ") + snd_strerror(rc); break; }
            continue;
        }
        if (n < 0) { rc = (int)n; err = std::string("read: ") + snd_strerror(rc); break; }

        auto t0 = std::chrono::steady_clock::now();
        for (snd_pcm_uframes_t i = 0; i < P; ++i)
            in[i] = read_sample(&inraw[i * in_stride + s.input_channel * cap.bytes_per_sample], cap.fmt);
        eng.process(in.data(), outF.data(), outB.data(), (int)P);
        for (snd_pcm_uframes_t i = 0; i < P; ++i) {
            uint8_t* q = &outraw[i * out_stride];
            write_sample(q, pb.fmt, outF[i]);
            if (pb.channels > 1) write_sample(q + pb.bytes_per_sample, pb.fmt, outB[i]);
            for (unsigned c = 2; c < pb.channels; ++c) write_sample(q + c * pb.bytes_per_sample, pb.fmt, 0.f);
        }
        double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        cpu_avg += (used / period_s - cpu_avg) * 0.01;
        S.cpu_pct.store((float)(cpu_avg * 100.0), std::memory_order_relaxed);

        snd_pcm_sframes_t w = snd_pcm_writei(pb.h, outraw.data(), P);
        if (w == -EPIPE || w == -ESTRPIPE || (w >= 0 && w < (snd_pcm_sframes_t)P)) {
            if ((rc = on_xrun()) < 0) { err = std::string("recover: ") + snd_strerror(rc); break; }
            continue;
        }
        if (w < 0) { rc = (int)w; err = std::string("write: ") + snd_strerror(rc); break; }
    }

    S.device_ok.store(0);
    if (linked) snd_pcm_unlink(cap.h);
    snd_pcm_drop(cap.h);
    snd_pcm_drop(pb.h);
    return running.load() ? (rc < 0 ? rc : -EIO) : 0;
}

void list_alsa_devices() {
    int card = -1;
    std::printf("Sound cards (use device = hw:<id> in the config):\n");
    while (snd_card_next(&card) == 0 && card >= 0) {
        char* name = nullptr;
        char* longname = nullptr;
        snd_card_get_name(card, &name);
        snd_card_get_longname(card, &longname);
        char ctlname[32];
        std::snprintf(ctlname, sizeof ctlname, "hw:%d", card);
        snd_ctl_t* ctl = nullptr;
        std::string id = "?";
        if (snd_ctl_open(&ctl, ctlname, 0) == 0) {
            snd_ctl_card_info_t* info;
            snd_ctl_card_info_alloca(&info);
            if (snd_ctl_card_info(ctl, info) == 0) id = snd_ctl_card_info_get_id(info);
            snd_ctl_close(ctl);
        }
        std::printf("  card %d  id=%-10s  hw:%s   %s\n", card, id.c_str(), id.c_str(), longname ? longname : (name ? name : ""));
        free(name);
        free(longname);
    }
}

}  // namespace imp
