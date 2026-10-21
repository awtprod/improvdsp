// sim.cpp - closed-loop acoustic feedback simulator for testing improvdsp.
//
// mic[n] = source[n] + room_noise[n] + room( speaker[n - device_latency] )
// speaker = Engine(mic)  (front output)
//
// The room path is a sparse "velvet noise" impulse response (direct sound,
// early reflections, exponentially decaying diffuse tail with the given RT60)
// after a speaker/mic band-pass. It is normalised so its loudest frequency has
// 0 dB gain, which makes Engine gain_db roughly the open-loop gain margin:
// without any feedback control the system should go unstable near 0 dB.
//
//   sim --src program.wav [--sweep -4:10:1] [--gain G] [--rt60 1.3] [--seed N]
//       [--noloop] [--trace] [--out out.wav] [--set name=value ...]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "detector.hpp"
#include "dsp.hpp"
#include "fft.hpp"
#include "params.hpp"
#include "wav.hpp"

using namespace imp;

struct Room {
    std::vector<int> delay;
    std::vector<float> amp;
    Biquad hp, lp;
    int max_delay = 0;
};

static Room make_room(double fs, double rt60, unsigned seed, int dev_latency) {
    Room r;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const double direct_s = 3.5 / 343.0;  // mic ~3.5 m from the speaker
    auto tap = [&](double t, double a) {
        r.delay.push_back(dev_latency + (int)std::lround(t * fs));
        r.amp.push_back((float)a);
    };
    tap(direct_s, 0.35);  // cardioid rear rejection of the direct sound (~ -9 dB)
    // early reflections: floor, side walls, ceiling, back wall
    const double er[][2] = {{0.0125, 0.45}, {0.017, 0.40}, {0.021, 0.35}, {0.026, 0.30}, {0.034, 0.30},
                            {0.041, 0.25}, {0.055, 0.22}, {0.105, 0.20}};
    for (auto& e : er) tap(e[0], (U(rng) < 0.5 ? -1 : 1) * e[1]);
    // diffuse tail: velvet noise, 2000 pulses/s, decaying 60 dB per rt60, truncated at -40 dB
    const double density = 2000.0, len = rt60 * 40.0 / 60.0, t0 = 0.02;
    const double seg = 1.0 / density;
    double tail_energy = 0;
    std::vector<std::pair<double, double>> tail;
    for (double t = t0; t < t0 + len; t += seg) {
        double tt = t + U(rng) * seg;
        double a = (U(rng) < 0.5 ? -1 : 1) * std::exp(-6.9078 * (tt - t0) / rt60);
        tail.push_back({tt, a});
        tail_energy += a * a;
    }
    // reverberant energy ~ 2x the direct energy (mic is beyond critical distance)
    double scale = std::sqrt(2.0 * 0.35 * 0.35 / tail_energy);
    for (auto& p : tail) tap(p.first, p.second * scale);
    for (int d : r.delay) r.max_delay = std::max(r.max_delay, d);
    r.hp.set_highpass(fs, 80, 0.7071);
    r.lp.set_lowpass(fs, 9000, 0.7071);
    return r;
}

// Max |H(f)| between 100 Hz and 8 kHz of the band-passed room path.
static double room_peak_gain(Room& r, double fs) {
    int N = 1;
    while (N < r.max_delay + 4096) N <<= 1;
    std::vector<std::complex<double>> h(N, {0, 0});
    Biquad hp = r.hp, lp = r.lp;
    hp.reset(); lp.reset();
    std::vector<double> imp_resp(N, 0.0);
    // impulse response of the shaping filters convolved with the taps
    std::vector<double> shp(4096);
    for (int i = 0; i < 4096; ++i) shp[i] = lp.process(hp.process(i == 0 ? 1.0 : 0.0));
    for (size_t k = 0; k < r.delay.size(); ++k)
        for (int i = 0; i < 4096; ++i) {
            int idx = r.delay[k] + i;
            if (idx < N) imp_resp[idx] += r.amp[k] * shp[i];
        }
    for (int i = 0; i < N; ++i) h[i] = {imp_resp[i], 0};
    FFT fft(N);
    fft.forward(h);
    double mx = 0;
    for (int k = (int)(100.0 * N / fs); k <= (int)(8000.0 * N / fs); ++k) mx = std::max(mx, std::abs(h[k]));
    return mx;
}

