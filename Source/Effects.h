#pragma once

// Open Vocal Studio - effects: plate / room reverb, slap/short delay with tempo sync, doubler.
// The reverb and delay engines come from Clean Tone Studio (same author, same license); the
// reverb gains plate and room voicings here.

#include "Base.h"

namespace vs
{
//==============================================================================
// Smooth stereo delay: tape-style glide on time changes, darkening repeats, ping-pong width and
// ducking. Modulation is switchable between a gentle sine wow and "tape": random wow plus flutter,
// with soft saturation that builds up on each repeat. Every control glides per sample.
class SmoothDelay
{
public:
    void prepare (double fs)
    {
        sr = fs;
        size = 1;
        const int needed = (int) (fs * 3.2) + 8;
        while (size < needed) size <<= 1;
        for (auto& b : buf) b.assign ((size_t) size, 0.0f);
        mask = size - 1;
        timeSmooth = onePoleCoef (180.0, fs);
        velCoef = onePoleCoef (15.0, fs);
        paramCoef = onePoleCoef (12.0, fs);    // two stages: ~30 ms S-shaped glide
        duckAtt = onePoleCoef (8.0, fs);
        duckRel = onePoleCoef (320.0, fs);
        hp.setHighPass (sr, 110.0, 0.6);
        resetState();
    }

    void resetState()      // no allocation, O(1): a complete restart, modulation included
    {
        clearAudio();
        w = 0;
        phase = 0.0f;
        wow.init (0x1234567u, sr, 0.6f, 1.4f);          // slow, uneven tape-speed drift
        flutL.init (0x2468aceu, sr, 0.06f, 0.16f);      // fast capstan flutter, a little different per side
        flutR.init (0x13579bdu, sr, 0.06f, 0.16f);
        primed = false;
        idle = true;
    }

    void setParams (float timeMs, float feedback01, float toneHz, float mod01, float duck01,
                    float width01, float mix01, bool enabled, bool tapeStyle = false)
    {
        styleT = tapeStyle ? 1.0f : 0.0f;
        targetDelay = std::clamp ((double) timeMs * 0.001 * sr, 4.0, sr * 3.0);
        fbT = std::min (feedback01, 0.95f);
        modT = mod01 * (float) (0.0016 * sr);
        phaseInc = (float) (2.0 * kPiD * (0.45 + 0.35 * mod01) / sr);
        duckT = duck01; widthT = width01;
        mixT = enabled ? mix01 : 0.0f;
        Biquad t; t.setLowPass (sr, toneHz, 0.6);
        lp.glideTo (t, primed ? kCoefRamp : 0);

        if (! primed)
        {
            curDelay = targetDelay;
            fb.snap (fbT); modDepth.snap (modT); duck.snap (duckT); width.snap (widthT); mix.snap (mixT);
            inFade.snap (1.0f); style.snap (styleT);
            idle = mixT <= 0.0f;
            primed = true;
        }
        else if (mixT > 0.0f && idle)
        {
            clearAudio();                 // waking up: start from silence and fade the input in too,
            curDelay = targetDelay;       // so the first echo doesn't start mid-waveform
            idle = false;
        }
    }

