#pragma once

// Mixed-radix FFT (any size made of factors 2, 3, 4, 5 and other small primes), following
// KISS FFT (Copyright (c) 2003-2010 Mark Borgerding, BSD 3-Clause; see THIRD-PARTY-NOTICES.txt):
// recursive decimation in time, radix-2/4 and generic butterflies. DeepFilterNet needs
// a 960-point transform, which a power-of-two FFT can't do. Plans are made in prepare(); the
// transforms don't allocate.

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>

namespace vs
{
class Fft
{
public:
    using cpx = std::complex<float>;

    void prepare (int size)
    {
        n = size;
        tw.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double ph = -2.0 * 3.14159265358979323846 * i / n;
            tw[(size_t) i] = cpx ((float) std::cos (ph), (float) std::sin (ph));
        }
        factors.clear();
        int m = n, p = 4;
        const double floorSqrt = std::floor (std::sqrt ((double) n));
        do
        {
            while (m % p)
            {
                switch (p) { case 4: p = 2; break; case 2: p = 3; break; default: p += 2; break; }
                if (p > floorSqrt) p = m;
            }
            m /= p;
            factors.push_back (p);
            factors.push_back (m);
        } while (m > 1);
        scratch.resize ((size_t) std::max (8, maxFactor()));
        work.resize ((size_t) n);
        rbuf.resize ((size_t) n);
    }

    int size() const { return n; }

    // out[k] = sum x[j] e^{-2 pi i jk/n}
    void forward (const cpx* in, cpx* out) { run (out, in, 1, 1, factors.data(), false); }
    // out[j] = sum X[k] e^{+2 pi i jk/n}  (unnormalized)
    void inverse (const cpx* in, cpx* out) { run (out, in, 1, 1, factors.data(), true); }

    // Real input of length n -> n/2+1 bins (unnormalized).
    void forwardReal (const float* in, cpx* outBins)
    {
        for (int i = 0; i < n; ++i) rbuf[(size_t) i] = cpx (in[i], 0.0f);
        forward (rbuf.data(), work.data());
        std::copy (work.begin(), work.begin() + n / 2 + 1, outBins);
    }
    // n/2+1 bins -> real output of length n (unnormalized; Hermitian symmetry assumed).
    void inverseReal (const cpx* bins, float* out)
    {
        const int h = n / 2;
        for (int k = 0; k <= h; ++k) rbuf[(size_t) k] = bins[k];
        for (int k = h + 1; k < n; ++k) rbuf[(size_t) k] = std::conj (bins[n - k]);
        inverse (rbuf.data(), work.data());
        for (int i = 0; i < n; ++i) out[i] = work[(size_t) i].real();
    }

private:
    int maxFactor() const { int m = 0; for (size_t i = 0; i < factors.size(); i += 2) m = std::max (m, factors[i]); return m; }

    void run (cpx* out, const cpx* in, int fstride, int inStride, const int* f, bool inv)
    {
        const int p = f[0], m = f[1];
        cpx* const beg = out;
        cpx* const end = out + p * m;
        if (m == 1)
        {
            do { *out = *in; in += fstride * inStride; } while (++out != end);
        }
        else
        {
            do { run (out, in, fstride * p, inStride, f + 2, inv); in += fstride * inStride; } while ((out += m) != end);
        }
        out = beg;
        switch (p)
        {
            case 2: bfly2 (out, fstride, m, inv); break;
            case 4: bfly4 (out, fstride, m, inv); break;
            default: bflyGeneric (out, fstride, m, p, inv); break;
        }
    }

    inline cpx twd (int idx, bool inv) const { const cpx t = tw[(size_t) idx]; return inv ? std::conj (t) : t; }

    void bfly2 (cpx* out, int fstride, int m, bool inv)
    {
        cpx* o2 = out + m;
        for (int k = 0; k < m; ++k)
        {
            const cpx t = o2[k] * twd (k * fstride, inv);
            o2[k] = out[k] - t;
            out[k] += t;
        }
    }

    void bfly4 (cpx* out, int fstride, int m, bool inv)
    {
        const int m2 = 2 * m, m3 = 3 * m;
        for (int k = 0; k < m; ++k)
        {
            const cpx s0 = out[k + m] * twd (k * fstride, inv);
            const cpx s1 = out[k + m2] * twd (k * fstride * 2, inv);
            const cpx s2 = out[k + m3] * twd (k * fstride * 3, inv);
            const cpx s5 = out[k] - s1;
            out[k] += s1;
            const cpx s3 = s0 + s2, s4 = s0 - s2;
            out[k + m2] = out[k] - s3;
            out[k] += s3;
            // multiply s4 by -i (forward) or +i (inverse)
            const cpx r = inv ? cpx (-s4.imag(), s4.real()) : cpx (s4.imag(), -s4.real());
            out[k + m] = s5 + r;
            out[k + m3] = s5 - r;
        }
    }

    void bflyGeneric (cpx* out, int fstride, int m, int p, bool inv)
    {
        for (int u = 0; u < m; ++u)
        {
            for (int q1 = 0, k = u; q1 < p; ++q1, k += m) scratch[(size_t) q1] = out[k];
            for (int q1 = 0, k = u; q1 < p; ++q1, k += m)
            {
                int twIdx = 0;
                cpx acc = scratch[0];
                for (int q = 1; q < p; ++q)
                {
                    twIdx += fstride * k;
                    if (twIdx >= n) twIdx -= n;
                    acc += scratch[(size_t) q] * twd (twIdx, inv);
                }
                out[k] = acc;
            }
        }
    }

    int n = 0;
    std::vector<cpx> tw, scratch, work, rbuf;
    std::vector<int> factors;
};
} // namespace vs
