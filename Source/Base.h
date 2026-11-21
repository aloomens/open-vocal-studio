#pragma once

// Open Vocal Studio - shared DSP building blocks (smoothing, biquads, small helpers).
// Glide, SmoothParam, Biquad and SmoothRandom come from Clean Tone Studio (same author, same license).
// Everything here is allocation-free in process(); memory is sized in prepare().

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>
#include <cmath>
#include <complex>
#include <algorithm>

namespace vs
{
constexpr double kPiD = 3.14159265358979323846;
constexpr float  kPi  = 3.14159265358979f;

inline float dbToGain (float db)     { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g)      { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }
inline float onePoleCoef (double ms, double fs)
{
    return (float) (1.0 - std::exp (-1.0 / (std::max (ms, 0.001) * 0.001 * fs)));
}

// Filter coefficients glide linearly over this many samples whenever they're updated,
// and the Chain refreshes them on the same grid - so filter sweeps have no steps at all.
constexpr int kCoefRamp = 4;

//==============================================================================
// Two cascaded one-pole smoothers: an S-shaped move with no sharp corner at the start
// (a plain one-pole jumps to full speed instantly, which is itself a faint click).
struct Glide
{
    float s1 = 0.0f, s2 = 0.0f;
    void snap (float v) { s1 = s2 = v; }
    inline float next (float target, float c)
    {
        // when a step rounds to nothing in float, the move is finished: land exactly on the target
        const float n1 = s1 + c * (target - s1); s1 = (n1 == s1) ? target : n1;
        const float n2 = s2 + c * (s1 - s2);     s2 = (n2 == s2) ? s1 : n2;
        return s2;
    }
    float value() const { return s2; }
};

//==============================================================================
// A parameter that glides towards its target (S-shaped). Stepped by the Chain every
// kCoefRamp samples; 'logScale' smooths frequencies in the log domain so sweeps sound even.
struct SmoothParam
{
    float s1 = 0.0f, cur = 0.0f, target = 0.0f;
    bool logScale = false, primed = false;

    void reset() { primed = false; }
    void setTarget (float v)
    {
        target = logScale ? std::log (std::max (v, 1.0e-6f)) : v;
        if (! primed) { s1 = cur = target; primed = true; }
    }
    bool isMoving() const { return cur != target || s1 != target; }
    bool step (float c)
    {
        if (! isMoving()) return false;
        // a step that rounds to nothing in float means the glide is finished: land exactly on the target
        // (otherwise it stalls a hair short, forever "moving", and the control never quite arrives)
        const float n1 = s1 + c * (target - s1); s1 = (n1 == s1) ? target : n1;
        const float n2 = cur + c * (s1 - cur);   cur = (n2 == cur) ? s1 : n2;
        const float eps = 1.0e-5f * (1.0f + std::abs (target));
        if (std::abs (target - s1) <= eps && std::abs (target - cur) <= eps) s1 = cur = target;
        return true;
    }
    float value() const { return logScale ? std::exp (cur) : cur; }
};

//==============================================================================
// RBJ-cookbook biquad, transposed direct form II, up to 2 channels. Coefficients and state are
// double precision: low-cut and low-shelf filters have poles very close to 1, and in float their
// rounding noise is amplified enormously near DC (measured: -76 dB below a 10 kHz tone at 11 Hz).
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] {}, z2[2] {};

    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }

    // Glide this filter's coefficients to those of 't' over 'samples' calls of advance().
    void glideTo (const Biquad& t, int samples)
    {
        tgt[0] = t.b0; tgt[1] = t.b1; tgt[2] = t.b2; tgt[3] = t.a1; tgt[4] = t.a2;
        if (samples <= 0) { b0 = t.b0; b1 = t.b1; b2 = t.b2; a1 = t.a1; a2 = t.a2; rampLeft = 0; return; }
        const double inv = 1.0 / (double) samples;
        d[0] = (t.b0 - b0) * inv; d[1] = (t.b1 - b1) * inv; d[2] = (t.b2 - b2) * inv;
        d[3] = (t.a1 - a1) * inv; d[4] = (t.a2 - a2) * inv;
        rampLeft = samples;
    }
    inline void advance() noexcept     // once per sample (not per channel)
    {
        if (rampLeft <= 0) return;
        if (--rampLeft == 0) { b0 = tgt[0]; b1 = tgt[1]; b2 = tgt[2]; a1 = tgt[3]; a2 = tgt[4]; return; }
        b0 += d[0]; b1 += d[1]; b2 += d[2]; a1 += d[3]; a2 += d[4];
    }

    inline float process (float xf, int ch) noexcept
    {
        const double x = xf;
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return (float) y;
    }

    void setIdentity() { b0 = 1; b1 = b2 = a1 = a2 = 0; }

    void setPeak (double fs, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        double w, c, al; prep (fs, f, q, w, c, al);
        set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }

    void setLowShelf (double fs, double f, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        double w, c, al; prep (fs, f, 0.7071, w, c, al);
        const double sA = 2.0 * std::sqrt (A) * al;
        set (A * ((A + 1) - (A - 1) * c + sA),
             2 * A * ((A - 1) - (A + 1) * c),
             A * ((A + 1) - (A - 1) * c - sA),
             (A + 1) + (A - 1) * c + sA,
             -2 * ((A - 1) + (A + 1) * c),
             (A + 1) + (A - 1) * c - sA);
    }

    void setHighShelf (double fs, double f, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        double w, c, al; prep (fs, f, 0.7071, w, c, al);
        const double sA = 2.0 * std::sqrt (A) * al;
        set (A * ((A + 1) + (A - 1) * c + sA),
             -2 * A * ((A - 1) + (A + 1) * c),
             A * ((A + 1) + (A - 1) * c - sA),
             (A + 1) - (A - 1) * c + sA,
             2 * ((A - 1) - (A + 1) * c),
             (A + 1) - (A - 1) * c - sA);
    }

    void setLowPass (double fs, double f, double q)
    {
        double w, c, al; prep (fs, f, q, w, c, al);
        set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }

    void setHighPass (double fs, double f, double q)
    {
        double w, c, al; prep (fs, f, q, w, c, al);
        set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }

    void setNotch (double fs, double f, double q)
    {
        double w, c, al; prep (fs, f, q, w, c, al);
        set (1, -2 * c, 1, 1 + al, -2 * c, 1 - al);
    }