    // L and R are the output channels (R may be nullptr for mono).
    void process (float* L, float* R, int n)
    {
        if (idle)
        {
            // resting: keep the modulation running so it comes back in step, as if it had never stopped
            for (int i = 0; i < n; ++i)
            {
                phase += phaseInc; if (phase > 2.0f * kPi) phase -= 2.0f * kPi;
                wow.next(); flutL.next(); flutR.next();
            }
            return;
        }
        const bool stereo = R != nullptr;
        const float pc = paramCoef;
        for (int i = 0; i < n; ++i)
        {
            // tape-style glide, but capped so the "tape" never runs more than 40% fast or slow
            // the speed itself ramps (like a tape motor), so the glide starts and stops without a corner
            delayVel += velCoef * (std::clamp (timeSmooth * (targetDelay - curDelay), -0.4, 0.4) - delayVel);
            curDelay += delayVel;
            if (std::abs (targetDelay - curDelay) < 1.0e-9 && std::abs (delayVel) < 1.0e-12) { curDelay = targetDelay; delayVel = 0.0; }
            const float fbNow = fb.next (fbT, pc), modNow = modDepth.next (modT, pc);
            const float duckNow = duck.next (duckT, pc), widthNow = width.next (widthT, pc);
            const float mixNow = mix.next (mixT, pc), inNow = inFade.next (1.0f, pc);
            const float wd = stereo ? widthNow : 0.0f;
            lp.advance();

            // modulation: sine wow, or tape (random wow + flutter); switching glides between them
            phase += phaseInc; if (phase > 2.0f * kPi) phase -= 2.0f * kPi;
            const float st = style.next (styleT, pc);
            const float wv = wow.next(), fl = flutL.next(), fr = flutR.next();
            const float mL = std::sin (phase) + st * (std::clamp (0.8f * wv + 0.3f * fl, -1.0f, 1.0f) - std::sin (phase));
            const float mR = std::sin (phase + 1.5708f) + st * (std::clamp (0.8f * wv + 0.3f * fr, -1.0f, 1.0f) - std::sin (phase + 1.5708f));
            const double dL = curDelay + (double) (modNow * (1.0f + mL));
            const double dR = curDelay + (double) (modNow * (1.0f + mR));

            const float yL = hp.process (lp.process (read (buf[0], dL), 0), 0);
            const float yR = stereo ? hp.process (lp.process (read (buf[1], dR), 1), 1) : 0.0f;

            const float in = (stereo ? 0.5f * (L[i] + R[i]) : L[i]) * inNow;
            const float wL = in + fbNow * ((1.0f - wd) * yL + wd * yR);
            const float wR = in * (1.0f - wd) + fbNow * ((1.0f - wd) * yR + wd * yL);
            // tape: gentle saturation on what goes onto the tape, so each repeat gets a little rounder
            buf[0][(size_t) w] = softLimit (wL + st * (tapeSat (wL) - wL));
            buf[1][(size_t) w] = stereo ? softLimit (wR + st * (tapeSat (wR) - wR)) : 0.0f;
            w = (w + 1) & mask;
            if (written < size) ++written;

            const float a = std::abs (in);
            duckEnv += (a > duckEnv ? duckAtt : duckRel) * (a - duckEnv);
            const float duckGain = 1.0f - 0.9f * duckNow * std::min (1.0f, duckEnv * 8.0f);
            const float wet = mixNow * duckGain;

            L[i] += wet * yL;
            if (stereo) R[i] += wet * yR;
        }
        if (mixT <= 0.0f && mix.s1 < 1.0e-6f && mix.value() < 1.0e-6f) { mix.snap (0.0f); idle = true; }
    }
private:
    void clearAudio()
    {
        written = 0;              // older samples now read as silence - no need to erase the buffers
        duckEnv = 0; delayVel = 0;
        mix.snap (0.0f); inFade.snap (0.0f);
        lp.reset(); hp.reset();
    }

    static inline float tapeSat (float x) { return std::tanh (1.4f * x) * (1.0f / 1.4f); }   // unity for small signals

    static inline float softLimit (float x)
    {
        const float a = std::abs (x);
        if (a < 0.8f) return x;
        const float s = 0.8f + 0.2f * std::tanh ((a - 0.8f) * 5.0f);
        return x < 0.0f ? -s : s;
    }

    // a sample written before the last reset reads as silence (lazy clear)
    inline float tap (const std::vector<float>& b, int idx) const
    {
        const int age = (w - 1 - idx) & mask;
        return age < written ? b[(size_t) (idx & mask)] : 0.0f;
    }

    // Read position in double: in float, (write position - delay) only resolves ~1/64 sample once the
    // write position has run up to a few hundred thousand, which quantises the delay time and the wow.
    inline float read (const std::vector<float>& b, double d) const
    {
        double rp = (double) w - d;
        while (rp < 0.0) rp += (double) size;
        const int i1 = (int) rp;
        const float t = (float) (rp - (double) i1);
        const float y0 = tap (b, i1 - 1), y1 = tap (b, i1), y2 = tap (b, i1 + 1), y3 = tap (b, i1 + 2);
        // 4-point Hermite
        const float c0 = y1, c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    double sr = 48000.0;
    std::array<std::vector<float>, 2> buf;
    int size = 1, mask = 0, w = 0, written = 0;
    bool primed = false, idle = true;
    double curDelay = 0, delayVel = 0, targetDelay = 1000, timeSmooth = 0.001, velCoef = 0.01;
    float paramCoef = 0.01f;
    float fbT = 0, modT = 0, duckT = 0, widthT = 0, mixT = 0, styleT = 0;
    Glide fb, modDepth, duck, width, mix, inFade, style;
    SmoothRandom wow, flutL, flutR;
    float phase = 0, phaseInc = 0, duckEnv = 0, duckAtt = 0, duckRel = 0;
    Biquad lp, hp;
};

//==============================================================================
// FDN reverb (from Clean Tone Studio's Clean reverb): early reflections, a 4-stage Hadamard diffuser and a 16-line Householder FDN
// with slow delay modulation (no metallic ringing). Every line has a calibrated absorption shelf
// (Jot): below the Tone frequency it decays in exactly the Decay time, above it about 4x faster,
// so the whole tail decays evenly and Decay means what it says.
class FdnReverb
{
public:
    static constexpr int N = 16;
    static constexpr int kTaps = 8;

