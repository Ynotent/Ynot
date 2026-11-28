// Small radix-2 FFT shared by the core's offline DSP (time stretch, analysis). Internal header.
#pragma once

#include <complex>
#include <vector>

namespace ynot::detail {

class Fft {
public:
    explicit Fft(int n) : n_(n), rev_(static_cast<size_t>(n)), tw_(static_cast<size_t>(n / 2)) {
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            rev_[static_cast<size_t>(i)] = r;
        }
        for (int i = 0; i < n / 2; ++i) tw_[static_cast<size_t>(i)] = std::polar(1.0, -2.0 * 3.14159265358979323846 * i / n);
    }
    void run(std::vector<std::complex<double>>& x, bool inverse) const {
        for (int i = 0; i < n_; ++i)
            if (i < rev_[static_cast<size_t>(i)]) std::swap(x[static_cast<size_t>(i)], x[static_cast<size_t>(rev_[static_cast<size_t>(i)])]);
        for (int len = 2; len <= n_; len <<= 1) {
            const int stride = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int j = 0; j < len / 2; ++j) {
                    auto w = tw_[static_cast<size_t>(j * stride)];
                    if (inverse) w = std::conj(w);
                    auto& a = x[static_cast<size_t>(i + j)];
                    auto& b = x[static_cast<size_t>(i + j + len / 2)];
                    const auto t = w * b;
                    b = a - t;
                    a += t;
                }
        }
        if (inverse)
            for (auto& v : x) v /= n_;
    }

private:
    int n_;
    std::vector<int> rev_;
    std::vector<std::complex<double>> tw_;
};

}  // namespace ynot::detail
