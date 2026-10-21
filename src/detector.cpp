// detector.cpp - feedback detector (see detector.hpp for the method).
#include "detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dsp.hpp"  // q_from_octaves

namespace imp {

void FeedbackDetector::init(double fs, int fft_size, int hop) {
    fs_ = fs;
    N_ = fft_size;
    hop_ = hop;
    fft_.init(N_);
    frame_.assign(N_, 0.f);
    hopbuf_.assign(hop_, 0.f);
    win_.resize(N_);
    double wsum = 0;
    for (int i = 0; i < N_; ++i) {
        win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / N_));  // Hann
        wsum += win_[i];
    }
    amp_scale_ = 2.0 / wsum;  // |X| * amp_scale_ = sine amplitude
    X_.assign(N_, {0, 0});
    Pdb_.assign(N_ / 2 + 1, -200.f);
    hop_fill_ = 0;
    frame_no_ = 0;
    for (auto& t : tracks_) t = Track();
    for (auto& m : meta_) m = SlotMeta();
}

int FeedbackDetector::poll() {
    int frames = 0;
    for (;;) {
        size_t need = (size_t)(hop_ - hop_fill_);
        size_t got = ring.pop(hopbuf_.data() + hop_fill_, need);
        hop_fill_ += (int)got;
        if (hop_fill_ < hop_) break;
        // slide the analysis window by one hop
        std::memmove(frame_.data(), frame_.data() + hop_, sizeof(float) * (N_ - hop_));
        std::memcpy(frame_.data() + (N_ - hop_), hopbuf_.data(), sizeof(float) * hop_);
        hop_fill_ = 0;
        analyse_frame();
        ++frames;
    }
    return frames;
}

void FeedbackDetector::analyse_frame() {
    ++frame_no_;
    if (!P.on(P_afs_on)) {
        S.howl_suspect.store(0, std::memory_order_relaxed);
        for (auto& t : tracks_) t.used = false;
        return;
    }

    for (int i = 0; i < N_; ++i) X_[i] = {frame_[i] * win_[i], 0.0};
    fft_.forward(X_);
    const int half = N_ / 2;
    for (int k = 0; k <= half; ++k) {
        double m = std::abs(X_[k]) * amp_scale_;
        Pdb_[k] = (float)(20.0 * std::log10(m + 1e-10));
    }

    verify_pending();

    const double bin_hz = fs_ / N_;
    const int kmin = std::max(4, (int)std::ceil(P.get(P_afs_min_hz) / bin_hz));
    const int kmax = std::min(half - 6, (int)std::floor(P.get(P_afs_max_hz) / bin_hz));
    if (kmax <= kmin + 4) return;

    double sum = 0;
    for (int k = kmin; k <= kmax; ++k) sum += std::pow(10.0, Pdb_[k] / 10.0);
    const float avg_db = (float)(10.0 * std::log10(sum / (kmax - kmin + 1) + 1e-20));

    const float min_level = P.get(P_afs_min_level_dbfs);
    const float papr_thr = P.get(P_afs_papr_db);
    const float pnpr_thr = P.get(P_afs_pnpr_db);
    const float phpr_thr = P.get(P_afs_phpr_db);

    // local maxima -> top 5 candidates
    struct Cand { int k; float db; };
    std::vector<Cand> cands;
    cands.reserve(32);
    for (int k = kmin; k <= kmax; ++k) {
        float p = Pdb_[k];
        if (p < min_level) continue;
        if (p > Pdb_[k - 1] && p >= Pdb_[k + 1] && p > Pdb_[k - 2] && p >= Pdb_[k + 2])
            cands.push_back({k, p});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.db > b.db; });
    if (cands.size() > 5) cands.resize(5);

    auto pmax_near = [&](double kk) {
        int c = (int)std::lround(kk);
        float m = -200.f;
        for (int j = c - 1; j <= c + 1; ++j)
            if (j >= 1 && j <= half) m = std::max(m, Pdb_[j]);
        return m;
    };

    for (auto& t : tracks_) t.matched = false;

    for (const Cand& c : cands) {
        const int k = c.k;
        // parabolic interpolation of the peak (on dB magnitude)
        float a = Pdb_[k - 1], b = Pdb_[k], g = Pdb_[k + 1];
        float den = a - 2 * b + g;
        float p = (std::fabs(den) > 1e-6f) ? 0.5f * (a - g) / den : 0.f;
        p = std::clamp(p, -0.5f, 0.5f);
        float kf = k + p;
        float level = b - 0.25f * (a - g) * p;
        float hz = (float)(kf * bin_hz);

        float papr = level - avg_db;
        double nb = 0;
        for (int j : {-4, -3, 3, 4}) nb += std::pow(10.0, Pdb_[k + j] / 10.0);
        float pnpr = level - (float)(10.0 * std::log10(nb / 4 + 1e-20));
        float phpr = 200.f;
        for (double m : {0.5, 1.5, 2.0, 3.0}) {
            double fh = hz * m;
            if (fh < 60.0 || fh > fs_ * 0.47) continue;
            phpr = std::min(phpr, level - pmax_near(fh / bin_hz));
        }
        bool pass = papr >= papr_thr && pnpr >= pnpr_thr && phpr >= phpr_thr;
        if (!pass || ignored(hz) || pending_near(hz)) continue;

        // match to an existing track (within 1.5 bins) or start a new one
        Track* tr = nullptr;
        for (auto& t : tracks_)
            if (t.used && !t.matched && std::fabs(t.k - kf) <= 1.5f) { tr = &t; break; }
        if (tr) {
            tr->hz = hz; tr->k = kf; tr->last_db = level; tr->miss = 0; tr->matched = true;
            if (tr->count == 0) tr->first_db = level;
            tr->count++;
        } else {
            for (auto& t : tracks_)
                if (!t.used) {
                    t = Track();
                    t.used = true; t.hz = hz; t.k = kf; t.count = 1; t.matched = true;
                    t.first_db = t.last_db = level;
                    break;
                }
        }
    }

    const int persist = (int)P.get(P_afs_persist);
    const int64_t cooldown = (int64_t)std::lround(0.25 * fs_ / hop_);
    bool suspect = false;
    for (auto& t : tracks_) {
        if (!t.used) continue;
        if (!t.matched) {
            if (++t.miss > 1) { t.used = false; continue; }
        }
        if (t.count >= 2) suspect = true;
        if (t.matched && t.count >= persist && frame_no_ >= t.cooldown_until &&
            t.last_db >= t.first_db - 1.5f) {
            on_detect(t.hz, t.last_db);
            t.cooldown_until = frame_no_ + cooldown;
            t.count = 0;
            t.first_db = t.last_db;
        }
    }
    S.howl_suspect.store(suspect ? 1 : 0, std::memory_order_relaxed);
}

