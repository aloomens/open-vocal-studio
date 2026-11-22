#pragma once
// Deterministic synthetic test signals for Open Vocal Studio's tests: a sung-vowel "voice" (glottal
// pulses through formant resonators, with vibrato), noise, hum, sibilant bursts and plosives.
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <functional>

namespace testsig
{
constexpr double kPi = 3.14159265358979323846;

struct Rng
{
    uint32_t s = 0x12345678u;
    explicit Rng (uint32_t seed = 0x12345678u) : s (seed ? seed : 1u) {}
    float uni() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (float) (s >> 8) * (1.0f / 16777216.0f); }
    float bip() { return 2.0f * uni() - 1.0f; }
    float gauss() { float a = 0; for (int i = 0; i < 6; ++i) a += uni(); return (a - 3.0f) * 1.41f; }
};

// two-pole resonator (constant peak gain normalization)
struct Reson
{
    double a1 = 0, a2 = 0, g = 1, y1 = 0, y2 = 0;
    void set (double f, double bw, double fs)
    {
        const double r = std::exp (-kPi * bw / fs);
        a1 = -2.0 * r * std::cos (2.0 * kPi * f / fs); a2 = r * r;
        g = (1.0 - r) * std::sqrt (1.0 - 2.0 * r * std::cos (4.0 * kPi * f / fs) + r * r);
    }
    double proc (double x) { const double y = g * x - a1 * y1 - a2 * y2; y2 = y1; y1 = y; return y; }
};

struct Note { double startSec, durSec, midi; };

// A sung voice: notes (MIDI, may be fractional = out of tune), vibrato, vowel formants.
// detuneCents adds a constant offset to every note (for pitch-correction tests).
inline std::vector<float> voice (double fs, const std::vector<Note>& notes, double totalSec,
                                 double vibratoCents = 25.0, double level = 0.25, uint32_t seed = 7)
{
    const int n = (int) (totalSec * fs);
    std::vector<float> out ((size_t) n, 0.0f);
    Rng rng (seed);
    const double F[4] = { 700, 1220, 2600, 3500 }, B[4] = { 90, 110, 160, 250 }, A[4] = { 1.0, 0.6, 0.35, 0.2 };
    for (const auto& nt : notes)
    {
        Reson r[4];
        for (int k = 0; k < 4; ++k) r[k].set (F[k], B[k], fs);
        double phase = 0.0, vib = 0.0;
        const int s0 = (int) (nt.startSec * fs), len = (int) (nt.durSec * fs);
        double lp = 0.0;
        for (int i = 0; i < len && s0 + i < n; ++i)
        {
            const double t = i / fs;
            // vibrato fades in after 300 ms, like a singer
            const double vdepth = vibratoCents * std::clamp ((t - 0.3) / 0.4, 0.0, 1.0);
            vib += 2.0 * kPi * 5.6 / fs;
            const double cents = vdepth * std::sin (vib);
            const double f0 = 440.0 * std::pow (2.0, (nt.midi - 69.0) / 12.0 + cents / 1200.0);
            phase += f0 / fs;
            if (phase >= 1.0) phase -= 1.0;
            // Rosenberg-like glottal pulse derivative
            const double op = 0.6;
            double g = phase < op ? 0.5 * (1.0 - std::cos (kPi * phase / op)) : std::max (0.0, std::cos (kPi * (phase - op) / (2.0 * (1.0 - op))));
            const double src = g - lp; lp = g;
            const double asp = 0.02 * rng.gauss();
            double y = 0.0;
            for (int k = 0; k < 4; ++k) y += A[k] * r[k].proc (src * 8.0 + asp);
            // note envelope: 40 ms attack, 80 ms release
            const double env = std::min ({ 1.0, t / 0.04, (nt.durSec - t) / 0.08 });
            out[(size_t) (s0 + i)] += (float) (level * std::max (0.0, env) * y);
        }
    }
    return out;
}

