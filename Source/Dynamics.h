#pragma once

// Open Vocal Studio - time-domain processing: rumble filter, de-plosive, tone EQ (with the Analyze
// auto-match), air, vocal rider, compressor and a true-peak limiter.

#include "Base.h"

namespace vs
{
//==============================================================================
// 4th-order Butterworth high-pass (24 dB/oct) with gliding cutoff.
class RumbleFilter
{
public:
    void prepare (double fs) { sr = fs; f.logScale = true; f.reset(); a.reset(); b.reset(); primed = false; tick = 0; }
    void setCutoff (float hz) { f.setTarget (hz); if (! primed) { update (0); primed = true; } }
    void process (float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            // coefficient updates on a running sample count (not the position in the host's
            // buffer), so the result is the same at every buffer size
            if ((tick++ & (kCoefRamp - 1)) == 0 && f.step (0.02f)) update (kCoefRamp);
            a.advance(); b.advance();
            x[i] = b.process (a.process (x[i], 0), 0);
        }
    }
private:
    void update (int ramp)
    {
        Biquad t1, t2;
        t1.setHighPass (sr, f.value(), 0.5412); t2.setHighPass (sr, f.value(), 1.3066);
        a.glideTo (t1, ramp); b.glideTo (t2, ramp);
    }
    double sr = 48000;
    SmoothParam f;
    Biquad a, b;
    bool primed = false;
    int64_t tick = 0;
};

//==============================================================================
// De-plosive: a detector compares the band below 150 Hz with the band above it (Linkwitz-Riley
// split); when a low burst is much stronger than the rest of the voice, a dynamic low shelf at
// 160 Hz turns it down for just that moment. At rest the shelf is exactly flat (bit-transparent).
// With look-ahead (Mix mode) the cut is already in place when the "p" arrives.
// Consonant (same stage, same look-ahead): a dynamic dip around 3.5 kHz for the click of K, T and
// hard C (see process()); also exactly flat at rest.
class Deplosive
{
public:
    void prepare (double fs, int lookaheadSamples)
    {
        sr = fs; la = lookaheadSamples;
        lp1.setLowPass (fs, 150.0, 0.7071); lp2 = lp1;
        hp1.setHighPass (fs, 150.0, 0.7071); hp2 = hp1;
        dl.prepare (la + 4); dl.setDelay (la);
        envLo.set (0.5, 50.0, fs); envHi.set (0.5, 50.0, fs);
        gAtt = onePoleCoef (1.0, fs); gRel = onePoleCoef (45.0, fs);
        // consonants: the 1.8-9 kHz band, its sudden rises against what it usually does there
        cHp.setHighPass (fs, 1800.0, 0.7071); cLp.setLowPass (fs, std::min (9000.0, 0.45 * fs), 0.7071);
        envCF.set (0.3, 8.0, fs); envCS.set (20.0, 80.0, fs); envAll.set (0.3, 8.0, fs);
        cAtt = onePoleCoef (0.5, fs); cRel = onePoleCoef (30.0, fs);
        reset();
    }
    void reset()
    {
        lp1.reset(); lp2.reset(); hp1.reset(); hp2.reset(); shelf.reset(); shelf.setIdentity();
        dl.clear(); envLo.v = envHi.v = 0; gr = 0; lastGr = 0;
        cHp.reset(); cLp.reset(); bell.reset(); bell.setIdentity(); tick = 0;
        envCF.v = envCS.v = envAll.v = 0; cgr = 0; lastCgr = 0;
    }
    void setAmount (float a) { amount = a; }
    void setConsonant (float a) { consonant = a; }
    int getLatency() const { return la; }
    std::atomic<float> grDb { 0.0f }, consGrDb { 0.0f };