void FeedbackDetector::on_detect(float hz, float level_db) {
    std::lock_guard<std::mutex> lock(mu_);
    const int slots = std::min((int)P.get(P_afs_slots), kMaxDynNotches);
    // Same-notch tolerance: a quarter of the notch width (a notch is only a few
    // dB deep that far off-centre), or the FFT's frequency resolution if larger.
    const double bin_hz = fs_ / N_;
    const double tol = std::max(P.get(P_afs_width_oct) / 4.0, std::log2(1.0 + 0.5 * bin_hz / hz));
    HowlEvent ev{frame_no_ * (double)hop_ / fs_, hz, 0.f, -1, false, level_db, false, ++event_serial_};
    float prev_gain = 0.f;

    int best = -1;
    double best_d = 1e9;
    for (int i = 0; i < slots; ++i) {
        float g = P.dyn[i].gain_db.load();
        if (g > -0.01f) continue;
        double d = std::fabs(std::log2(hz / P.dyn[i].hz.load()));
        if (d <= tol && d < best_d) { best = i; best_d = d; }
    }
    if (best >= 0) {
        prev_gain = P.dyn[best].gain_db.load();
        float g = std::max(prev_gain + P.get(P_afs_step_db), P.get(P_afs_max_db));
        P.dyn[best].hz.store(0.5f * (P.dyn[best].hz.load() + hz));  // re-centre on the new estimate
        P.dyn[best].gain_db.store(g);
        meta_[best].last_used = frame_no_;
        meta_[best].hits++;
        ev.slot = best; ev.gain_db = g; ev.deepened = true;
    } else if (slots > 0) {
        int idx = -1;
        for (int i = 0; i < slots; ++i)
            if (P.dyn[i].gain_db.load() > -0.01f) { idx = i; break; }
        if (idx < 0) {  // all busy: recycle the least recently used slot
            idx = 0;
            for (int i = 1; i < slots; ++i)
                if (meta_[i].last_used < meta_[idx].last_used) idx = i;
        }
        float g = std::max(P.get(P_afs_init_db), P.get(P_afs_max_db));
        P.dyn[idx].hz.store(hz);
        P.dyn[idx].gain_db.store(g);
        meta_[idx].last_used = frame_no_;
        meta_[idx].hits = 1;
        ev.slot = idx; ev.gain_db = g;
    }
    if (ev.slot >= 0) {
        for (auto& p : pending_)
            if (!p.active) {
                p.active = true;
                p.slot = ev.slot;
                p.hz = hz;
                p.level0 = level_db;
                p.prev_gain = prev_gain;
                p.due = frame_no_ + (int64_t)std::lround(0.30 * fs_ / hop_);
                p.event_serial = ev.serial;
                break;
            }
    }
    events_.push_back(ev);
    if (events_.size() > 64) events_.erase(events_.begin());
    ++detections_;
    S.howl_events.fetch_add(1, std::memory_order_relaxed);
}

