// ring.hpp - lock-free single-producer / single-consumer ring buffer.
// The audio thread is the producer; it never blocks and never allocates.
#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

namespace imp {

template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity_pow2 = 1 << 16) {
        size_t c = 1;
        while (c < capacity_pow2) c <<= 1;
        buf_.assign(c, T{});
        mask_ = c - 1;
    }
    size_t capacity() const { return mask_ + 1; }
    size_t available() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
    }
    // Producer: returns number written (drops what does not fit).
    size_t push(const T* data, size_t n) {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_acquire);
        size_t space = capacity() - (h - t);
        if (n > space) n = space;
        for (size_t i = 0; i < n; ++i) buf_[(h + i) & mask_] = data[i];
        head_.store(h + n, std::memory_order_release);
        return n;
    }
    // Consumer: copy up to n items out.
    size_t pop(T* out, size_t n) {
        size_t t = tail_.load(std::memory_order_relaxed);
        size_t h = head_.load(std::memory_order_acquire);
        size_t avail = h - t;
        if (n > avail) n = avail;
        for (size_t i = 0; i < n; ++i) out[i] = buf_[(t + i) & mask_];
        tail_.store(t + n, std::memory_order_release);
        return n;
    }
    void clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::vector<T> buf_;
    size_t mask_ = 0;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace imp
