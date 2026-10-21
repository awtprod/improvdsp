// fft.hpp - small iterative radix-2 complex FFT (no external dependencies).
#pragma once
#include <cmath>
#include <complex>
#include <vector>

namespace imp {

class FFT {
public:
    explicit FFT(int n = 2048) { init(n); }
    void init(int n) {
        n_ = n;
        tw_.resize(n / 2);
        for (int k = 0; k < n / 2; ++k)
            tw_[k] = std::polar(1.0, -2.0 * 3.14159265358979323846 * k / n);
        rev_.resize(n);
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            rev_[i] = r;
        }
    }
    int size() const { return n_; }
    // In-place forward transform.
    void forward(std::vector<std::complex<double>>& a) const {
        for (int i = 0; i < n_; ++i)
            if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
        for (int len = 2; len <= n_; len <<= 1) {
            int half = len / 2, step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int j = 0; j < half; ++j) {
                    std::complex<double> u = a[i + j];
                    std::complex<double> v = a[i + j + half] * tw_[j * step];
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
        }
    }

private:
    int n_ = 0;
    std::vector<std::complex<double>> tw_;
    std::vector<int> rev_;
};

}  // namespace imp
