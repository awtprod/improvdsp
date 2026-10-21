// recorder.hpp - background thread that writes [raw mic, processed output]
// to a 2-channel WAV whenever the record_on parameter is set. Recordings of
// real shows are the best way to tune the feedback detector offline:
//   improvdsp --offline recording.wav out.wav --channel 0
#pragma once
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "params.hpp"
#include "ring.hpp"
#include "wav.hpp"

namespace imp {

class Recorder {
public:
    Recorder(Params& p, Status& s) : P(p), S(s) {}
    ~Recorder() { stop(); }

    // segment_min: start a new file every this many minutes (0 = one file).
    void start(const std::string& dir, int sample_rate, int segment_min = 10) {
        dir_ = dir;
        sr_ = sample_rate;
        seg_frames_ = segment_min > 0 ? (uint64_t)segment_min * 60 * (uint64_t)sample_rate : 0;
        run_ = true;
        th_ = std::thread([this] { loop(); });
    }
    void stop() {
        run_ = false;
        if (th_.joinable()) th_.join();
    }
    std::string last_file() const { return last_file_; }

    SpscRing<int16_t> ring{1 << 20};  // interleaved stereo int16 from the engine

private:
    void loop() {
        std::vector<int16_t> buf(1 << 16);
        while (run_) {
            bool want = P.on(P_record_on);
            if (want && !w_.is_open()) { part_ = 0; open_new(); }
            size_t n;
            while ((n = ring.pop(buf.data(), buf.size() & ~size_t(1))) > 0) {
                if (!w_.is_open()) continue;
                w_.write(buf.data(), n);
                if (seg_frames_ && w_.frames() >= seg_frames_) {  // gapless roll-over to the next part
                    done_frames_ += w_.frames();
                    w_.close();
                    open_new();
                }
            }
            if (!want && w_.is_open()) {
                w_.close();
                S.recording.store(0);
            }
            if (w_.is_open()) S.rec_seconds.store((float)(done_frames_ + w_.frames()) / (float)sr_);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (w_.is_open()) w_.close();
        S.recording.store(0);
    }
    void open_new() {
        for (size_t i = 1; i <= dir_.size(); ++i)  // mkdir -p
            if (i == dir_.size() || dir_[i] == '/') ::mkdir(dir_.substr(0, i).c_str(), 0755);
        // One show = one base name; parts are numbered so they sort in order:
        //   improv-20261010-203015-part01.wav, -part02.wav, ...
        if (part_ == 0) {
            char base[48];
            std::time_t t = std::time(nullptr);
            std::strftime(base, sizeof base, "improv-%Y%m%d-%H%M%S", std::localtime(&t));
            base_ = base;
            done_frames_ = 0;
        }
        ++part_;
        char suffix[32];
        std::snprintf(suffix, sizeof suffix, "-part%02d.wav", part_);
        std::string path = dir_ + "/" + base_ + suffix;
        if (w_.open(path, sr_, 2)) {
            last_file_ = path;
            S.recording.store(1);
            S.rec_seconds.store(0);
            std::fprintf(stderr, "recording to %s\n", path.c_str());
        } else {
            std::fprintf(stderr, "cannot record to %s (check record_dir)\n", path.c_str());
            P.set(P_record_on, 0);
            ring.clear();
        }
    }

    Params& P;
    Status& S;
    std::string dir_, last_file_;
    int sr_ = 48000;
    uint64_t seg_frames_ = 0, done_frames_ = 0;
    int part_ = 0;
    std::string base_;
    std::atomic<bool> run_{false};
    std::thread th_;
    WavStreamWriter w_;
};

}  // namespace imp