bool FeedbackDetector::tone_near(float hz, float& level_db) const {
    const double bin_hz = fs_ / N_;
    const int half = N_ / 2;
    int c = (int)std::lround(hz / bin_hz);
    int k = -1;
    for (int j = c - 1; j <= c + 1; ++j)
        if (j >= 5 && j <= half - 5 && (k < 0 || Pdb_[j] > Pdb_[k])) k = j;
    if (k < 0) return false;
    level_db = Pdb_[k];
    double nb = 0;
    for (int j : {-4, -3, 3, 4}) nb += std::pow(10.0, Pdb_[k + j] / 10.0);
    float pnpr = level_db - (float)(10.0 * std::log10(nb / 4 + 1e-20));
    return pnpr >= P.get(P_afs_pnpr_db);
}

bool FeedbackDetector::pending_near(float hz) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& p : pending_)
        if (p.active && std::fabs(std::log2(hz / p.hz)) < 1.0 / 24.0) return true;
    return false;
}

bool FeedbackDetector::ignored(float hz) const {
    for (const auto& ig : ignore_)
        if (frame_no_ < ig.until && std::fabs(std::log2(hz / ig.hz)) < 1.0 / 24.0) return true;
    return false;
}

void FeedbackDetector::verify_pending() {
    std::lock_guard<std::mutex> lock(mu_);  // pending_ is also touched by the web thread
    const int64_t per_s = (int64_t)std::lround(fs_ / hop_);
    ignore_.erase(std::remove_if(ignore_.begin(), ignore_.end(), [&](const Ignore& ig) { return frame_no_ > ig.forget; }),
                  ignore_.end());
    for (auto& p : pending_) {
        if (!p.active || frame_no_ < p.due) continue;
        p.active = false;
        float l1 = -200.f;
        bool tone = tone_near(p.hz, l1);
        bool limiting = S.lim_db.load(std::memory_order_relaxed) < -1.0f;
        // Unchanged pure tone while the output is not limiting -> not our feedback.
        if (!tone || limiting || std::fabs(l1 - p.level0) > 2.5f) continue;
        if (p.slot >= 0 && std::fabs(std::log2(P.dyn[p.slot].hz.load() / p.hz)) < 1.0 / 24.0)
            P.dyn[p.slot].gain_db.store(p.prev_gain);
        uint32_t refunds = 1;
        for (auto& q : pending_)  // any later checks on the same tone are moot
            if (q.active && std::fabs(std::log2(q.hz / p.hz)) < 1.0 / 24.0) {
                q.active = false;
                ++refunds;
                for (auto& e : events_)
                    if (e.serial == q.event_serial) e.external = true;
            }
        bool seen_before = false;
        for (auto& ig : ignore_)
            if (std::fabs(std::log2(p.hz / ig.hz)) < 1.0 / 24.0) {
                ig.until = frame_no_ + 600 * per_s;  // second strike: ignore for 10 minutes
                ig.forget = ig.until + 600 * per_s;
                seen_before = true;
            }
        if (!seen_before) ignore_.push_back({p.hz, frame_no_ + 30 * per_s, frame_no_ + 630 * per_s});
        for (auto& e : events_)
            if (e.serial == p.event_serial) e.external = true;
        S.interlock_refunds.fetch_add(refunds, std::memory_order_relaxed);
    }
}

void FeedbackDetector::clear_dynamic() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& p : pending_) p.active = false;
    for (int i = 0; i < kMaxDynNotches; ++i) {
        P.dyn[i].gain_db.store(0.f);
        meta_[i] = SlotMeta();
    }
}

int FeedbackDetector::commit_to_fixed(int* left) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& p : pending_) p.active = false;
    int n = P.fixed_count.load();
    int committed = 0, not_kept = 0;
    const float width = P.get(P_afs_width_oct);
    for (int i = 0; i < kMaxDynNotches; ++i) {
        float g = P.dyn[i].gain_db.load(), hz = P.dyn[i].hz.load();
        if (g > -0.01f) continue;
        int match = -1;
        for (int j = 0; j < n; ++j)
            if (std::fabs(std::log2(hz / P.fixed[j].hz.load())) <= width / 4.0) { match = j; break; }
        if (match >= 0) {
            P.fixed[match].gain_db.store(std::min(P.fixed[match].gain_db.load(), g));
            ++committed;
        } else if (n < kMaxFixedNotches) {
            P.fixed[n].hz.store(hz);
            P.fixed[n].gain_db.store(g);
            P.fixed[n].q.store((float)q_from_octaves(fs_, hz, width));
            ++n;
            ++committed;
        } else {
            ++not_kept;  // fixed list full: leave it running as an automatic notch
            continue;
        }
        P.dyn[i].gain_db.store(0.f);
        meta_[i] = SlotMeta();
    }
    if (left) *left = not_kept;
    P.fixed_count.store(n);
    P.fixed_version.fetch_add(1, std::memory_order_release);
    return committed;
}

void FeedbackDetector::clear_fixed() {
    std::lock_guard<std::mutex> lock(mu_);
    P.fixed_count.store(0);
    P.fixed_version.fetch_add(1, std::memory_order_release);
}

std::vector<HowlEvent> FeedbackDetector::recent_events() {
    std::lock_guard<std::mutex> lock(mu_);
    return events_;
}

}  // namespace imp
