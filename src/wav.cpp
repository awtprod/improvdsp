#include "wav.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace imp {

static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool wav_read(const std::string& path, WavData& out, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return fail("cannot open file");
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    out.recovered = false;
    out.header_seconds = 0;
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4))
        return fail("not a RIFF/WAVE file");
    int fmt = 0, ch = 0, sr = 0, bits = 0;
    const uint8_t* data = nullptr;
    size_t data_len = 0;
    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        uint32_t len = rd32(&d[pos + 4]);
        const uint8_t* body = &d[pos + 8];
        size_t avail = std::min<size_t>(len, d.size() - pos - 8);
        if (!std::memcmp(&d[pos], "fmt ", 4) && avail >= 16) {
            fmt = rd16(body);
            ch = rd16(body + 2);
            sr = (int)rd32(body + 4);
            bits = rd16(body + 14);
            if (fmt == 0xFFFE && avail >= 26) fmt = rd16(body + 24);  // WAVE_FORMAT_EXTENSIBLE
        } else if (!std::memcmp(&d[pos], "data", 4)) {
            data = body;
            size_t rest = d.size() - pos - 8;
            data_len = avail;
            // An interrupted recording leaves the header's length stale (0, or the
            // value from the last refresh) with more audio after it. If what follows
            // the stated length is not another chunk, take everything to end of file.
            size_t next = pos + 8 + (size_t)len + (len & 1);
            auto is_chunk_id = [&](size_t q) {
                if (q + 8 > d.size()) return false;
                for (int k = 0; k < 4; ++k) {
                    uint8_t c = d[q + k];
                    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ')) return false;
                }
                return true;
            };
            if (len == 0 || (len < rest && !is_chunk_id(next))) {
                out.recovered = true;
                out.header_seconds = len;  // bytes for now; converted below
                data_len = rest;
                break;
            }
        }
        pos += 8 + len + (len & 1);
    }
    if (!data || ch <= 0 || sr <= 0) return fail("missing fmt or data chunk");
    int bps = bits / 8;
    if (!((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32)))
        return fail("unsupported sample format");
    size_t frames = data_len / (size_t)(bps * ch);
    if (out.recovered) out.header_seconds = out.header_seconds / (double)(bps * ch) / sr;
    out.sample_rate = sr;
    out.channels = ch;
    out.ch.assign(ch, std::vector<float>(frames));
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c) {
            const uint8_t* p = data + (i * ch + c) * bps;
            float v = 0;
            if (fmt == 3) { std::memcpy(&v, p, 4); }
            else if (bits == 16) v = (int16_t)rd16(p) / 32768.f;
            else if (bits == 24) { int32_t x = (p[0] << 8) | (p[1] << 16) | (p[2] << 24); v = (x >> 8) / 8388608.f; }
            else v = (int32_t)rd32(p) / 2147483648.f;
            out.ch[c][i] = v;
        }
    return true;
}

static void put32(FILE* f, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; std::fwrite(b, 1, 4, f); }
static void put16(FILE* f, uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; std::fwrite(b, 1, 2, f); }

static void header(FILE* f, int sr, int ch, int bits, int fmt, uint32_t data_bytes) {
    std::fwrite("RIFF", 1, 4, f); put32(f, 36 + data_bytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, (uint16_t)fmt); put16(f, (uint16_t)ch);
    put32(f, (uint32_t)sr); put32(f, (uint32_t)(sr * ch * bits / 8)); put16(f, (uint16_t)(ch * bits / 8));
    put16(f, (uint16_t)bits); std::fwrite("data", 1, 4, f); put32(f, data_bytes);
}

bool wav_write(const std::string& path, const WavData& in, bool as_float) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t frames = in.frames();
    int bits = as_float ? 32 : 16;
    header(f, in.sample_rate, in.channels, bits, as_float ? 3 : 1, (uint32_t)(frames * in.channels * bits / 8));
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < in.channels; ++c) {
            float v = in.ch[c][i];
            if (as_float) std::fwrite(&v, 4, 1, f);
            else put16(f, (uint16_t)(int16_t)std::lround(std::clamp(v, -1.f, 1.f) * 32767.f));
        }
    std::fclose(f);
    return true;
}

bool WavStreamWriter::open(const std::string& path, int sample_rate, int channels) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    sr_ = sample_rate;
    channels_ = channels;
    bytes_ = 0;
    last_header_ = 0;
    write_header();
    return true;
}

void WavStreamWriter::write_header() {
    std::fseek(f_, 0, SEEK_SET);
    uint32_t db = (uint32_t)std::min<uint64_t>(bytes_, 0xFFFFFFFFull - 36);
    header(f_, sr_, channels_, 16, 1, db);
    std::fseek(f_, 0, SEEK_END);
}

void WavStreamWriter::write(const int16_t* x, size_t samples) {
    if (!f_) return;
    std::fwrite(x, sizeof(int16_t), samples, f_);  // little-endian hosts only (Pi, x86)
    bytes_ += samples * 2;
    // Every ~5 s: refresh the header and force everything onto the card/stick
    // (fsync), so a power cut loses at most about the last 5 seconds.
    if (bytes_ - last_header_ >= (uint64_t)sr_ * channels_ * 2 * 5) {
        write_header();
        std::fflush(f_);
        ::fsync(fileno(f_));
        last_header_ = bytes_;
    }
}

void WavStreamWriter::close() {
    if (!f_) return;
    write_header();
    std::fflush(f_);
    ::fsync(fileno(f_));
    std::fclose(f_);
    f_ = nullptr;
}

}  // namespace imp