    void prepare (double fs)
    {
        sr = fs;
        const double diffMs[4] = { 38.0, 21.0, 11.0, 6.0 };
        juce::Random rng (0x5eed1234);
        for (int s = 0; s < 4; ++s)
        {
            const double range = diffMs[s] * 0.001 * fs;
            for (int c = 0; c < N; ++c)
            {
                const double lo = range * c / N, hi = range * (c + 1) / N;
                const int d = std::max (1, (int) (lo + rng.nextDouble() * (hi - lo)));
                stages[(size_t) s].d[(size_t) c] = d;
                stages[(size_t) s].flip[(size_t) c] = rng.nextBool() ? 1.0f : -1.0f;
                stages[(size_t) s].lines[(size_t) c].init (d + 2);
            }
        }
        for (int c = 0; c < N; ++c)
        {
            fdn[(size_t) c].init ((int) (kBaseMs[c] * 1.45 * 0.001 * fs) + 64);
            const double inc = 2.0 * kPiD * (0.09 + 0.023 * c) / fs;
            rotC[(size_t) c] = std::cos (inc); rotS[(size_t) c] = std::sin (inc);
        }
        // pre-delay line also feeds the early reflections (up to 150 ms + 60 ms x 1.4)
        for (auto& p : pre) p.init ((int) (0.25 * fs) + 4);
        modDepth = (float) (0.00035 * fs);
        lenSmooth = 1.0 - std::exp (-1.0 / (0.250 * fs));
        preSmooth = 1.0 - std::exp (-1.0 / (0.075 * fs));   // two stages: S-shaped, no sudden pitch jump
        paramCoef = onePoleCoef (12.0, fs);    // two stages: ~30 ms S-shaped glide
        duckAtt = onePoleCoef (10.0, fs);
        duckRel = onePoleCoef (450.0, fs);     // blooms back in over about half a second
        duckGCoef = onePoleCoef (5.0, fs);
        resetState();
    }

    void resetState()      // no allocation, O(1) per line: a complete restart, modulation included
    {
        clearAudio();
        for (int c = 0; c < N; ++c) { lfoS[(size_t) c] = std::sin (c * 0.392); lfoC[(size_t) c] = std::cos (c * 0.392); }
        primed = false;
        idle = true;
    }

    void setParams (float size01, float decaySec, float preDelayMs, float toneHz, float lowCutHz,
                    float mix01, bool enabled, float duck01 = 0.0f)
    {
        duckAmt = std::clamp (duck01, 0.0f, 1.0f);
        const double scale = 0.4 + size01;
        const double t60 = std::max (decaySec, 0.1f), t60hf = t60 * hfRatio;
        for (int c = 0; c < N; ++c)
        {
            target[(size_t) c] = kBaseMs[c] * scale * 0.001 * sr;
            const double secs = target[(size_t) c] / sr;
            gLfT[(size_t) c] = std::pow (10.0, -3.0 * secs / t60);     // exact decay below Tone...
            gHfT[(size_t) c] = std::pow (10.0, -3.0 * secs / t60hf);   // ...and faster above it
        }
        erScaleT = scale;
        kT = 1.0 / std::tan (kPiD * std::clamp ((double) toneHz, 200.0, 0.45 * sr) / sr);
        preT = std::clamp ((double) preDelayMs * 0.001 * sr, 0.0, (double) (pre[0].mask) - 0.07 * 1.4 * sr - 8.0);
        Biquad th, tl;
        th.setHighPass (sr, lowCutHz, 0.6);
        tl.setLowPass (sr, std::min (toneHz * 1.6f, 18000.0f), 0.6);
        inHp.glideTo (th, primed ? kCoefRamp : 0);
        inLp.glideTo (tl, primed ? kCoefRamp : 0);
        mixT = enabled ? mix01 : 0.0f;

        if (! primed)
        {
            snapControls();
            mix.snap (mixT); inFade.snap (1.0f);
            idle = mixT <= 0.0f;
            primed = true;
        }
        else if (mixT > 0.0f && idle)
        {
            clearAudio();                 // waking up: start from silence and fade the input in too
            snapControls();
            idle = false;
        }
    }