    void process (float* x, int n)
    {
        float peak = 0.0f, cPeak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];
            const bool rampTick = (tick++ & (kCoefRamp - 1)) == 0;    // running count: buffer-size independent
            // Consonant: the click of K, T and hard C - a burst in 1.8-9 kHz that rises suddenly
            // (against what that band did over the last ~50 ms) and carries most of the moment's
            // energy. A vowel's onset doesn't qualify (its energy sits lower), a held S only at
            // its very start (then the slow follower catches up; De-ess handles the rest).
            const float b = cLp.process (cHp.process (in, 0), 0);
            const float eF = envCF.process (std::abs (b)), eS = envCS.process (std::abs (b)), eA = envAll.process (std::abs (in));
            float cTarget = 0.0f;
            if (consonant > 0.0f && eF > 1.0e-3f)
            {
                const float rise = gainToDb (eF) - gainToDb (eS + 1.0e-7f);
                const float share = gainToDb (eF) - gainToDb (eA + 1.0e-7f);   // 0 dB: the band is all of it
                const float shareW = std::clamp ((share + 9.0f) / 6.0f, 0.0f, 1.0f);
                cTarget = std::clamp ((rise - 6.0f) * (0.6f + 0.6f * consonant) * shareW, 0.0f, 14.0f * consonant);
            }
            cgr += (cTarget > cgr ? cAtt : cRel) * (cTarget - cgr);
            if (cgr < 1e-4f && cTarget == 0.0f) cgr = 0.0f;
            if (rampTick && cgr != lastCgr)
            {
                Biquad t;
                if (cgr == 0.0f) t.setIdentity(); else t.setPeak (sr, 3500.0, 0.6, -cgr);
                bell.glideTo (t, kCoefRamp);
                lastCgr = cgr;
            }
            bell.advance();
            cPeak = std::max (cPeak, cgr);

            const float eL = envLo.process (std::abs (lp2.process (lp1.process (in, 0), 0)));
            const float eH = envHi.process (std::abs (hp2.process (hp1.process (in, 0), 0)));
            float target = 0.0f;
            if (amount > 0.0f && eL > 3.0e-3f)
            {
                const float ratioDb = gainToDb (eL) - gainToDb (eH + 1e-6f);
                target = std::clamp ((ratioDb - 2.0f) * 1.5f * amount, 0.0f, 24.0f * amount);
            }
            gr += (target > gr ? gAtt : gRel) * (target - gr);
            if (gr < 1e-4f && target == 0.0f) gr = 0.0f;
            if (rampTick && gr != lastGr)
            {
                Biquad t;
                if (gr == 0.0f) t.setIdentity(); else t.setLowShelf (sr, 160.0, -gr);
                shelf.glideTo (t, kCoefRamp);
                lastGr = gr;
            }
            shelf.advance();
            x[i] = bell.process (shelf.process (dl.process (in), 0), 0);
            peak = std::max (peak, gr);
        }
        grDb.store (-peak);
        consGrDb.store (-cPeak);
    }
private:
    double sr = 48000;
    int la = 0;
    float amount = 0, gr = 0, lastGr = 0, gAtt = 0.1f, gRel = 0.01f;
    float consonant = 0, cgr = 0, lastCgr = 0, cAtt = 0.1f, cRel = 0.01f;
    int64_t tick = 0;
    Biquad lp1, lp2, hp1, hp2, shelf, cHp, cLp, bell;
    Envelope envLo, envHi, envCF, envCS, envAll;
    DelayLine dl;
};

//==============================================================================
// Tone EQ: 8 peaking bands an octave apart (63 Hz .. 8 kHz) + a 12.5 kHz shelf, and an Air
// shelf. Gains come from Analyze (matched to a reference vocal curve) and can be edited.
class VocalEq
{
public:
    static constexpr int kBands = 9;
    static constexpr float kFreqs[kBands] = { 63, 125, 250, 500, 1000, 2000, 4000, 8000, 12500 };

