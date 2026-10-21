// wav.hpp - minimal WAV reading/writing (PCM 16/24/32-bit and 32-bit float).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace imp {

struct WavData {
    int sample_rate = 0;
    int channels = 0;
    std::vector<std::vector<float>> ch;  // ch[c][i]
    bool recovered = false;      // header length was stale (interrupted recording); read to end of file
    double header_seconds = 0;   // what the header claimed, for reporting
    size_t frames() const { return ch.empty() ? 0 : ch[0].size(); }
};

bool wav_read(const std::string& path, WavData& out, std::string* err = nullptr);
// Writes 16-bit PCM (float32 if as_float).
bool wav_write(const std::string& path, const WavData& in, bool as_float = false);

// Streaming 16-bit writer (used by the recorder thread).
class WavStreamWriter {
public:
    ~WavStreamWriter() { close(); }
    bool open(const std::string& path, int sample_rate, int channels);
    void write(const int16_t* interleaved, size_t samples);
    void close();
    bool is_open() const { return f_ != nullptr; }
    uint64_t frames() const { return channels_ ? bytes_ / 2 / channels_ : 0; }

private:
    void write_header();
    FILE* f_ = nullptr;
    int sr_ = 0, channels_ = 0;
    uint64_t bytes_ = 0, last_header_ = 0;
};

}  // namespace imp