    void process (float* L, float* R, int n)
    {
        if (idle)
        {
            // resting: keep the modulation LFOs turning so the reverb comes back in step
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < N; ++c) stepLfo (c);
            return;
        }
        const bool stereo = R != nullptr;
        std::array<float, N> x {}, y {};
        for (int i = 0; i < n; ++i)
        {
            const float mixNow = mix.next (mixT, paramCoef);
            const float inNow = inFade.next (1.0f, paramCoef);
            glide (preMid, preT, preSmooth);
            glide (preCur, preMid, preSmooth);
            glide (erScale, erScaleT, lenSmooth);
            glide (kCur, kT, lenSmooth);
            inHp.advance(); inLp.advance();

            float inL = inLp.process (inHp.process (L[i] * inNow, 0), 0);
            float inR = stereo ? inLp.process (inHp.process (R[i] * inNow, 1), 1) : inL;
            pre[0].push (inL); pre[1].push (inR);
            const float pL = pre[0].readFrac ((float) preCur), pR = pre[1].readFrac ((float) preCur);

            // early reflections: a few distinct, decaying echoes, different on each side
            float erL = 0.0f, erR = 0.0f;
            for (int t = 0; t < kTaps; ++t)
            {
                erL += kErGain[t] * pre[0].readFrac ((float) (preCur + kErMsL[t] * 0.001 * sr * erScale));
                erR += kErGain[t] * pre[1].readFrac ((float) (preCur + kErMsR[t] * 0.001 * sr * erScale));
            }

            for (int c = 0; c < N; ++c) x[(size_t) c] = ((c & 1) ? pR : pL) * 0.35355339f;   // 8 copies each: keep the energy

            for (auto& s : stages)
            {
                for (int c = 0; c < N; ++c)
                {
                    auto& line = s.lines[(size_t) c];
                    const float out = line.readInt (s.d[(size_t) c] - 1);
                    line.push (x[(size_t) c]);
                    x[(size_t) c] = out * s.flip[(size_t) c];
                }
                hadamard (x);
            }

            const double K = kCur, invK1 = 1.0 / (K + 1.0), a1 = (1.0 - K) * invK1;
            float sum = 0.0f;
            std::array<float, N> fb {};
            for (int c = 0; c < N; ++c)
            {
                auto& cc = cur[(size_t) c];
                glide (cc, target[(size_t) c], lenSmooth);
                glide (gLf[(size_t) c], gLfT[(size_t) c], lenSmooth);
                glide (gHf[(size_t) c], gHfT[(size_t) c], lenSmooth);
                stepLfo (c);
                const float d = (float) cc + modDepth * (1.0f + (float) lfoS[(size_t) c]);
                y[(size_t) c] = fdn[(size_t) c].readFrac (d - 1.0f);
                // absorption shelf: gain gLf below the crossover, gHf above (first order, prewarped)
                const double b0 = (gHf[(size_t) c] * K + gLf[(size_t) c]) * invK1;
                const double b1 = (gLf[(size_t) c] - gHf[(size_t) c] * K) * invK1;
                auto& z = abs[(size_t) c];
                const double o = b0 * y[(size_t) c] + z;
                z = b1 * y[(size_t) c] - a1 * o;
                fb[(size_t) c] = (float) o;
                sum += (float) o;
            }
            const float hh = (2.0f / N) * sum;
            for (int c = 0; c < N; ++c)
                fdn[(size_t) c].push (fb[(size_t) c] - hh + x[(size_t) c]);

            float oL = 0.0f, oR = 0.0f;
            for (int c = 0; c < N; c += 2) { const float sgn = (c & 2) ? -1.0f : 1.0f; oL += sgn * y[(size_t) c]; oR += sgn * y[(size_t) (c + 1)]; }
            oL = oL * kOutScale + erLevel * erL;
            oR = oR * kOutScale + erLevel * erR;
            // ducking: the reverb backs off while you play and blooms in the gaps
            {
                const float a = stereo ? 0.5f * (std::abs (L[i]) + std::abs (R[i])) : std::abs (L[i]);
                duckEnv += (a > duckEnv ? duckAtt : duckRel) * (a - duckEnv);
            }
            const float duckTarget = 1.0f - 0.85f * duckAmt * std::min (1.0f, duckEnv * 8.0f);
            duckG += duckGCoef * (duckTarget - duckG);
            const float wetNow = mixNow * duckG;
            if (stereo) { L[i] += wetNow * oL; R[i] += wetNow * oR; }
            else        { L[i] += wetNow * 0.5f * (oL + oR); }
        }
        if (mixT <= 0.0f && mix.s1 < 1.0e-6f && mix.value() < 1.0e-6f) { mix.snap (0.0f); idle = true; }
    }
private:
    static constexpr double kBaseMs[N] = { 31.3, 35.9, 40.7, 44.9, 49.3, 54.1, 58.7, 63.1,
                                           68.3, 73.9, 79.1, 84.7, 91.3, 98.1, 105.7, 113.9 };
    static constexpr double kErMsL[kTaps] = { 7.1, 11.3, 17.9, 23.3, 29.9, 37.1, 44.3, 53.9 };
    static constexpr double kErMsR[kTaps] = { 8.3, 13.7, 19.1, 26.9, 31.7, 41.3, 47.9, 57.1 };
    static constexpr float kErGain[kTaps] = { 0.80f, -0.66f, 0.54f, 0.44f, -0.36f, 0.30f, -0.24f, 0.20f };
    static constexpr float kOutScale = 0.35355339f;   // 8 lines per side
    float erLevel = 0.30f;      // early reflections (a room has them, a plate doesn't)
    double hfRatio = 0.25;      // highs decay this much faster than the Decay time
public:
    void setVoicing (float earlyLevel, double highDecayRatio) { erLevel = earlyLevel; hfRatio = highDecayRatio; }
private:

