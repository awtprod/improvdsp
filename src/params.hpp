// params.hpp - live-tunable parameters, status values and notch slots.
//
// Everything the real-time audio thread reads is a std::atomic so the web
// control page and the feedback detector can change settings without locks
// on the audio path.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

namespace imp {

// X(name, default, min, max, persist, help)
// persist = 1 -> saved to the config file by "Save settings".
#define IMP_PARAM_LIST(X)                                                                         \
    X(gain_db, 0.0f, -40.0f, 40.0f, 1, "Master gain (dB). Your 'fader'.")                         \
    X(ceiling_db, 12.0f, -40.0f, 40.0f, 1, "Hard limit on gain + AGC boost (dB). Ring point - 6") \
    X(ringout_mode, 0.0f, 0.0f, 1.0f, 0, "Ring-out mode: AGC/ceiling/expander off, notches learn") \
    X(mute, 0.0f, 0.0f, 1.0f, 0, "Mute both outputs")                                            \
    X(hpf_hz, 120.0f, 20.0f, 400.0f, 1, "High-pass corner (Hz), 24 dB/oct")                       \
    X(agc_on, 1.0f, 0.0f, 1.0f, 1, "Speech-gated automatic gain control")                         \
    X(agc_ref_dbfs, -38.0f, -80.0f, -10.0f, 1, "Speech level that gets 0 dB of AGC (dBFS)")       \
    X(agc_max_boost_db, 6.0f, 0.0f, 18.0f, 1, "Most the AGC may raise a quiet performer (dB)")    \
    X(agc_max_cut_db, 12.0f, 0.0f, 24.0f, 1, "Most the AGC may lower a loud performer (dB)")      \
    X(agc_rise_dbps, 6.0f, 0.5f, 30.0f, 1, "AGC rise speed while speech is present (dB/s)")       \
    X(agc_fall_dbps, 20.0f, 1.0f, 80.0f, 1, "AGC fall speed (dB/s)")                              \
    X(speech_margin_db, 8.0f, 3.0f, 25.0f, 1, "Speech must be this far above noise floor (dB)")   \
    X(exp_on, 1.0f, 0.0f, 1.0f, 1, "Downward expander (turns down the room when nobody talks)")   \
    X(exp_thresh_dbfs, -65.0f, -100.0f, -20.0f, 1, "Expander threshold floor (dBFS)")             \
    X(exp_margin_db, 6.0f, 0.0f, 20.0f, 1, "Expander threshold above measured noise floor (dB)")  \
    X(exp_ratio, 2.0f, 1.0f, 8.0f, 1, "Expander ratio")                                           \
    X(exp_range_db, 10.0f, 0.0f, 30.0f, 1, "Max expander attenuation (dB)")                       \
    X(shift_on, 1.0f, 0.0f, 1.0f, 1, "Frequency shifter")                                         \
    X(shift_hz, 4.0f, 0.0f, 12.0f, 1, "Frequency shift (Hz). 3-5 typical, keep under 12")          \
    X(afs_on, 1.0f, 0.0f, 1.0f, 1, "Automatic feedback notches")                                  \
    X(afs_slots, 8.0f, 0.0f, 12.0f, 1, "Number of automatic notch filters")                       \
    X(afs_width_oct, 0.1f, 0.02f, 0.5f, 1, "Notch width (octaves)")                               \
    X(afs_init_db, -6.0f, -18.0f, -1.0f, 1, "Depth of a new notch (dB)")                          \
    X(afs_step_db, -3.0f, -9.0f, -1.0f, 1, "Extra depth when feedback recurs (dB)")               \
    X(afs_max_db, -12.0f, -30.0f, -3.0f, 1, "Deepest automatic notch (dB)")                       \
    X(afs_min_hz, 150.0f, 60.0f, 1000.0f, 1, "Lowest frequency the detector watches (Hz)")        \
    X(afs_max_hz, 8000.0f, 2000.0f, 16000.0f, 1, "Highest frequency the detector watches (Hz)")   \
    X(afs_min_level_dbfs, -55.0f, -90.0f, -10.0f, 1, "Ignore peaks quieter than this (dBFS)")     \
    X(afs_papr_db, 10.0f, 3.0f, 40.0f, 1, "Peak-to-average threshold (dB)")                       \
    X(afs_phpr_db, 30.0f, 6.0f, 60.0f, 1, "Peak-to-harmonic threshold (dB)")                      \
    X(afs_pnpr_db, 12.0f, 3.0f, 40.0f, 1, "Peak-to-neighbour threshold (dB)")                     \
    X(afs_persist, 7.0f, 2.0f, 30.0f, 1, "Frames a peak must persist (1 frame = 10.7 ms)")        \
    X(afs_interlock_db, 3.0f, 0.0f, 9.0f, 1, "Drop gain this much per feedback event (dB)")    \
    X(afs_interlock_max_db, 9.0f, 0.0f, 24.0f, 1, "Max total interlock drop (dB)")                \
    X(afs_hold_s, 30.0f, 0.0f, 600.0f, 1, "Hold interlock drop this long after last event (s)")   \
    X(afs_recover_dbps, 0.1f, 0.01f, 3.0f, 1, "Interlock recovery speed after hold (dB/s)")       \
    X(limiter_dbfs, -3.0f, -24.0f, 0.0f, 1, "Output peak limiter (dBFS)")                         \
    X(guard_on, 1.0f, 0.0f, 1.0f, 0, "Last-resort runaway guard (leave on; off only for testing)") \
    X(front_on, 1.0f, 0.0f, 1.0f, 1, "Front speaker output (left)")                               \
    X(front_trim_db, 0.0f, -40.0f, 12.0f, 1, "Front speaker trim (dB)")                           \
    X(front_delay_ms, 0.0f, 0.0f, 150.0f, 1, "Front speaker delay (ms)")                          \
    X(fill_on, 1.0f, 0.0f, 1.0f, 1, "Fill speaker output (right)")                                \
    X(fill_trim_db, 0.0f, -40.0f, 12.0f, 1, "Fill speaker trim (dB)")                             \
    X(fill_delay_ms, 0.0f, 0.0f, 150.0f, 1, "Fill delay (ms) = its distance from the stage in ft / 1.13")         \
    X(record_on, 0.0f, 0.0f, 1.0f, 0, "Record raw mic + processed output to WAV")

enum ParamId : int {
#define IMP_ENUM(name, def, lo, hi, persist, help) P_##name,
    IMP_PARAM_LIST(IMP_ENUM)
#undef IMP_ENUM
    P_COUNT
};

struct ParamInfo {
    const char* name;
    float def, lo, hi;
    bool persist;
    const char* help;
};

extern const ParamInfo kParamInfo[P_COUNT];

constexpr int kMaxFixedNotches = 16;
constexpr int kMaxDynNotches = 12;

struct FixedNotch {
    std::atomic<float> hz{1000.f};
    std::atomic<float> gain_db{0.f};
    std::atomic<float> q{14.f};
};

struct DynNotch {
    std::atomic<float> hz{1000.f};
    std::atomic<float> gain_db{0.f};  // 0 = inactive
};

// Settings that are only read at start-up.
struct StartupSettings {
    std::string device = "hw:CODEC";  // Behringer UMC22 shows up as "USB Audio CODEC"
    int sample_rate = 48000;
    int period = 128;        // frames per period; 64 = lower latency, more risk of dropouts
    int prefill = 2;         // playback periods queued ahead (latency vs. safety)
    int input_channel = 0;   // 0 = UMC22 XLR/combo input
    int rt_priority = 70;
    int web_port = 8080;
    std::string web_bind = "0.0.0.0";
    int record_segment_min = 10;  // new recording file every N minutes (0 = one file per show)
    std::string record_dir = "/var/lib/improvdsp/recordings";  // point at a USB stick if the SD card is read-only
};

struct Params {
    std::atomic<float> v[P_COUNT];
    FixedNotch fixed[kMaxFixedNotches];
    std::atomic<int> fixed_count{0};
    std::atomic<uint32_t> fixed_version{0};
    DynNotch dyn[kMaxDynNotches];