    void prepare (double fs)
    {
        sr = fs;
        for (auto& g : gains) { g.reset(); }
        air.reset();
        for (auto& b : bq) b.reset();
        primed = false; tick = 0;
    }
    void setGains (const std::array<float, kBands>& db, float amount01, float airDb)
    {
        for (int b = 0; b < kBands; ++b) gains[(size_t) b].setTarget (db[(size_t) b] * amount01);
        air.setTarget (airDb);
        if (! primed) { update (0); primed = true; }
    }
    void process (float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if ((tick++ & (kCoefRamp - 1)) == 0)     // running count: buffer-size independent
            {
                bool moved = air.step (0.02f);
                for (auto& g : gains) moved |= g.step (0.02f);
                if (moved) update (kCoefRamp);
            }
            float v = x[i];
            for (auto& b : bq) { b.advance(); v = b.process (v, 0); }
            x[i] = v;
        }
    }

    // Magnitude (dB) at f of a set of band gains (for the auto-EQ fit and the display)
    static double responseDb (double fs, const std::array<float, kBands>& g, double airDb, double f)
    {
        std::array<Biquad, kBands + 1> b;
        design (fs, g, (float) airDb, b);
        double db = 0;
        for (auto& q : b) db += magDb (q, fs, f);
        return db;
    }

    // Reference long-term spectrum of a finished pop/rock lead vocal, dB per octave band,
    // relative to the 500 Hz - 2 kHz average. Averaged from published long-term spectra of
    // singing voice (e.g. Sundberg, "The Science of the Singing Voice") with a modern presence lift.
    static constexpr float kTarget[kBands] = { -16.0f, -7.0f, -1.5f, 0.5f, 0.0f, -0.5f, -3.0f, -8.0f, -13.0f };

    // Fit band gains so voice LTAS + EQ approaches the target. Returns gains (dB, clamped).
    static std::array<float, kBands> matchFromLtas (double fs, const std::array<double, kBands>& ltas)
    {
        std::array<double, kBands> m {};
        for (int b = 0; b < kBands; ++b) m[(size_t) b] = 10.0 * std::log10 (ltas[(size_t) b] + 1e-20);
        const double ref = (m[3] + m[4] + m[5]) / 3.0;
        const double tRef = (kTarget[3] + kTarget[4] + kTarget[5]) / 3.0;
        std::array<double, kBands> want {};
        for (int b = 0; b < kBands; ++b)
        {
            double d = (kTarget[b] - tRef) - (m[(size_t) b] - ref);
            // never boost the deep lows (that's rumble), and go easy on the top
            if (b <= 1) d = std::min (d, 0.0);
            if (b == kBands - 1) d = std::min (d, 4.0);
            want[(size_t) b] = std::clamp (d * 0.8, -6.0, 6.0);     // 80 %: a match, not a clone
        }
        // iterate: the bands overlap, so correct the actual response at each center
        std::array<float, kBands> g {};
        for (int b = 0; b < kBands; ++b) g[(size_t) b] = (float) want[(size_t) b];
        for (int it = 0; it < 12; ++it)
            for (int b = 0; b < kBands; ++b)
            {
                const double r = responseDb (fs, g, 0.0, kFreqs[b]);
                g[(size_t) b] = (float) std::clamp (g[(size_t) b] + 0.6 * (want[(size_t) b] - r), -9.0, 9.0);
            }
        for (auto& v : g) v = std::round (v * 10.0f) / 10.0f;
        return g;
    }

private:
    static void design (double fs, const std::array<float, kBands>& g, float airDb, std::array<Biquad, kBands + 1>& b)
    {
        for (int i = 0; i < kBands - 1; ++i) b[(size_t) i].setPeak (fs, std::min ((double) kFreqs[i], 0.42 * fs), 1.414, g[(size_t) i]);
        b[kBands - 1].setHighShelf (fs, std::min (10000.0, 0.4 * fs), g[kBands - 1]);
        b[kBands].setHighShelf (fs, std::min (12000.0, 0.42 * fs), airDb);
    }
    static double magDb (const Biquad& q, double fs, double f)
    {
        const std::complex<double> z = std::polar (1.0, -2.0 * kPiD * f / fs);
        const auto num = q.b0 + q.b1 * z + q.b2 * z * z;
        const auto den = 1.0 + q.a1 * z + q.a2 * z * z;
        return 20.0 * std::log10 (std::abs (num / den) + 1e-30);
    }
    void update (int ramp)
    {
        std::array<float, kBands> g {};
        for (int b = 0; b < kBands; ++b) g[(size_t) b] = gains[(size_t) b].value();
        std::array<Biquad, kBands + 1> t;
        design (sr, g, air.value(), t);
        for (size_t i = 0; i < bq.size(); ++i) bq[i].glideTo (t[i], ramp);
    }
    double sr = 48000;
    std::array<SmoothParam, kBands> gains;
    SmoothParam air;
    std::array<Biquad, kBands + 1> bq;
    bool primed = false;
    int64_t tick = 0;
};