    struct Line
    {
        // 'filled' counts samples written since the last clear; older ones read as silence (lazy clear)
        std::vector<float> buf; int mask = 0, w = 0, filled = 0;
        void init (int maxLen) { int s = 1; while (s < maxLen + 4) s <<= 1; buf.assign ((size_t) s, 0.0f); mask = s - 1; w = 0; filled = 0; }
        void clear() { filled = 0; }
        inline void push (float v) { buf[(size_t) w] = v; w = (w + 1) & mask; if (filled <= mask) ++filled; }
        inline float at (int age) const { return age < filled ? buf[(size_t) ((w - 1 - age) & mask)] : 0.0f; }
        inline float readInt (int d) const { return at (d); }
        inline float readFrac (float d) const
        {
            const int i = (int) d; const float f = d - (float) i;
            const float a = at (i), b = at (i + 1);
            return a + f * (b - a);
        }
    };
    struct Stage { std::array<Line, N> lines; std::array<int, N> d {}; std::array<float, N> flip {}; };

    // one-pole glide in double precision that lands exactly on its target (float would stall short of it)
    static inline void glide (double& v, double t, double c) { v += c * (t - v); if (std::abs (t - v) < 1.0e-9 * (1.0 + std::abs (t))) v = t; }

    void snapControls()
    {
        cur = target; gLf = gLfT; gHf = gHfT; preCur = preMid = preT; erScale = erScaleT; kCur = kT;
    }

    void clearAudio()
    {
        for (auto& s : stages) for (auto& l : s.lines) l.clear();
        for (auto& l : fdn) l.clear();
        for (auto& p : pre) p.clear();
        abs.fill (0.0);
        inHp.reset(); inLp.reset();
        mix.snap (0.0f); inFade.snap (0.0f);
        duckEnv = 0.0f; duckG = 1.0f;
    }

    static void hadamard (std::array<float, N>& v)
    {
        for (int h = 1; h < N; h *= 2)
            for (int i = 0; i < N; i += h * 2)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = v[(size_t) j], b = v[(size_t) (j + h)];
                    v[(size_t) j] = a + b; v[(size_t) (j + h)] = a - b;
                }
        for (auto& e : v) e *= 0.25f;   // 1/sqrt(16)
    }