private:
    static void prep (double fs, double f, double q, double& w, double& c, double& al)
    {
        f = std::clamp (f, 10.0, fs * 0.45);
        w = 2.0 * kPiD * f / fs;
        c = std::cos (w);
        al = std::sin (w) / (2.0 * std::max (q, 0.05));
    }

    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2)
    {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0;
        a1 = na1 / na0; a2 = na2 / na0;
        rampLeft = 0;
    }

    double d[5] {}, tgt[5] {};
    int rampLeft = 0;
};

//==============================================================================
// A smooth random wobble in [-1, 1]: picks a new random target every few tenths of a second
// (random length too) and moves there along a cosine curve. Deterministic (fixed seed).
struct SmoothRandom
{
    void init (uint32_t seed, double fs, float minSeconds, float maxSeconds)
    {
        state = seed ? seed : 1u; rate = fs; minS = minSeconds; maxS = maxSeconds;
        from = 0.0f; to = rnd(); pos = 0; posF = 0.0f; len = segLength();
    }
    // the same, moving 'speed' times as fast (below 1: slower, longer segments)
    inline float next (float speed)
    {
        const float t = std::min (1.0f, posF / (float) len);
        const float v = from + (to - from) * (0.5f - 0.5f * std::cos (kPi * t));
        posF += speed;
        if (posF >= (float) len) { from = to; to = rnd(); posF -= (float) len; len = segLength(); }
        return v;
    }
    inline float next()
    {
        const float t = (float) pos / (float) len;
        const float v = from + (to - from) * (0.5f - 0.5f * std::cos (kPi * t));
        if (++pos >= len) { from = to; to = rnd(); pos = 0; len = segLength(); }
        return v;
    }
private:
    inline float rnd() { state = state * 1664525u + 1013904223u; return (float) (state >> 8) * (2.0f / 16777216.0f) - 1.0f; }
    int segLength() { const float u = 0.5f * (rnd() + 1.0f); return std::max (1, (int) ((minS + u * (maxS - minS)) * rate)); }
    uint32_t state = 1; double rate = 48000.0; float minS = 0.5f, maxS = 1.0f, from = 0, to = 0, posF = 0; int pos = 0, len = 1;
};


//==============================================================================
// A plain delay line for whole-sample latency alignment (dry paths, A/B compare).
struct DelayLine
{
    std::vector<float> buf; int mask = 0, w = 0, delay = 0;
    void prepare (int maxDelay) { int s = 1; while (s < maxDelay + 2) s <<= 1; buf.assign ((size_t) s, 0.0f); mask = s - 1; w = 0; }
    void setDelay (int d) { delay = std::clamp (d, 0, mask); }
    void clear() { std::fill (buf.begin(), buf.end(), 0.0f); }
    inline float process (float x) noexcept
    {
        buf[(size_t) w] = x;
        const float y = buf[(size_t) ((w - delay) & mask)];
        w = (w + 1) & mask;
        return y;
    }
};

// Envelope follower with separate attack and release (one pole each), on a linear value.
struct Envelope
{
    float att = 0.1f, rel = 0.01f, v = 0.0f;
    void set (double attMs, double relMs, double fs) { att = onePoleCoef (attMs, fs); rel = onePoleCoef (relMs, fs); }
    inline float process (float x) noexcept { v += (x > v ? att : rel) * (x - v); return v; }
};

} // namespace vs