struct Result {
    double gain, tail_db, speech_out_db, lim_frac_end, max_interlock;
    unsigned events, refunds, runaways;
    int notches;
    bool stable;
};

static Result run_once(const std::vector<float>& src, double fs, const std::vector<std::string>& sets, double gain_db,
                       double rt60, unsigned seed, bool noloop, bool trace, std::vector<float>* out_front,
                       std::vector<float>* out_mic, int block, int dev_latency) {
    static Params P;
    static Status S;
    P.reset();
    P.fixed_count = 0;
    P.fixed_version++;
    for (auto& d : P.dyn) { d.gain_db = 0; d.hz = 1000; }
    S.howl_events = 0;
    S.howl_suspect = 0;
    S.interlock_refunds = 0;
    S.runaway_events = 0;
    P.set(P_ceiling_db, 40);
    for (auto& kv : sets) {
        size_t eq = kv.find('=');
        int id = Params::find(kv.substr(0, eq));
        if (id >= 0) P.set((ParamId)id, std::stof(kv.substr(eq + 1)));
        else std::fprintf(stderr, "unknown param %s\n", kv.c_str());
    }
    P.set(P_gain_db, (float)gain_db);

    Engine eng(P, S);
    FeedbackDetector det(P, S);
    eng.init(fs, block);
    det.init(fs);
    eng.analysis = &det.ring;

    Room room = make_room(fs, rt60, seed, dev_latency);
    double norm = 1.0 / room_peak_gain(room, fs);
    for (auto& a : room.amp) a = (float)(a * norm);

    const size_t total = src.size();
    int hist_len = 1;
    while (hist_len < room.max_delay + block + 8) hist_len <<= 1;
    std::vector<float> hist(hist_len, 0.f);  // shaped speaker signal history
    const int hmask = hist_len - 1;
    size_t hw = 0;  // write position (sample index)

    std::mt19937 rng(seed + 99);
    std::normal_distribution<float> N01(0.f, 1.f);
    const float noise_amp = std::pow(10.f, -72.f / 20.f);
    float pink = 0;

    std::vector<float> mic(block), of(block), ob(block);
    if (out_front) out_front->assign(total, 0.f);
    if (out_mic) out_mic->assign(total, 0.f);

    // segment bookkeeping: last 3 s of src are silence by construction
    const size_t tail_start = total - (size_t)(2.0 * fs);
    const size_t speech_end = total - (size_t)(3.0 * fs);
    double tail_e = 0, speech_e = 0, ref_e = 0;
    size_t tail_n = 0, speech_n = 0, lim_n = 0, lim_tot = 0;
    double max_int = 0;

    for (size_t pos = 0; pos < total; pos += block) {
        int n = (int)std::min<size_t>(block, total - pos);
        for (int i = 0; i < n; ++i) {
            size_t t = pos + i;
            double fb = 0;
            if (!noloop)
                for (size_t k = 0; k < room.delay.size(); ++k) {
                    long idx = (long)t - room.delay[k];
                    if (idx >= 0) fb += room.amp[k] * hist[idx & hmask];
                }
            pink = 0.98f * pink + 0.02f * N01(rng);
            mic[i] = src[t] + noise_amp * 5.f * pink + (float)fb;
        }
        eng.process(mic.data(), of.data(), ob.data(), n);
        det.poll();
        for (int i = 0; i < n; ++i) {
            size_t t = pos + i;
            double s = room.lp.process(room.hp.process(of[i]));
            hist[(hw++) & hmask] = (float)s;
            if (out_front) (*out_front)[t] = of[i];
            if (out_mic) (*out_mic)[t] = mic[i];
            if (t >= tail_start) { tail_e += (double)of[i] * of[i]; ++tail_n; }
            else if (t < speech_end) { speech_e += (double)of[i] * of[i]; ref_e += (double)src[t] * src[t]; ++speech_n; }
        }
        float lim = S.lim_db.load();
        if (pos + n > total - (size_t)(8.0 * fs)) { ++lim_tot; if (lim < -1.0f) ++lim_n; }
        max_int = std::max(max_int, (double)S.interlock_db.load());
        if (trace && (pos / block) % (size_t)(0.5 * fs / block) == 0)
            std::printf("  t=%6.2f speech=%d voice=%6.1f noise=%6.1f agc=%5.1f exp=%5.1f total=%5.1f lim=%5.1f int=%4.1f ev=%u\n",
                        pos / fs, S.speech.load(), S.speech_dbfs.load(), S.noise_dbfs.load(), S.agc_db.load(),
                        S.exp_db.load(), S.total_gain_db.load(), S.lim_db.load(), S.interlock_db.load(),
                        S.howl_events.load());
    }
    Result r;
    r.gain = gain_db;
    double sp = 10 * std::log10(speech_e / std::max<size_t>(1, speech_n) + 1e-20);
    r.tail_db = 10 * std::log10(tail_e / std::max<size_t>(1, tail_n) + 1e-20) - sp;
    r.speech_out_db = sp - 10 * std::log10(ref_e / std::max<size_t>(1, speech_n) + 1e-20);
    r.lim_frac_end = lim_tot ? (double)lim_n / lim_tot : 0;
    r.max_interlock = max_int;
    r.events = S.howl_events.load();
    r.refunds = S.interlock_refunds.load();
    r.runaways = S.runaway_events.load();
    r.notches = 0;
    for (int i = 0; i < kMaxDynNotches; ++i)
        if (P.dyn[i].gain_db.load() < -0.01f) ++r.notches;
    // Stable = the output dies away in the final silence and the limiter is not pinned.
    r.stable = r.tail_db < -30.0 && r.lim_frac_end < 0.05;
    return r;
}

