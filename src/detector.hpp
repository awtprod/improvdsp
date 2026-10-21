// detector.hpp - acoustic feedback (howl) detector and notch manager.
//
// Runs outside the audio thread. Every hop (512 samples = 10.7 ms at 48 kHz)
// it takes a 2048-point FFT of the raw mic signal and looks for spectral
// peaks that behave like feedback rather than speech:
//   PAPR  peak-to-average power   - the peak towers over the spectrum
//   PNPR  peak-to-neighbour power - it is a narrow line, not a formant
//   PHPR  peak-to-harmonic power  - it has no harmonics (voices do)
//   persistence                   - it lasts several frames and is not decaying
// (feature set from van Waterschoot & Moonen, "Fifty years of acoustic
// feedback control", Proc. IEEE 2011). A confirmed howl gets a notch in one
// of the automatic slots, or deepens an existing one, and bumps
// Status::howl_events so the engine's interlock lowers the gain ceiling.
#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <mutex>
#include <vector>

#include "fft.hpp"
#include "params.hpp"
#include "ring.hpp"

namespace imp {

struct HowlEvent {
    double t_s;      // seconds since start
    float hz;
    float gain_db;   // notch depth after this event
    int slot;
    bool deepened;   // true = existing notch made deeper
    float level_db;  // howl level when caught (dBFS, sine amplitude)
    bool external = false;  // later judged a steady outside tone: notch undone, frequency ignored
    uint64_t serial = 0;
};

class FeedbackDetector {
public:
    FeedbackDetector(Params& p, Status& s) : P(p), S(s) {}
    void init(double fs, int fft_size = 2048, int hop = 512);

    // Analyse every complete hop waiting in `ring`. Returns frames analysed.
    int poll();

    // Control actions (safe to call from the web thread).
    void clear_dynamic();
    // Keep the current automatic notches as fixed ones. Returns how many were
    // kept; *left = how many did not fit (they stay as automatic notches).
    int commit_to_fixed(int* left = nullptr);
    void clear_fixed();
    std::vector<HowlEvent> recent_events();
    uint64_t total_detections() const { return detections_; }

    SpscRing<float> ring{1 << 16};  // the engine pushes raw mic samples here

private:
    void analyse_frame();
    void on_detect(float hz, float level_db);
    void verify_pending();
    bool ignored(float hz) const;
    bool pending_near(float hz);
    // Level of a pure tone near hz in the current frame, if one is there.
    bool tone_near(float hz, float& level_db) const;

    Params& P;
    Status& S;
    double fs_ = 48000;
    int N_ = 2048, hop_ = 512;
    FFT fft_;
    std::vector<float> frame_, hopbuf_, win_;
    std::vector<std::complex<double>> X_;
    std::vector<float> Pdb_;
    double amp_scale_ = 1;
    int hop_fill_ = 0;
    int64_t frame_no_ = 0;

    struct Track {
        bool used = false;
        float hz = 0, k = 0;
        int count = 0, miss = 0;
        float first_db = 0, last_db = 0;
        int64_t cooldown_until = 0;
        bool matched = false;
    };
    std::array<Track, 16> tracks_{};

    struct SlotMeta {
        int64_t last_used = 0;
        int hits = 0;
    };
    std::array<SlotMeta, kMaxDynNotches> meta_{};

    // After a notch goes in we check the tone ~0.3 s later. Feedback dies away
    // once the loop gain drops; a tone from outside the loop (fridge whine, a
    // test tone, a sustained note) is just as loud at the mic as before. Those
    // get their notch and interlock undone and are ignored for a minute.
    struct Pending {
        bool active = false;
        int slot = -1;
        float hz = 0, level0 = 0, prev_gain = 0;
        int64_t due = 0;
        size_t event_index = 0;
        uint64_t event_serial = 0;
    };
    std::array<Pending, 8> pending_{};
    struct Ignore { float hz; int64_t until, forget; };
    std::vector<Ignore> ignore_;
    uint64_t event_serial_ = 0;

    std::mutex mu_;
    std::vector<HowlEvent> events_;
    uint64_t detections_ = 0;
};

}  // namespace imp
