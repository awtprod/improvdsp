// dsp.cpp - filters, frequency shifter and the real-time Engine.
#include "dsp.hpp"

#include <algorithm>
#include <cmath>

namespace imp {

// ---------------------------------------------------------------- Biquad
void Biquad::set_highpass(double fs, double f, double q) {
    f = std::min(f, fs * 0.49);
    double w0 = 2 * kPi * f / fs, c = std::cos(w0), s = std::sin(w0), alpha = s / (2 * q);
    double a0 = 1 + alpha;
    b0 = (1 + c) / 2 / a0;
    b1 = -(1 + c) / a0;
    b2 = (1 + c) / 2 / a0;
    a1 = -2 * c / a0;
    a2 = (1 - alpha) / a0;
}

void Biquad::set_lowpass(double fs, double f, double q) {
    f = std::min(f, fs * 0.49);
    double w0 = 2 * kPi * f / fs, c = std::cos(w0), s = std::sin(w0), alpha = s / (2 * q);
    double a0 = 1 + alpha;
    b0 = (1 - c) / 2 / a0;
    b1 = (1 - c) / a0;
    b2 = (1 - c) / 2 / a0;
    a1 = -2 * c / a0;
    a2 = (1 - alpha) / a0;
}

void Biquad::set_peaking(double fs, double f, double q, double gain_db) {
    f = std::min(f, fs * 0.49);
    double A = std::pow(10.0, gain_db / 40.0);
    double w0 = 2 * kPi * f / fs, c = std::cos(w0), s = std::sin(w0), alpha = s / (2 * q);
    double a0 = 1 + alpha / A;
    b0 = (1 + alpha * A) / a0;
    b1 = -2 * c / a0;
    b2 = (1 - alpha * A) / a0;
    a1 = -2 * c / a0;
    a2 = (1 - alpha / A) / a0;
}

double q_from_octaves(double fs, double f, double bw_oct) {
    f = std::min(f, fs * 0.49);
    double w0 = 2 * kPi * f / fs;
    double s = std::sin(w0);
    double alpha = s * std::sinh(std::log(2.0) / 2.0 * bw_oct * w0 / s);
    return s / (2 * alpha);
}

// ---------------------------------------------------------------- Hilbert90
// Coefficients a^2 for the two allpass chains (Olli Niemitalo).
const double Hilbert90::kA2[4] = {
    0.6923878 * 0.6923878, 0.9360654322959 * 0.9360654322959,
    0.9882295226860 * 0.9882295226860, 0.9987488452737 * 0.9987488452737};
const double Hilbert90::kB2[4] = {
    0.4021921162426 * 0.4021921162426, 0.8561710882420 * 0.8561710882420,
    0.9722909545651 * 0.9722909545651, 0.9952884791278 * 0.9952884791278};

void Hilbert90::reset() {
    for (int k = 0; k < 4; ++k) {
        ax1_[k] = ax2_[k] = ay1_[k] = ay2_[k] = 0;
        bx1_[k] = bx2_[k] = by1_[k] = by2_[k] = 0;
    }
    a_delay_ = 0;
}

// ---------------------------------------------------------------- Engine
static inline float lin2db(double x) { return (float)(20.0 * std::log10(x + 1e-12)); }

void Engine::init(double fs, int max_block) {
    fs_ = fs;
    a_slow_ = 1.0 - std::exp(-1.0 / (0.030 * fs));
    a_fast_ = 1.0 - std::exp(-1.0 / (0.008 * fs));
    det_hp_.set_highpass(fs, 200, 0.7071);
    det_lp_.set_lowpass(fs, 5000, 0.7071);
    g_coef_ = 1.0 - std::exp(-1.0 / (0.005 * fs));
    lim_rel_ = 1.0 - std::exp(-1.0 / (0.100 * fs));
    mute_coef_ = 1.0 - std::exp(-1.0 / (0.020 * fs));
    startup_ = 0;
    startup_step_ = 1.0 / (1.0 * fs);  // 1 s fade-in on start
    shift_xf_step_ = 1.0 / (0.020 * fs);
    shifter_.init(fs);
    dl_front_.init((int)(0.2 * fs));
    dl_fill_.init((int)(0.2 * fs));
    max_block_ = std::max(16, max_block);
    rec_tmp_.assign(2 * (size_t)max_block_, 0);
    for (auto& h : hpf_) h.reset();
    hpf_hz_cur_ = -1;
    fixed_ver_ = 0xffffffffu;
    width_cur_ = -1;
    for (auto& d : dyn_) { d = DynState(); }
    noise_db_ = -70;
    speech_db_ = P.get(P_agc_ref_dbfs);
    agc_db_ = exp_db_ = interlock_db_ = interlock_hold_ = 0;
    howl_seen_ = S.howl_events.load();
    refunds_seen_ = S.interlock_refunds.load();
    lim_frac_ = runaway_cool_ = runaway_db_ = runaway_hold_ = 0;
    unpaid_ = 0;
    last_blk_lim_ = 1;
    total_db_ = std::min(P.get(P_gain_db), P.get(P_ceiling_db));
    g_cur_ = g_target_ = db2lin(total_db_);
    lim_gf_ = lim_gb_ = 1;
    mute_g_ = P.on(P_mute) ? 0 : 1;
    shift_xf_ = P.on(P_shift_on) ? 1 : 0;
    shift_hz_cur_ = P.get(P_shift_hz);
    front_g_ = fill_g_ = 0;
    peak_in_ = peak_out_ = 0;
}

void Engine::update_filters() {
    // High-pass: 4th-order Butterworth (two sections).
    float hz = P.get(P_hpf_hz);
    if (std::fabs(hz - hpf_hz_cur_) > 0.01f) {
        hpf_[0].set_highpass(fs_, hz, 0.54119610);
        hpf_[1].set_highpass(fs_, hz, 1.30656296);
        hpf_hz_cur_ = hz;
    }
    // Fixed (rung-out) notches.
    uint32_t ver = P.fixed_version.load(std::memory_order_acquire);
    if (ver != fixed_ver_) {
        int n = std::min(P.fixed_count.load(), kMaxFixedNotches);
        for (int i = 0; i < n; ++i) {
            float f = P.fixed[i].hz.load(), g = P.fixed[i].gain_db.load(), q = P.fixed[i].q.load();
            if (g < -0.01f && f > 20 && f < fs_ * 0.49 && q > 0.1f)
                fixed_[i].set_peaking(fs_, f, q, g);
            else
                fixed_[i].set_identity();
        }
        for (int i = n; i < kMaxFixedNotches; ++i) { fixed_[i].set_identity(); fixed_[i].reset(); }
        fixed_n_ = n;
        fixed_ver_ = ver;
    }
}

void Engine::control_block(int n) {
    const double dt = n / fs_;
    const bool ringout = P.on(P_ringout_mode);

    // --- automatic notch slots: smooth depth changes, fade out before a slot moves
    float width = P.get(P_afs_width_oct);
    int slots = (int)P.get(P_afs_slots);
    float rate = (float)(150.0 * dt);  // dB per block
    for (int i = 0; i < kMaxDynNotches; ++i) {
        DynState& d = dyn_[i];
        float th = P.dyn[i].hz.load(std::memory_order_relaxed);
        float tg = std::min(0.f, P.dyn[i].gain_db.load(std::memory_order_relaxed));
        if (i >= slots) tg = 0;
        bool moved = std::fabs(th - d.hz) > 0.03f * d.hz;
        if (moved && d.g < -0.05f) {
            d.g = std::min(0.f, d.g + 2 * rate);  // fade old notch out first
        } else {
            d.hz = th;
            if (d.g < tg) d.g = std::min(tg, d.g + rate);
            else if (d.g > tg) d.g = std::max(tg, d.g - rate);
        }
        if (d.g != d.last_g || d.hz != d.last_hz || width != width_cur_) {
            if (d.g > -0.01f) d.bq.set_identity();
            else d.bq.set_peaking(fs_, d.hz, q_from_octaves(fs_, d.hz, width), d.g);
            d.last_g = d.g;
            d.last_hz = d.hz;
        }
    }
    width_cur_ = width;

    // --- levels (AES convention: full-scale sine = 0 dBFS)
    float L = (float)(10.0 * std::log10(ms_slow_ + 1e-14) + 3.01);
    float Lf = (float)(10.0 * std::log10(ms_fast_ + 1e-14) + 3.01);

    // --- noise floor: falls quickly, rises slowly
    if (L < noise_db_) noise_db_ += (L - noise_db_) * (float)std::min(1.0, dt / 0.25);
    else noise_db_ += (float)(1.5 * dt);
    noise_db_ = std::clamp(noise_db_, -120.f, 0.f);

    // --- speech activity
    float margin = P.get(P_speech_margin_db);
    bool active = (L > noise_db_ + margin) && (L > -80.f);
    active_s_ = active ? active_s_ + (float)dt : 0.f;
    bool speech_now = active_s_ >= 0.06f;
    hang_s_ = speech_now ? 0.15f : std::max(0.f, hang_s_ - (float)dt);
    speech_ = speech_now || hang_s_ > 0;
    if (speech_now) {
        // Active speech level (in the spirit of ITU-T P.56): average only the
        // louder parts of the talk - syllable nuclei - not the quiet word tails,
        // over about a second. Quieter talk below the gate pulls the estimate
        // down slowly, so a quiet performer after a loud one is still found.
        if (L > speech_db_ - 10.f) speech_db_ += (L - speech_db_) * (float)std::min(1.0, dt / 1.0);
        else speech_db_ = std::max(L, speech_db_ - (float)(2.0 * dt));
        silence_s_ = 0;
    } else {
        silence_s_ += (float)dt;
    }
    bool howl_suspect = S.howl_suspect.load(std::memory_order_relaxed) != 0;

    // --- runaway guard (last resort, active even in ring-out mode): if a limiter
    // was working for most of the last second something is running away - e.g. a
    // frequency-shifted "chirp" the notches cannot catch, or a kicked mic. Take
    // 3 dB off every half second, up to 30 dB, hold 60 s, then recover at 0.1 dB/s.
    lim_frac_ += ((last_blk_lim_ < 0.89 ? 1.f : 0.f) - lim_frac_) * (float)std::min(1.0, dt / 1.0);
    runaway_cool_ = std::max(0.f, runaway_cool_ - (float)dt);
    if (!P.on(P_guard_on)) runaway_db_ = 0;
    else if (lim_frac_ > 0.6f && runaway_cool_ <= 0) {
        runaway_cool_ = 0.5f;
        runaway_db_ = std::min(runaway_db_ + 3.f, 30.f);
        runaway_hold_ = 60.f;
        S.runaway_events.fetch_add(1, std::memory_order_relaxed);
    } else if (runaway_hold_ > 0) {
        runaway_hold_ -= (float)dt;
    } else {
        runaway_db_ = std::max(0.f, runaway_db_ - (float)(0.1 * dt));
    }

    // --- feedback interlock: each detected howl takes afs_interlock_db off the
    // gain (up to afs_interlock_max_db), held afs_hold_s, then recovers slowly
    uint32_t ev = S.howl_events.load(std::memory_order_relaxed);
    uint32_t refunds = S.interlock_refunds.load(std::memory_order_relaxed);
    const float step_db = P.get(P_afs_interlock_db);
    const float max_db = P.get(P_afs_interlock_max_db);
    if (ev != howl_seen_) {
        if (!ringout) {
            for (uint32_t k = 0; k < ev - howl_seen_; ++k) {
                float room = max_db - interlock_db_;
                if (room >= 0.5f * step_db && step_db > 0) interlock_db_ += std::min(step_db, room);
                else if (P.on(P_guard_on) && max_db > 0) {
                    // Still catching feedback with the interlock already at its cap:
                    // that is a runaway too - escalate to the last-resort guard.
                    runaway_db_ = std::min(runaway_db_ + 3.f, 30.f);
                    runaway_hold_ = 60.f;
                    S.runaway_events.fetch_add(1, std::memory_order_relaxed);
                    ++unpaid_;
                } else {
                    ++unpaid_;  // at the cap: nothing added, so nothing to refund later
                }
            }
            interlock_hold_ = P.get(P_afs_hold_s);
        }
        howl_seen_ = ev;
    } else if (interlock_hold_ > 0) {
        interlock_hold_ -= (float)dt;
    } else {
        interlock_db_ = std::max(0.f, interlock_db_ - P.get(P_afs_recover_dbps) * (float)dt);
    }
    if (refunds != refunds_seen_) {
        // the detector found that an "event" was a steady outside tone: give the gain back
        for (uint32_t k = 0; k < refunds - refunds_seen_; ++k) {
            if (unpaid_ > 0) { --unpaid_; if (P.on(P_guard_on) && max_db > 0) runaway_db_ = std::max(0.f, runaway_db_ - 3.f); }
            else interlock_db_ = std::max(0.f, interlock_db_ - step_db);
        }
        refunds_seen_ = refunds;
    }
    if (ringout) { interlock_db_ = 0; unpaid_ = 0; }
    interlock_db_ = std::clamp(interlock_db_, 0.f, max_db);

    // --- speech-gated AGC
    float boost = P.get(P_agc_max_boost_db), cut = P.get(P_agc_max_cut_db);
    if (P.on(P_agc_on) && !ringout) {
        float desired = std::clamp(P.get(P_agc_ref_dbfs) - speech_db_, -cut, boost);
        if (speech_now && !howl_suspect && desired > agc_db_)
            agc_db_ = std::min(desired, agc_db_ + P.get(P_agc_rise_dbps) * (float)dt);
        else if (speech_now && desired < agc_db_)
            agc_db_ = std::max(desired, agc_db_ - P.get(P_agc_fall_dbps) * (float)dt);
        // Long silence: drift a boost back toward 0 dB (never rise in silence).
        if (!speech_ && silence_s_ > 2.f && agc_db_ > 0) agc_db_ = std::max(0.f, agc_db_ - (float)dt);
        // Possible feedback building: give back any boost.
        if (howl_suspect && agc_db_ > 0) agc_db_ = std::max(0.f, agc_db_ - 6.f * (float)dt);
    } else {
        float step = (float)(10.0 * dt);
        agc_db_ = agc_db_ > 0 ? std::max(0.f, agc_db_ - step) : std::min(0.f, agc_db_ + step);
    }
    agc_db_ = std::clamp(agc_db_, -cut, boost);

    // Ceiling caps gain + AGC; the interlock and runaway guard then take their
    // dB straight off the top. Increases are slew-limited to 20 dB/s so no
    // setting change (or leaving ring-out mode) can jump the gain; cuts are instant.
    float total = P.get(P_gain_db) + agc_db_;
    if (!ringout) total = std::min(total, P.get(P_ceiling_db)) - interlock_db_;
    total -= runaway_db_;
    if (total > total_db_) total = std::min(total, total_db_ + (float)(20.0 * dt));

    // --- downward expander (lower loop gain when nobody is talking)
    float target_exp = 0;
    if (P.on(P_exp_on) && !ringout) {
        float thr = std::max(P.get(P_exp_thresh_dbfs), noise_db_ + P.get(P_exp_margin_db));
        float below = thr - Lf;
        if (below > 0) {
            float ratio = std::max(1.f, P.get(P_exp_ratio));
            target_exp = -std::min(below * (1.f - 1.f / ratio), P.get(P_exp_range_db));
        }
    }
    if (target_exp > exp_db_) exp_db_ += (target_exp - exp_db_) * (float)std::min(1.0, dt / 0.005);
    else exp_db_ = std::max(target_exp, exp_db_ - (float)(40.0 * dt));

    total_db_ = total;
    g_target_ = db2lin(total + exp_db_);

    // --- publish
    S.in_level_dbfs.store(L, std::memory_order_relaxed);
    S.noise_dbfs.store(noise_db_, std::memory_order_relaxed);
    S.speech_dbfs.store(speech_db_, std::memory_order_relaxed);
    S.speech.store(speech_ ? 1 : 0, std::memory_order_relaxed);
    S.agc_db.store(agc_db_, std::memory_order_relaxed);
    S.total_gain_db.store(total, std::memory_order_relaxed);
    S.exp_db.store(exp_db_, std::memory_order_relaxed);
    S.interlock_db.store(interlock_db_ + runaway_db_, std::memory_order_relaxed);
}

void Engine::restart(bool full) {
    startup_ = 0;
    startup_step_ = 1.0 / ((full ? 1.0 : 0.02) * fs_);
    lim_frac_ = 0;
    if (full) {
        dl_front_.init((int)(0.2 * fs_));
        dl_fill_.init((int)(0.2 * fs_));
    }
}

void Engine::reset_state() {
    for (auto& h : hpf_) h.reset();
    for (auto& f : fixed_) f.reset();
    for (auto& d : dyn_) d.bq.reset();
    det_hp_.reset();
    det_lp_.reset();
    hpf_hz_cur_ = -1;
    fixed_ver_ = 0xffffffffu;
    width_cur_ = -1;
    for (auto& d : dyn_) { d.last_g = 1; if (!std::isfinite(d.g) || !std::isfinite(d.hz)) { d.g = 0; d.hz = 1000; } }
    shifter_.init(fs_);
    shift_hz_cur_ = std::isfinite(P.get(P_shift_hz)) ? P.get(P_shift_hz) : 0.0;
    dl_front_.init((int)(0.2 * fs_));
    dl_fill_.init((int)(0.2 * fs_));
    ms_slow_ = ms_fast_ = 1e-10;
    auto fix = [](float& v, float d) { if (!std::isfinite(v)) v = d; };
    fix(noise_db_, -70); fix(speech_db_, -38); fix(agc_db_, 0); fix(exp_db_, 0);
    fix(interlock_db_, 0); fix(runaway_db_, 0); fix(total_db_, 0); fix(lim_frac_, 0);
    g_cur_ = g_target_ = 0;
    lim_gf_ = lim_gb_ = 1;
    front_g_ = fill_g_ = 0;
    startup_ = 0;
    startup_step_ = 1.0 / (1.0 * fs_);
}

void Engine::process(const float* in, float* outFront, float* outFill, int n) {
    while (n > 0) {
        int m = std::min(n, max_block_);
        process_block(in, outFront, outFill, m);
        in += m; outFront += m; outFill += m; n -= m;
    }
}

void Engine::process_block(const float* in, float* outFront, float* outFill, int n) {
    update_filters();
    control_block(n);

    const double lim_thr = db2lin(P.get(P_limiter_dbfs));
    const double mute_target = P.on(P_mute) ? 0.0 : 1.0;
    const double xf_target = P.on(P_shift_on) ? 1.0 : 0.0;
    const double shift_target = P.get(P_shift_hz);
    const double front_t = P.on(P_front_on) ? db2lin(P.get(P_front_trim_db)) : 0.0;
    const double fill_t = P.on(P_fill_on) ? db2lin(P.get(P_fill_trim_db)) : 0.0;
    const int d_front = (int)std::lround(P.get(P_front_delay_ms) * 1e-3 * fs_);
    const int d_fill = (int)std::lround(P.get(P_fill_delay_ms) * 1e-3 * fs_);
    const bool rec = recorder && P.on(P_record_on);
    const int nfixed = fixed_n_;

    double blk_peak_in = 0, blk_peak_out = 0, blk_lim = 1, check = 0;

    for (int i = 0; i < n; ++i) {
        double x = std::isfinite(in[i]) ? in[i] : 0.0;
        blk_peak_in = std::max(blk_peak_in, std::fabs(x));

        double y = hpf_[1].process(hpf_[0].process(x));

        // speech-band level detectors (200 Hz - 5 kHz)
        double d = det_lp_.process(det_hp_.process(y));
        double d2 = d * d;
        ms_slow_ += (d2 - ms_slow_) * a_slow_;
        ms_fast_ += (d2 - ms_fast_) * a_fast_;

        for (int k = 0; k < nfixed; ++k) y = fixed_[k].process(y);
        for (int k = 0; k < kMaxDynNotches; ++k) y = dyn_[k].bq.process(y);

        // frequency shifter (always running so toggling is click-free)
        shift_hz_cur_ += (shift_target - shift_hz_cur_) * 0.0005;
        double s = shifter_.process(y, shift_hz_cur_);
        if (shift_xf_ < xf_target) shift_xf_ = std::min(xf_target, shift_xf_ + shift_xf_step_);
        else if (shift_xf_ > xf_target) shift_xf_ = std::max(xf_target, shift_xf_ - shift_xf_step_);
        y += (s - y) * shift_xf_;

        g_cur_ += (g_target_ - g_cur_) * g_coef_;
        y *= g_cur_;

        mute_g_ += (mute_target - mute_g_) * mute_coef_;
        if (startup_ < 1.0) startup_ = std::min(1.0, startup_ + startup_step_);
        y *= mute_g_ * startup_;

        // per-speaker delay and trim, then a peak limiter on each output
        // (after the trim, so a positive trim can never push past the limit)
        front_g_ += (front_t - front_g_) * g_coef_;
        fill_g_ += (fill_t - fill_g_) * g_coef_;
        double f = dl_front_.process((float)y, d_front) * front_g_;
        double b = dl_fill_.process((float)y, d_fill) * fill_g_;
        double af = std::fabs(f), ab = std::fabs(b);
        lim_gf_ += (1.0 - lim_gf_) * lim_rel_;  // release first, then never let a sample through above the limit
        if (af * lim_gf_ > lim_thr) lim_gf_ = lim_thr / af;
        lim_gb_ += (1.0 - lim_gb_) * lim_rel_;
        if (ab * lim_gb_ > lim_thr) lim_gb_ = lim_thr / ab;
        f = std::clamp(f * lim_gf_, -0.98, 0.98);
        b = std::clamp(b * lim_gb_, -0.98, 0.98);
        blk_lim = std::min(blk_lim, std::min(lim_gf_, lim_gb_));
        blk_peak_out = std::max(blk_peak_out, std::max(std::fabs(f), std::fabs(b)));
        check += f + b;
        outFront[i] = (float)f;
        outFill[i] = (float)b;

        if (rec) {
            rec_tmp_[2 * i] = (int16_t)std::lround(std::clamp(x, -1.0, 1.0) * 32767.0);
            rec_tmp_[2 * i + 1] = std::isfinite(f) ? (int16_t)std::lround(f * 32767.0) : 0;
        }
    }

    // A non-finite value anywhere (bad parameter, corrupt input) would otherwise
    // live forever in the recursive filters: silence this block and start clean.
    if (!std::isfinite(check) || !std::isfinite(g_cur_)) {
        for (int i = 0; i < n; ++i) outFront[i] = outFill[i] = 0.f;
        if (rec) std::fill(rec_tmp_.begin(), rec_tmp_.begin() + 2 * n, (int16_t)0);
        reset_state();
        S.dsp_resets.fetch_add(1, std::memory_order_relaxed);
        blk_peak_out = 0;
        blk_lim = 1;
    }

    if (analysis) analysis->push(in, (size_t)n);
    if (rec) {
        size_t w = recorder->push(rec_tmp_.data(), 2 * (size_t)n);
        if (w < 2 * (size_t)n) S.rec_dropped.fetch_add(1, std::memory_order_relaxed);
    }

    // meters: peak hold with ~20 dB/s decay
    double decay = std::pow(10.0, -20.0 * (n / fs_) / 20.0);
    peak_in_ = (float)std::max(blk_peak_in, peak_in_ * decay);
    peak_out_ = (float)std::max(blk_peak_out, peak_out_ * decay);
    S.in_peak_dbfs.store(lin2db(peak_in_), std::memory_order_relaxed);
    S.out_peak_dbfs.store(lin2db(peak_out_), std::memory_order_relaxed);
    last_blk_lim_ = blk_lim;
    S.lim_db.store(lin2db(blk_lim), std::memory_order_relaxed);
    S.frames.fetch_add((uint64_t)n, std::memory_order_relaxed);
}

}  // namespace imp