int main(int argc, char** argv) {
    std::string src_path, out_path;
    std::vector<std::string> sets;
    double gain = 0, rt60 = 1.3, lo = 0, hi = 0, step = 1;
    unsigned seed = 1;
    bool sweep = false, noloop = false, trace = false;
    int block = 128, dev_latency = 3 * 128 + 72;  // (prefill 2 + 1) periods + ~1.5 ms converters
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(i + 1 < argc ? argv[++i] : ""); };
        if (a == "--src") src_path = next();
        else if (a == "--out") out_path = next();
        else if (a == "--set") sets.push_back(next());
        else if (a == "--gain") gain = std::stod(next());
        else if (a == "--rt60") rt60 = std::stod(next());
        else if (a == "--seed") seed = (unsigned)std::stoul(next());
        else if (a == "--noloop") noloop = true;
        else if (a == "--trace") trace = true;
        else if (a == "--latency") dev_latency = std::stoi(next());
        else if (a == "--sweep") {
            sweep = true;
            std::string s = next();
            std::sscanf(s.c_str(), "%lf:%lf:%lf", &lo, &hi, &step);
        } else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }
    WavData w;
    std::string err;
    if (!wav_read(src_path, w, &err)) { std::fprintf(stderr, "%s: %s\n", src_path.c_str(), err.c_str()); return 1; }
    const double fs = w.sample_rate;
    std::vector<float> src = w.ch[0];
    src.resize(src.size() + (size_t)(3.0 * fs), 0.f);  // 3 s of silence at the end

    auto print = [](const Result& r) {
        std::printf("gain %+5.1f dB | %-8s | tail %6.1f dB | out/src %+5.1f dB | lim@end %3.0f%% | events %3u (undone %u) | notches %2d | runaway %u | interlock max %4.1f\n",
                    r.gain, r.stable ? "stable" : "UNSTABLE", r.tail_db, r.speech_out_db, r.lim_frac_end * 100, r.events,
                    r.refunds, r.notches, r.runaways, r.max_interlock);
        std::fflush(stdout);
    };
    if (sweep) {
        double best = -1e9;
        bool failed = false;
        for (double g = lo; g <= hi + 1e-9; g += step) {
            Result r = run_once(src, fs, sets, g, rt60, seed, noloop, false, nullptr, nullptr, block, dev_latency);
            print(r);
            if (r.stable && !failed) best = g;
            if (!r.stable) failed = true;
        }
        if (best > -1e8) std::printf("MAX STABLE GAIN: %+.1f dB\n", best);
        else std::printf("MAX STABLE GAIN: below %+.1f dB\n", lo);
    } else {
        std::vector<float> of, mic;
        Result r = run_once(src, fs, sets, gain, rt60, seed, noloop, trace, &of, &mic, block, dev_latency);
        print(r);
        if (!out_path.empty()) {
            WavData o;
            o.sample_rate = (int)fs;
            o.channels = 2;
            o.ch = {mic, of};
            wav_write(out_path, o);
            std::printf("wrote %s (L = mic, R = speaker)\n", out_path.c_str());
        }
    }
    return 0;
}