// A sung voice (the same glottal source and formants as voice()) whose pitch follows midiAt (t), a MIDI
// note number that may be fractional and move freely (vibrato, scoops, drift, glides). NaN = silence;
// sounding stretches fade in and out over about 15 ms.
inline std::vector<float> voiceCurve (double fs, double totalSec, const std::function<double (double)>& midiAt,
                                      double level = 0.25, uint32_t seed = 7)
{
    const int n = (int) (totalSec * fs);
    std::vector<float> out ((size_t) n, 0.0f);
    Rng rng (seed);
    const double F[4] = { 700, 1220, 2600, 3500 }, B[4] = { 90, 110, 160, 250 }, A[4] = { 1.0, 0.6, 0.35, 0.2 };
    Reson r[4];
    for (int k = 0; k < 4; ++k) r[k].set (F[k], B[k], fs);
    double phase = 0.0, lp = 0.0, env = 0.0, f0 = 220.0;
    const double ec = 1.0 - std::exp (-1.0 / (0.005 * fs));
    for (int i = 0; i < n; ++i)
    {
        const double m = midiAt (i / fs);
        const bool on = ! std::isnan (m);
        env += ec * ((on ? 1.0 : 0.0) - env);
        if (on) f0 = 440.0 * std::pow (2.0, (m - 69.0) / 12.0);
        phase += f0 / fs;
        if (phase >= 1.0) phase -= 1.0;
        const double op = 0.6;
        double g = phase < op ? 0.5 * (1.0 - std::cos (kPi * phase / op)) : std::max (0.0, std::cos (kPi * (phase - op) / (2.0 * (1.0 - op))));
        const double src = (g - lp) * env; lp = g;
        const double asp = 0.02 * rng.gauss() * env;
        double y = 0.0;
        for (int k = 0; k < 4; ++k) y += A[k] * r[k].proc (src * 8.0 + asp);
        out[(size_t) i] = (float) (level * y);
    }
    return out;
}

// pinkish noise (Paul Kellet's economy filter), RMS about 'rms'
inline std::vector<float> noise (double fs, double totalSec, double rms, uint32_t seed = 99)
{
    const int n = (int) (totalSec * fs);
    std::vector<float> out ((size_t) n);
    Rng rng (seed);
    double b0 = 0, b1 = 0, b2 = 0;
    for (int i = 0; i < n; ++i)
    {
        const double w = rng.gauss();
        b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913;
        out[(size_t) i] = (float) ((b0 + b1 + b2 + w * 0.1848) * 0.11 * rms / 0.1);
    }
    return out;
}

inline double rms (const std::vector<float>& x, size_t from, size_t to)
{
    double s = 0.0; to = std::min (to, x.size());
    for (size_t i = from; i < to; ++i) s += (double) x[i] * x[i];
    return to > from ? std::sqrt (s / (double) (to - from)) : 0.0;
}
inline double db (double v) { return 20.0 * std::log10 (std::max (v, 1e-12)); }

// Reference pitch track (YIN at the full rate, parabolic interpolation). 0 = unvoiced.
inline std::vector<double> yinTrack (const std::vector<float>& x, double fs, double hopSec, std::vector<double>& times)
{
    std::vector<double> out;
    const int W = 1024, maxLag = (int) (fs / 70.0);
    const int hop = (int) (hopSec * fs);
    std::vector<double> d ((size_t) maxLag + 2);
    for (size_t s = 0; s + (size_t) (W + maxLag) < x.size(); s += (size_t) hop)
    {
        double run = 0; d[0] = 1;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            double v = 0; for (int k = 0; k < W; ++k) { const double e = x[s + (size_t) k] - x[s + (size_t) (k + tau)]; v += e * e; }
            run += v; d[(size_t) tau] = run > 0 ? v * tau / run : 1;
        }
        int best = -1;
        for (int tau = (int) (fs / 1200); tau <= maxLag; ++tau)
            if (d[(size_t) tau] < 0.2) { while (tau + 1 <= maxLag && d[(size_t) tau + 1] < d[(size_t) tau]) ++tau; best = tau; break; }
        double f = 0;
        if (best > 1 && best < maxLag)
        {
            const double a = d[(size_t) best - 1], b = d[(size_t) best], c = d[(size_t) best + 1];
            f = fs / (best + 0.5 * (a - c) / (a - 2 * b + c));
        }
        out.push_back (f);
        times.push_back ((s + (W + maxLag) / 2.0) / fs);
    }
    return out;
}

// mean cents of a pitch track against a MIDI note, between t0 and t1
inline double meanCents (const std::vector<double>& tr, const std::vector<double>& tm, double t0, double t1, double target)
{
    double acc = 0; int n = 0;
    for (size_t i = 0; i < tr.size(); ++i)
        if (tm[i] > t0 && tm[i] < t1 && tr[i] > 0) { acc += 100.0 * (69.0 + 12.0 * std::log2 (tr[i] / 440.0) - target); ++n; }
    return n ? acc / n : 9999.0;
}
} // namespace testsig