    Params() { reset(); }
    void reset() {
        for (int i = 0; i < P_COUNT; ++i) v[i].store(kParamInfo[i].def, std::memory_order_relaxed);
    }
    float get(ParamId id) const { return v[id].load(std::memory_order_relaxed); }
    bool on(ParamId id) const { return get(id) >= 0.5f; }
    // Rejects NaN/Inf (returns false); clamps everything else into range.
    bool set(ParamId id, float x) {
        if (!std::isfinite(x)) return false;
        const ParamInfo& pi = kParamInfo[id];
        if (x < pi.lo) x = pi.lo;
        if (x > pi.hi) x = pi.hi;
        v[id].store(x, std::memory_order_relaxed);
        return true;
    }
    static int find(const std::string& name);
};

// Values the audio thread publishes for the meters.
struct Status {
    std::atomic<float> in_peak_dbfs{-120.f};
    std::atomic<float> in_level_dbfs{-120.f};   // speech-band RMS
    std::atomic<float> noise_dbfs{-120.f};
    std::atomic<float> speech_dbfs{-120.f};     // AGC's running speech level
    std::atomic<int> speech{0};
    std::atomic<float> agc_db{0.f};
    std::atomic<float> total_gain_db{0.f};
    std::atomic<float> exp_db{0.f};
    std::atomic<float> lim_db{0.f};
    std::atomic<float> out_peak_dbfs{-120.f};
    std::atomic<float> interlock_db{0.f};
    std::atomic<int> howl_suspect{0};
    std::atomic<uint32_t> howl_events{0};
    std::atomic<uint32_t> interlock_refunds{0};  // detector decided an event was an outside tone
    std::atomic<uint32_t> runaway_events{0};     // limiter pinned -> broadband gain drop
    std::atomic<uint32_t> dsp_resets{0};         // recovered from a non-finite value
    std::atomic<uint32_t> xruns{0};
    std::atomic<float> latency_ms{0.f};
    std::atomic<float> cpu_pct{0.f};
    std::atomic<int> device_ok{0};
    std::atomic<int> recording{0};
    std::atomic<float> rec_seconds{0.f};
    std::atomic<uint32_t> rec_dropped{0};
    std::atomic<uint64_t> frames{0};
};

inline float db2lin(float db) { return std::pow(10.f, db / 20.f); }

}  // namespace imp