    double sr = 48000.0;
    std::array<Stage, 4> stages;
    std::array<Line, N> fdn;
    std::array<Line, 2> pre;
    std::array<double, N> cur {}, target {}, gLf {}, gLfT {}, gHf {}, gHfT {}, abs {};
    // modulation LFOs as rotating phasors (sine and cosine), a few multiplies per sample instead of sin()
    std::array<double, N> lfoS {}, lfoC {}, rotS {}, rotC {};
    inline void stepLfo (int c) noexcept
    {
        const double s = lfoS[(size_t) c], co = lfoC[(size_t) c];
        double ns = s * rotC[(size_t) c] + co * rotS[(size_t) c];
        double nc = co * rotC[(size_t) c] - s * rotS[(size_t) c];
        const double g = 1.5 - 0.5 * (ns * ns + nc * nc);     // keeps the amplitude at exactly 1
        lfoS[(size_t) c] = ns * g; lfoC[(size_t) c] = nc * g;
    }
    float modDepth = 0, paramCoef = 0.01f;
    double lenSmooth = 0, preSmooth = 0, preCur = 0, preMid = 0, preT = 0, erScale = 1, erScaleT = 1, kCur = 1, kT = 1;
    float mixT = 0;
    float duckAmt = 0, duckEnv = 0, duckG = 1, duckAtt = 0.01f, duckRel = 0.001f, duckGCoef = 0.01f;
    Glide mix, inFade;
    bool primed = false, idle = true;
    Biquad inHp, inLp;
};
//==============================================================================
// Doubler (ADT): two short, slowly wandering delays (about 12 and 19 ms) panned left and right,
// a few cents of drift each and slightly darker than the voice, like a second take.
class Doubler
{
public:
    void prepare (double fs)
    {
        sr = fs;
        int s = 1; while (s < (int) (0.05 * fs) + 8) s <<= 1;
        buf.assign ((size_t) s, 0.0f); mask = s - 1; w = 0;
        rL.init (0xabcdef1u, fs, 0.4f, 1.1f); rR.init (0x1357ace5u, fs, 0.5f, 1.3f);
        lpL.setLowPass (fs, 7500.0, 0.6); lpR = lpL; lpL.reset(); lpR.reset();
        hpL.setHighPass (fs, 180.0, 0.6); hpR = hpL; hpL.reset(); hpR.reset();
        lvl.snap (0.0f); shape.snap (0.0f); pc = onePoleCoef (12.0, fs); pcSlow = onePoleCoef (300.0, fs);
    }
    // The one Doubler knob (0..1) sets three things, the extra mostly in the upper half:
    //  - the copies' level: -6 dB at 100 % up to 1.2.5, now -3 dB (0.5 a + 0.21 a^2)
    //  - their timing drift: +-1.6 / 2 ms up to +-3 ms
    //  - how fast it drifts: up to 1.5x slower, so the pitch wander it causes (a real second
    //    take is never exactly in tune with the first) grows only a little, to ~7-8 cents
    // Drift depth and speed follow the knob over ~300 ms: a knob move never jerks the delay.
    void setLevel (float a01) { amtT = a01; }
    void process (const float* mono, float* L, float* R, int n)
    {
        if (amtT <= 0.0f && lvl.value() < 1e-6f)
        {
            for (int i = 0; i < n; ++i) { rL.next (1.0f); rR.next (1.0f); }
            lvl.snap (0.0f); shape.snap (0.0f);
            return;
        }
        for (int i = 0; i < n; ++i)
        {
            buf[(size_t) w] = mono[i];
            const float a = lvl.next (amtT, pc);
            const float sh = shape.next (amtT * amtT, pcSlow);
            const float speed = 1.0f / (1.0f + 0.5f * sh);
            const double dL = (0.0120 + (0.0016 + 0.0014 * sh) * rL.next (speed)) * sr;
            const double dR = (0.0190 + (0.0020 + 0.0010 * sh) * rR.next (speed)) * sr;
            const float yl = hpL.process (lpL.process (read (dL), 0), 0);
            const float yr = hpR.process (lpR.process (read (dR), 0), 0);
            w = (w + 1) & mask;
            const float g = 0.5f * a + 0.21f * a * a;
            L[i] += g * yl;
            if (R != nullptr) R[i] += g * yr; else L[i] += g * yr;
        }
    }
private:
    float read (double d) const
    {
        double rp = (double) w - d; while (rp < 0) rp += (double) (mask + 1);
        const int i1 = (int) rp; const float t = (float) (rp - i1);
        auto at = [&] (int k) { return buf[(size_t) (k & mask)]; };
        const float y0 = at (i1 - 1), y1 = at (i1), y2 = at (i1 + 1), y3 = at (i1 + 2);
        const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }
    double sr = 48000;
    std::vector<float> buf; int mask = 0, w = 0;
    SmoothRandom rL, rR;
    Biquad lpL, lpR, hpL, hpR;
    Glide lvl, shape; float amtT = 0, pc = 0.01f, pcSlow = 0.001f;
};
} // namespace vs