//==============================================================================
// Vocal rider: evens out phrase-to-phrase level before the compressor, so the compressor can work
// gently. It measures the short-term level (300 ms, voice-weighted) of sung passages only and
// moves the gain slowly toward the long-term level of the voice, within +/- range.
class Rider
{
public:
    void prepare (double fs)
    {
        sr = fs;
        hp.setHighPass (fs, 150.0, 0.7071);
        shelf.setHighShelf (fs, 1500.0, 4.0);
        ms = onePoleCoef (300.0, fs);
        longC = onePoleCoef (4000.0, fs);
        fastC = onePoleCoef (400.0, fs);
        learnLen = (int64_t) (3.0 * fs);
        gC = onePoleCoef (250.0, fs);
        msFastC = onePoleCoef (50.0, fs);
        holdLen = (int64_t) (0.4 * fs);
        lowLen = (int64_t) (2.0 * fs);
        relearnC = onePoleCoef (1000.0, fs);
        relearnLen = (int64_t) (4.0 * fs);
        highLen = (int64_t) (0.3 * fs);
        reset();
    }
    void reset() { hp.reset(); shelf.reset(); msv = 0; msFast = 0; longDb = -24.0f; gDb = 0; primedLong = false; learned = 0; holdoff = 0; lowRun = 0; relearnLeft = 0; highRun = 0; }
    void setRange (float db) { range = db; }
    float getLongDb() const { return longDb; }
    std::atomic<float> gainDb { 0.0f };

    void process (float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float w = shelf.process (hp.process (x[i], 0), 0);
            msv += ms * (w * w - msv);
            const float st = 10.0f * std::log10 (msv + 1e-12f);
            msFast += msFastC * (w * w - msFast);
            // after silence (a stopped DAW, a pause) the short-term level needs a moment to come
            // back up, and at the end of a phrase it falls away: in both cases hold the gain and
            // don't learn (riding up a fading tail or a breath is exactly what a rider must not do)
            const bool falling = 10.0f * std::log10 (msFast + 1e-12f) < st - 6.0f;
            if (st <= -60.0f) holdoff = holdLen; else if (holdoff > 0) --holdoff;
            const bool settled = holdoff == 0 && ! falling;
            if (st > -60.0f && settled)
            {
                if (! primedLong && st > -45.0f) { longDb = st; primedLong = true; }
                // far louder than what it learned, for longer than a shout (the voice coming in
                // after cleaned-up room tone): take that as the level, and learn quickly from there
                if (primedLong && st > longDb + 12.0f) { if (++highRun > highLen) { longDb = st; learned = 0; highRun = 0; } }
                else highRun = 0;
                if (st > longDb - 15.0f)
                {
                    // learn quickly over the first 3 s of singing, then follow slowly
                    const float c = learned < learnLen ? fastC : (relearnLeft > 0 ? relearnC : longC);
                    longDb += c * (st - longDb);
                    ++learned;
                    lowRun = 0;
                    if (relearnLeft > 0) --relearnLeft;
                }
                // a quiet word doesn't count, but 2 s of singing far below it means the level has
                // dropped for good (a quieter take, the input turned down): follow it down (1 s)
                else if (primedLong && ++lowRun > lowLen) { relearnLeft = relearnLen; longDb += relearnC * (st - longDb); }
            }
            float target = gDb;
            if (range > 0.0f && settled && st > longDb - 18.0f && primedLong) target = std::clamp (longDb - st, -range, range);
            else if (range <= 0.0f) target = 0.0f;
            gDb += gC * (target - gDb);
            x[i] *= dbToGain (gDb);
        }
        gainDb.store (gDb);
    }
private:
    double sr = 48000;
    Biquad hp, shelf;
    float ms = 0.001f, longC = 0.0001f, fastC = 0.001f, gC = 0.001f, msv = 0, longDb = -24, gDb = 0, range = 0;
    float msFast = 0, msFastC = 0.01f, relearnC = 0.0001f;
    bool primedLong = false; int64_t relearnLeft = 0, relearnLen = 0, highRun = 0, highLen = 0;
    int64_t learned = 0, learnLen = 0, holdoff = 0, holdLen = 0, lowRun = 0, lowLen = 0;
};

//==============================================================================
// Compressor with an auto threshold that follows the voice's long-term level (so 'Compress'
// means the same amount of control whatever the input level), soft knee, program-dependent
// release (short peaks recover fast, sustained compression recovers slowly) and auto make-up.
// Everything moves per sample, so the result doesn't depend on the host's block size.
class VocalCompressor
{
public:
    void prepare (double fs)
    {
        sr = fs;
        det = onePoleCoef (3.0, fs);
        att = onePoleCoef (4.0, fs);
        memC = onePoleCoef (800.0, fs);
        ms = onePoleCoef (300.0, fs);
        longC = onePoleCoef (4000.0, fs);
        amtC = onePoleCoef (20.0, fs);
        fastC = onePoleCoef (400.0, fs);
        learnLen = (int64_t) (3.0 * fs);
        holdLen = (int64_t) (0.4 * fs);
        msFastC = onePoleCoef (50.0, fs);
        lowLen = (int64_t) (2.0 * fs);
        relearnC = onePoleCoef (1000.0, fs);
        relearnLen = (int64_t) (4.0 * fs);
        highLen = (int64_t) (0.3 * fs);
        reset();
    }
    void reset() { env = 0; gr = 0; mem = 0; msv = 0; msFast = 0; longDb = -24.0f; primedLong = false; learned = 0; holdoff = 0; lowRun = 0; relearnLeft = 0; highRun = 0; amt.snap (amountT); }
    void setAmount (float a) { amountT = a; }
    std::atomic<float> grDb { 0.0f };

