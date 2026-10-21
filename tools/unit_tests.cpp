// unit_tests.cpp - checks of the individual DSP blocks.
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "detector.hpp"
#include "dsp.hpp"
#include "fft.hpp"
#include "params.hpp"

using namespace imp;
static int fails = 0;
#define CHECK(cond, ...) do { bool ok_ = (cond); std::printf("%s  ", ok_ ? "PASS" : "FAIL"); std::printf(__VA_ARGS__); std::printf("\n"); if (!ok_) ++fails; } while (0)

static double tone_gain_db(Biquad b, double fs, double f) {
    b.reset();
    double e_in = 0, e_out = 0;
    for (int i = 0; i < 48000; ++i) {
        double x = std::sin(2 * kPi * f * i / fs);
        double y = b.process(x);
        if (i > 24000) { e_in += x * x; e_out += y * y; }
    }
    return 10 * std::log10(e_out / e_in);
}

// Dominant frequency via a long FFT with parabolic interpolation.
static double peak_freq(const std::vector<double>& x, double fs, double* image_db, double expect_image_hz) {
    int N = 1 << 18;
    std::vector<std::complex<double>> X(N);
    for (int i = 0; i < N; ++i) {
        double w = 0.5 - 0.5 * std::cos(2 * kPi * i / N);
        X[i] = {i < (int)x.size() ? x[i] * w : 0.0, 0};
    }
    FFT f(N);
    f.forward(X);
    int kb = 1;
    for (int k = 1; k < N / 2; ++k) if (std::abs(X[k]) > std::abs(X[kb])) kb = k;
    double a = std::log(std::abs(X[kb - 1])), b = std::log(std::abs(X[kb])), c = std::log(std::abs(X[kb + 1]));
    double p = 0.5 * (a - c) / (a - 2 * b + c);
    if (image_db) {
        int ki = (int)std::lround(expect_image_hz * N / fs);
        double mi = 0;
        for (int k = ki - 3; k <= ki + 3; ++k) mi = std::max(mi, std::abs(X[k]));
        *image_db = 20 * std::log10(mi / std::abs(X[kb]));
    }
    return (kb + p) * fs / N;
}

