// dsp.hpp - signal processing building blocks and the real-time Engine.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

#include "params.hpp"
#include "ring.hpp"

namespace imp {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- Biquad
// Transposed direct form II, double precision (safe for low corner
// frequencies and for coefficient changes while running).
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    inline double process(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
    void set_identity() { b0 = 1; b1 = b2 = a1 = a2 = 0; }
    // RBJ audio-EQ-cookbook designs.
    void set_highpass(double fs, double f, double q);
    void set_lowpass(double fs, double f, double q);
    void set_peaking(double fs, double f, double q, double gain_db);
};

// Q for a peaking filter of the given bandwidth in octaves (RBJ, digital).
double q_from_octaves(double fs, double f, double bw_oct);

// ---------------------------------------------------------------- Hilbert
// 90-degree phase-difference network: two chains of 2nd-order allpass
// sections (Olli Niemitalo's polyphase IIR design). Output pair (i, q) is
// ~90 deg apart (+-0.7 deg) from 20 Hz to 22 kHz at 48 kHz, so i + j*q is
// an analytic signal with >36 dB image rejection - and almost no latency.
class Hilbert90 {
public:
    void reset();
    inline void process(double x, double& i_out, double& q_out) {
        // Path A (becomes Q after a one-sample delay).
        double a = x;
        for (int k = 0; k < 4; ++k) a = section(a, kA2[k], ax1_[k], ax2_[k], ay1_[k], ay2_[k]);
        // Path B (I).
        double b = x;
        for (int k = 0; k < 4; ++k) b = section(b, kB2[k], bx1_[k], bx2_[k], by1_[k], by2_[k]);
        q_out = a_delay_;
        a_delay_ = a;
        i_out = b;
    }

private:
    static inline double section(double x, double c, double& x1, double& x2, double& y1,
                                 double& y2) {
        // y[n] = c*(x[n] + y[n-2]) - x[n-2]
        double y = c * (x + y2) - x2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }
    static const double kA2[4];
    static const double kB2[4];
    double ax1_[4]{}, ax2_[4]{}, ay1_[4]{}, ay2_[4]{};
    double bx1_[4]{}, bx2_[4]{}, by1_[4]{}, by2_[4]{};
    double a_delay_ = 0;
};

// Single-sideband frequency shifter (shifts every frequency up by `hz`).
class FreqShifter {
public:
    void init(double fs) { fs_ = fs; hil_.reset(); phase_ = 0; }
    inline double process(double x, double hz) {
        double i, q;
        hil_.process(x, i, q);
        phase_ += 2.0 * kPi * hz / fs_;
        if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
        return i * std::cos(phase_) - q * std::sin(phase_);
    }

private:
    Hilbert90 hil_;
    double fs_ = 48000, phase_ = 0;
};

// ---------------------------------------------------------------- Delay
class DelayLine {
public:
    void init(int max_samples) { buf_.assign(max_samples + 1, 0.f); w_ = 0; }
    inline float process(float x, int delay) {
        int n = (int)buf_.size();
        if (delay >= n) delay = n - 1;
        buf_[w_] = x;
        int r = w_ - delay;
        if (r < 0) r += n;
        float y = buf_[r];
        if (++w_ >= n) w_ = 0;
        return y;
    }

private:
    std::vector<float> buf_;
    int w_ = 0;
};

// ---------------------------------------------------------------- Engine
// One mono microphone in, two outputs (front speaker, fill speaker).
//
//  mic -> HPF -> fixed notches -> automatic notches -> frequency shift
//      -> gain (manual + AGC, capped by ceiling - interlock) x expander
//      -> peak limiter -> [front delay/trim] [fill delay/trim]
//
// The raw mic signal is also copied into `analysis` for the feedback
// detector, which runs in another thread (or inline for offline tests).
class Engine {
public:
    Engine(Params& p, Status& s) : P(p), S(s) {}
    void init(double fs, int max_block);
    // in: mono input, n samples. outFront/outFill: n samples each (any n).
    void process(const float* in, float* outFront, float* outFill, int n);
    // Call when the audio stream (re)starts: fades in again so a dropout or a
    // USB replug never resumes with a step. full = after a (re)connect: 1 s
    // fade and stale delay-line audio cleared; otherwise a 20 ms fade.
    void restart(bool full);

    SpscRing<float>* analysis = nullptr;      // raw mic samples for the detector
    SpscRing<int16_t>* recorder = nullptr;    // interleaved [raw mic, front out]

private:
    void process_block(const float* in, float* outFront, float* outFill, int n);
    void update_filters();
    void control_block(int n);
    void reset_state();  // recover from a non-finite value anywhere in the path

    Params& P;
    Status& S;
    double fs_ = 48000;

    // filters
    Biquad hpf_[2];
    float hpf_hz_cur_ = -1;
    Biquad fixed_[kMaxFixedNotches];
    int fixed_n_ = 0;
    uint32_t fixed_ver_ = 0xffffffffu;
    struct DynState {
        Biquad bq;
        float hz = 1000, g = 0;  // current (smoothed)
        float last_hz = -1, last_g = 1;
    } dyn_[kMaxDynNotches];
    float width_cur_ = -1;

    // detectors
    Biquad det_hp_, det_lp_;
    double ms_slow_ = 1e-10, ms_fast_ = 1e-10;
    double a_slow_ = 0, a_fast_ = 0;
    float peak_in_ = 0, peak_out_ = 0;

    // control state (block rate)
    float noise_db_ = -70, speech_db_ = -36;
    float active_s_ = 0, hang_s_ = 0, silence_s_ = 0;
    bool speech_ = false;
    float agc_db_ = 0;
    float exp_db_ = 0;
    float interlock_db_ = 0, interlock_hold_ = 0;
    uint32_t howl_seen_ = 0, refunds_seen_ = 0, unpaid_ = 0;
    float lim_frac_ = 0, runaway_cool_ = 0;
    float runaway_db_ = 0, runaway_hold_ = 0;  // last-resort drop, up to 30 dB
    double last_blk_lim_ = 1;
    float total_db_ = 0;  // slew-limited total gain actually applied
    int max_block_ = 1024;

    // sample-rate smoothed gains
    double g_cur_ = 0, g_target_ = 0, g_coef_ = 0;
    double lim_gf_ = 1, lim_gb_ = 1, lim_rel_ = 0;  // per-output peak limiters
    double mute_g_ = 0, mute_coef_ = 0;
    double startup_ = 0, startup_step_ = 0;

    FreqShifter shifter_;
    double shift_xf_ = 0, shift_xf_step_ = 0, shift_hz_cur_ = 0;

    DelayLine dl_front_, dl_fill_;
    double front_g_ = 1, fill_g_ = 1;

    std::vector<int16_t> rec_tmp_;
};

}  // namespace imp