    void process (float* x, int n)
    {
        if (amountT <= 0.0f && amt.value() <= 0.0f && gr < 1e-4f) { gr = 0; grDb.store (0.0f); return; }
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float a = amt.next (amountT, amtC);
            if (a <= 0.0f && amountT <= 0.0f) amt.snap (0.0f);
            // the voice's usual level (sung passages only)
            msv += ms * (x[i] * x[i] - msv);
            const float st = 10.0f * std::log10 (msv + 1e-12f);
            msFast += msFastC * (x[i] * x[i] - msFast);
            // don't learn from the ramp after silence, nor from a phrase fading out
            const bool falling = 10.0f * std::log10 (msFast + 1e-12f) < st - 6.0f;
            if (st <= -60.0f) holdoff = holdLen; else if (holdoff > 0) --holdoff;
            const bool settled = holdoff == 0 && ! falling;
            if (! primedLong && st > -45.0f && settled) { longDb = st; primedLong = true; }
            // far louder than what it learned, for longer than a shout (the voice coming in after
            // cleaned-up room tone): learn the new level quickly again
            if (primedLong && settled && st > longDb + 12.0f) { if (++highRun > highLen) { longDb = st; learned = 0; highRun = 0; } }
            else if (settled) highRun = 0;
            if (primedLong && settled && st > longDb - 15.0f)
            {
                longDb += (learned < learnLen ? fastC : (relearnLeft > 0 ? relearnC : longC)) * (st - longDb);
                ++learned;
                lowRun = 0;
                if (relearnLeft > 0) --relearnLeft;
            }
            // 2 s of singing far below the learned level: the level has dropped for good, follow it (1 s)
            else if (primedLong && settled && st > -60.0f && ++lowRun > lowLen) { relearnLeft = relearnLen; longDb += relearnC * (st - longDb); }
            const float ratio = 1.5f + 2.5f * a;
            const float thr = longDb + 1.0f - 10.0f * a;
            const float makeup = 0.7f * staticGr (longDb + 3.0f - thr, ratio, kKnee) * std::min (1.0f, a * 20.0f);
            env += det * (x[i] * x[i] - env);
            const float lvl = 10.0f * std::log10 (env + 1e-12f);
            const float target = a > 0.0f ? staticGr (lvl - thr, ratio, kKnee) * std::min (1.0f, a * 20.0f) : 0.0f;
            const float relMs = 60.0f + 350.0f * std::clamp (mem / 6.0f, 0.0f, 1.0f);
            const float rel = (float) (1.0 - std::exp (-1.0 / (relMs * 0.001 * sr)));
            gr += (target > gr ? att : rel) * (target - gr);
            mem += memC * (gr - mem);
            x[i] *= dbToGain (makeup - gr);
            peak = std::max (peak, gr);
        }
        grDb.store (-peak);
    }
private:
    static constexpr float kKnee = 6.0f;
    static float staticGr (float over, float ratio, float knee)
    {
        if (over <= -knee * 0.5f) return 0.0f;
        if (over >= knee * 0.5f) return over * (1.0f - 1.0f / ratio);
        const float t = over + knee * 0.5f;
        return (1.0f - 1.0f / ratio) * t * t / (2.0f * knee);
    }
    double sr = 48000;
    float det = 0.01f, att = 0.01f, memC = 0.0001f, ms = 0.001f, longC = 0.0001f, fastC = 0.001f, amtC = 0.001f;
    int64_t learned = 0, learnLen = 0, holdoff = 0, holdLen = 0, lowRun = 0, lowLen = 0;
    float msFast = 0, msFastC = 0.01f, relearnC = 0.0001f;
    float env = 0, gr = 0, mem = 0, msv = 0, longDb = -24, amountT = 0;
    bool primedLong = false; int64_t relearnLeft = 0, relearnLen = 0, highRun = 0, highLen = 0;
    Glide amt;
};

