#pragma once

// Streaming sample-rate converter for a fixed rational ratio (e.g. 44.1 kHz <-> 48 kHz = 147:160).
// Exact polyphase: output sample k sits at input time k*M/L, with one precomputed windowed-sinc
// phase per position (no interpolation between phases). Kaiser window (beta 10, ~100 dB stopband),
// passband to 20 kHz, and the transition band placed so that nothing aliases or images below
// 20 kHz. The filter is centered on each output sample, so the conversion itself has no delay:
// only its emission waits for the half-length of input to arrive.

#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace vs
{
class Resampler
{
public:
    void prepare (double inRate, double outRate, int maxInBlock)
    {
        const int64_t a = (int64_t) std::llround (inRate), b = (int64_t) std::llround (outRate);
        const int64_t g = std::gcd (a, b);
        M = (int) (a / g);       // input samples per ...
        L = (int) (b / g);       // ... output samples
        identity = (M == L);
        const double lo = std::min (inRate, outRate);
        const double fpass = std::min (20000.0, 0.42 * lo);
        const double fstop = lo - fpass;                    // images / aliases land above fpass
        const double fc = 0.5 * (fpass + fstop);            // cutoff, Hz
        const double trans = std::max (fstop - fpass, 0.02 * lo);
        const double A = 100.0, beta = 0.1102 * (A - 8.7);
        // Kaiser length in seconds -> taps at the input rate
        const double secs = (A - 8.0) / (2.285 * 2.0 * 3.14159265358979 * trans);
        taps = std::max (8, (int) std::ceil (secs * inRate));
        taps += taps & 1;                                   // even
        half = taps / 2;
        table.assign ((size_t) L * (size_t) taps, 0.0f);
        const double i0b = besselI0 (beta);
        for (int ph = 0; ph < L; ++ph)
        {
            const double frac = (double) ph / L;            // output time = i + frac (input samples)
            double sum = 0.0;
            float* row = &table[(size_t) ph * (size_t) taps];
            for (int t = 0; t < taps; ++t)
            {
                const double d = (double) (t - half + 1) - frac;      // input sample time - output time
                const double x = 2.0 * fc / inRate * d;
                const double sinc = std::abs (x) < 1e-12 ? 1.0 : std::sin (3.14159265358979 * x) / (3.14159265358979 * x);
                const double r = d / (double) half;
                const double w = std::abs (r) >= 1.0 ? 0.0 : besselI0 (beta * std::sqrt (1.0 - r * r)) / i0b;
                const double h = sinc * w;
                row[t] = (float) h;
                sum += h;
            }
            for (int t = 0; t < taps; ++t) row[t] = (float) (row[t] / sum);
        }
        hist.assign ((size_t) (taps + maxInBlock + 8), 0.0f);
        reset();
    }

    void reset()
    {
        std::fill (hist.begin(), hist.end(), 0.0f);
        // pre-fill with (half - 1) zeros so output 0 (centered on input 0) can use "past" samples
        histLen = half - 1;
        base = 0; k = 0; phase = 0; iPos = 0;
    }

    // input samples the converter needs ahead of an output's time (for latency bookkeeping)
    int lookahead() const { return identity ? 0 : half; }
    int ratioM() const { return M; }
    int ratioL() const { return L; }
    bool isIdentity() const { return identity; }

    // Converts nIn input samples; writes the outputs that can now be computed. Returns how many.
    int process (const float* in, int nIn, float* out, int maxOut)
    {
        if (identity) { const int n = std::min (nIn, maxOut); std::copy (in, in + n, out); return n; }
        std::copy (in, in + nIn, hist.begin() + histLen);
        histLen += nIn;
        int produced = 0;
        // output k is centered on input index iPos (+ phase/L); it needs hist up to iPos + half
        while (produced < maxOut)
        {
            const int64_t localI = iPos - base;           // index of input sample iPos in hist (offset by half-1)
            const int64_t last = localI + (half - 1) + half;  // last hist index needed
            if (last >= histLen) break;
            const float* row = &table[(size_t) phase * (size_t) taps];
            const float* x = &hist[(size_t) localI];       // x[t] = input (iPos - half + 1 + t)
            float acc = 0.0f;
            for (int t = 0; t < taps; ++t) acc += row[t] * x[t];
            out[produced++] = acc;
            ++k;
            phase += M;
            while (phase >= L) { phase -= L; ++iPos; }
        }
        // drop history no longer needed
        const int64_t keepFrom = iPos - base;
        if (keepFrom > 0)
        {
            const int drop = (int) std::min<int64_t> (keepFrom, histLen);
            std::copy (hist.begin() + drop, hist.begin() + histLen, hist.begin());
            histLen -= drop;
            base += drop;
        }
        return produced;
    }

private:
    static double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k2 = 1; k2 < 60; ++k2) { term *= (x / (2.0 * k2)) * (x / (2.0 * k2)); sum += term; if (term < 1e-17 * sum) break; }
        return sum;
    }

    int M = 1, L = 1, taps = 8, half = 4;
    bool identity = true;
    std::vector<float> table, hist;
    int histLen = 0;
    int64_t base = 0, k = 0, iPos = 0;
    int phase = 0;
};
} // namespace vs