int main() {
    const double fs = 48000;

    // --- High-pass 4th order @120 Hz
    Biquad h1, h2;
    h1.set_highpass(fs, 120, 0.54119610);
    h2.set_highpass(fs, 120, 1.30656296);
    auto hp_db = [&](double f) { return tone_gain_db(h1, fs, f) + tone_gain_db(h2, fs, f); };
    CHECK(std::fabs(hp_db(120) + 3.01) < 0.3, "HPF -3 dB at corner: %.2f dB", hp_db(120));
    CHECK(hp_db(60) < -23, "HPF 60 Hz (one octave below): %.1f dB (expect ~ -24)", hp_db(60));
    CHECK(std::fabs(hp_db(1000)) < 0.1, "HPF passband 1 kHz: %.2f dB", hp_db(1000));

    // --- Peaking notch: -6 dB, 1/10 octave at 1 kHz
    Biquad n;
    double q = q_from_octaves(fs, 1000, 0.1);
    n.set_peaking(fs, 1000, q, -6);
    CHECK(std::fabs(tone_gain_db(n, fs, 1000) + 6) < 0.1, "notch depth at 1 kHz: %.2f dB (Q %.1f)", tone_gain_db(n, fs, 1000), q);
    CHECK(tone_gain_db(n, fs, 1100) > -1.0, "notch 1.1 kHz barely touched: %.2f dB", tone_gain_db(n, fs, 1100));
    CHECK(tone_gain_db(n, fs, 1500) > -0.2, "notch 1.5 kHz untouched: %.2f dB", tone_gain_db(n, fs, 1500));

    // --- Frequency shifter: +4 Hz, image (f - 4 Hz) suppressed
    for (double f0 : {150.0, 1000.0, 5000.0}) {
        FreqShifter fsh;
        fsh.init(fs);
        std::vector<double> y(1 << 18);
        for (size_t i = 0; i < y.size() + 4800; ++i) {
            double v = fsh.process(std::sin(2 * kPi * f0 * i / fs), 4.0);
            if (i >= 4800) y[i - 4800] = v;
        }
        double img;
        double pf = peak_freq(y, fs, &img, f0 - 4.0);
        CHECK(std::fabs(pf - (f0 + 4.0)) < 0.2 && img < -30, "shift %5.0f Hz -> %.2f Hz (expect %.0f), image %.1f dB", f0, pf,
              f0 + 4.0, img);
    }

    // --- Detector: a steady 1234 Hz tone plus noise must be caught; speech-like harmonics must not
    {
        static Params P;
        static Status S;
        FeedbackDetector det(P, S);
        det.init(fs);
        std::vector<float> buf(512);
        unsigned seed = 1;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return ((seed >> 8) / 16777216.0f) - 0.5f; };
        for (int fr = 0; fr < 40; ++fr) {
            for (int i = 0; i < 512; ++i) {
                double t = (fr * 512 + i) / fs;
                buf[i] = (float)(0.05 * std::sin(2 * kPi * 1234.0 * t) + 0.002 * rnd());
            }
            det.ring.push(buf.data(), buf.size());
            det.poll();
        }
        auto ev = det.recent_events();
        CHECK(!ev.empty() && std::fabs(ev[0].hz - 1234) < 5, "detector catches steady tone: %zu event(s), first at %.1f Hz",
              ev.size(), ev.empty() ? 0.0 : ev[0].hz);

        FeedbackDetector det2(P, S);
        det2.init(fs);
        for (int i = 0; i < kMaxDynNotches; ++i) P.dyn[i].gain_db = 0;
        for (int fr = 0; fr < 200; ++fr) {
            for (int i = 0; i < 512; ++i) {
                double t = (fr * 512 + i) / fs;
                double f0 = 140 + 20 * std::sin(2 * kPi * 0.7 * t);  // gliding voice pitch
                double ph = 2 * kPi * (140 * t - 20 / (2 * kPi * 0.7) * std::cos(2 * kPi * 0.7 * t));
                double v = 0;
                for (int h = 1; h <= 25; ++h) v += std::sin(h * ph) / h * (h * f0 < 4000);
                buf[i] = (float)(0.03 * v + 0.002 * rnd());
            }
            det2.ring.push(buf.data(), buf.size());
            det2.poll();
        }
        CHECK(det2.total_detections() == 0, "harmonic 'voice' with gliding pitch: %llu false detections",
              (unsigned long long)det2.total_detections());
    }

    // --- Verification: a steady outside tone gets its notch undone and the gain refunded;
    //     a tone that dies away once notched (real feedback) keeps its notch.
    for (int scenario = 0; scenario < 2; ++scenario) {
        static Params P2;
        static Status S2;
        P2.reset();
        for (auto& d : P2.dyn) d.gain_db = 0;
        S2.interlock_refunds = 0;
        S2.howl_events = 0;
        FeedbackDetector det(P2, S2);
        det.init(fs);
        std::vector<float> buf(512);
        unsigned seed = 3;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return ((seed >> 8) / 16777216.0f) - 0.5f; };
        bool notched_once = false;
        for (int fr = 0; fr < 600; ++fr) {  // 6.4 s
            bool notched = false;
            for (auto& d : P2.dyn) notched |= d.gain_db.load() < -0.01f;
            notched_once |= notched;
            // scenario 1 ("feedback"): once a notch exists the tone collapses
            double amp = (scenario == 1 && notched_once) ? 0.0 : 0.02;
            for (int i = 0; i < 512; ++i) {
                double t = (fr * 512 + i) / fs;
                buf[i] = (float)(amp * std::sin(2 * kPi * 2100.0 * t) + 0.002 * rnd());
            }
            det.ring.push(buf.data(), buf.size());
            det.poll();
        }
        int active = 0;
        for (auto& d : P2.dyn) active += d.gain_db.load() < -0.01f;
        auto ev = det.recent_events();
        if (scenario == 0)
            CHECK(active == 0 && S2.interlock_refunds.load() >= 1 && !ev.empty() && ev[0].external,
                  "steady outside tone: notch undone (%d active), %u refund(s), %zu event(s) in 6.4 s", active,
                  S2.interlock_refunds.load(), ev.size());
        else
            CHECK(active == 1 && S2.interlock_refunds.load() == 0,
                  "tone that dies once notched (feedback): notch kept (%d active), %u refund(s)", active,
                  S2.interlock_refunds.load());
    }

    // --- Engine safety (fixes from code review)
    {
        static Params P3;
        static Status S3;
        auto fresh = [&]() {
            P3.reset();
            for (auto& d : P3.dyn) d.gain_db = 0;
            P3.fixed_count = 0;
            P3.fixed_version++;
            S3.howl_events = 0;
            S3.interlock_refunds = 0;
            S3.runaway_events = 0;
            S3.dsp_resets = 0;
        };
        const int B = 128;
        std::vector<float> in(B), of(B), ob(B);
        auto run = [&](Engine& e, double seconds, double amp, double hz, double* peak) {
            double pk = 0;
            for (int blk = 0; blk < (int)(seconds * fs / B); ++blk) {
                for (int i = 0; i < B; ++i) in[i] = (float)(amp * std::sin(2 * kPi * hz * (blk * B + i) / fs));
                e.process(in.data(), of.data(), ob.data(), B);
                for (int i = 0; i < B; ++i) pk = std::max(pk, (double)std::max(std::fabs(of[i]), std::fabs(ob[i])));
            }
            if (peak) *peak = pk;
        };

        // 1. +12 dB speaker trim and a hot signal must still respect the limiter (-3 dBFS = 0.708)
        fresh();
        P3.set(P_gain_db, 20); P3.set(P_ceiling_db, 40); P3.set(P_front_trim_db, 12); P3.set(P_fill_trim_db, 12);
        P3.set(P_agc_on, 0); P3.set(P_exp_on, 0);
        { Engine e(P3, S3); e.init(fs, B); double pk; run(e, 1.5, 0.3, 1000, &pk);
          CHECK(pk <= 0.709, "positive trim cannot exceed the limiter: output peak %.3f (limit 0.708)", pk); }

        // 2. NaN settings are rejected
        fresh();
        bool r1 = P3.set(P_gain_db, std::nanf("")), r2 = P3.set(P_limiter_dbfs, INFINITY);
        CHECK(!r1 && !r2 && P3.get(P_gain_db) == 0.f, "NaN/Inf parameter values rejected");

        // 3. A NaN input sample does not silence the output forever
        fresh();
        P3.set(P_agc_on, 0); P3.set(P_exp_on, 0);
        { Engine e(P3, S3); e.init(fs, B); run(e, 0.5, 0.1, 500, nullptr);
          in.assign(B, std::nanf("")); e.process(in.data(), of.data(), ob.data(), B);
          double pk; run(e, 1.5, 0.1, 500, &pk);
          CHECK(pk > 0.05 && std::isfinite(pk), "recovers after NaN input: output peak %.3f afterwards", pk); }

        // 4. Last-resort runaway guard: a howl pinned on the limiter gets pulled down, even in ring-out mode
        for (int ring = 0; ring < 2; ++ring) {
            fresh();
            P3.set(P_gain_db, 30); P3.set(P_ceiling_db, 40); P3.set(P_agc_on, 0); P3.set(P_exp_on, 0);
            P3.set(P_ringout_mode, ring);
            Engine e(P3, S3); e.init(fs, B);
            run(e, 8.0, 0.2, 1500, nullptr);
            double pk; run(e, 0.5, 0.2, 1500, &pk);
            // (a steady test tone is not a loop; the guard backs off until the limiter is nearly idle)
            CHECK(S3.interlock_db.load() >= 12 && S3.lim_db.load() > -1.5f && pk <= 0.709,
                  "%s: runaway guard took %.0f dB off; limiter now %.1f dB, peak %.3f",
                  ring ? "ring-out mode" : "show mode", S3.interlock_db.load(), S3.lim_db.load(), pk);
        }

        // 5. A sudden +14 dB gain change is slewed (<= 20 dB/s), never a jump
        fresh();
        P3.set(P_gain_db, 0); P3.set(P_ceiling_db, 40); P3.set(P_agc_on, 0); P3.set(P_exp_on, 0);
        { Engine e(P3, S3); e.init(fs, B); run(e, 0.5, 0.01, 700, nullptr);
          P3.set(P_gain_db, 14); run(e, 0.1, 0.01, 700, nullptr);
          float after100ms = S3.total_gain_db.load();
          CHECK(after100ms <= 2.2f, "+14 dB step is slewed: total gain %.1f dB after 100 ms", after100ms); }
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