//==============================================================================
// True-peak limiter (stereo-linked): 4x oversampled peak detection (ITU-R BS.1770 style
// interpolation), look-ahead with a sliding minimum, smooth attack over the look-ahead and a
// release that never pumps on short peaks. Ceiling -1 dBTP.
class TruePeakLimiter
{
public:
    void prepare (double fs, int lookahead)
    {
        sr = fs; la = std::max (1, lookahead);
        for (auto& d : dl) { d.prepare (la + 8); d.setDelay (la); }
        // 4x interpolation FIR (windowed sinc, 48 taps total = 12 per phase)
        for (int p = 0; p < 4; ++p)
            for (int t = 0; t < 12; ++t)
            {
                const double d = (t - 5.5) - (p / 4.0 - 0.5) ;
                const double x = d;
                const double s = std::abs (x) < 1e-9 ? 1.0 : std::sin (kPiD * x) / (kPiD * x);
                const double w = 0.5 + 0.5 * std::cos (kPiD * d / 6.5);
                fir[(size_t) p][(size_t) t] = (float) (s * w);
            }
        hist.fill ({});
        minBuf.assign ((size_t) la + 1, 1.0f);
        box.assign ((size_t) la, 1.0f);
        rel = onePoleCoef (60.0, fs);
        reset();
    }
    void reset()
    {
        for (auto& d : dl) d.clear();
        hist = {}; hpos = 0; mpos = 0; bpos = 0; resum = 0; env = 1.0f;
        std::fill (minBuf.begin(), minBuf.end(), 1.0f);
        std::fill (box.begin(), box.end(), 1.0f); boxSum = la;
    }
    void setCeilingDb (float db) { ceiling = dbToGain (db); }
    int getLatency() const { return la; }
    std::atomic<float> grDb { 0.0f };

    void process (float* L, float* R, int n)
    {
        float worst = 1.0f;
        for (int i = 0; i < n; ++i)
        {
            const float l = L[i], r = R != nullptr ? R[i] : l;
            hist[(size_t) hpos] = { l, r };
            hpos = (hpos + 1) % 12;
            float pk = std::max (std::abs (l), std::abs (r));
            for (int p = 0; p < 4; ++p)
            {
                float al = 0, ar = 0;
                for (int t = 0; t < 12; ++t)
                {
                    const auto& h = hist[(size_t) ((hpos + t) % 12)];
                    al += fir[(size_t) p][(size_t) t] * h[0]; ar += fir[(size_t) p][(size_t) t] * h[1];
                }
                pk = std::max (pk, std::max (std::abs (al), std::abs (ar)));
            }
            const float need = pk > ceiling ? ceiling / pk : 1.0f;
            // sliding minimum of the needed gain over the look-ahead window
            minBuf[(size_t) mpos] = need; mpos = (mpos + 1) % (la + 1);
            float m = 1.0f;
            for (float v : minBuf) m = std::min (m, v);
            // instant attack, smooth release ...
            env = m < env ? m : env + rel * (m - env);
            // ... then a moving average over the look-ahead: the gain glides down in a straight line and
            // is exactly 'm' when the peak comes out of the delay line
            boxSum += env - box[(size_t) bpos]; box[(size_t) bpos] = env; bpos = (bpos + 1) % la;
            if (++resum >= 4096) { resum = 0; boxSum = 0; for (float v : box) boxSum += v; }
            const float out = std::min (1.0f, (float) (boxSum / la));
            const float dL = dl[0].process (l), dR = dl[1].process (r);
            L[i] = std::clamp (dL * out, -ceiling, ceiling);
            if (R != nullptr) R[i] = std::clamp (dR * out, -ceiling, ceiling);
            worst = std::min (worst, out);
        }
        grDb.store (gainToDb (worst));
    }
private:
    double sr = 48000;
    int la = 64, hpos = 0, mpos = 0;
    std::array<DelayLine, 2> dl;
    std::array<std::array<float, 12>, 4> fir {};
    std::array<std::array<float, 2>, 12> hist {};
    std::vector<float> minBuf, box;
    double boxSum = 0;
    int bpos = 0, resum = 0;
    float env = 1, rel = 0.001f, ceiling = 0.891f;
};
} // namespace vs
