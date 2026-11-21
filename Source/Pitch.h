#pragma once

// Natural pitch correction.
//
// 1. Pitch tracking: YIN (de Cheveigne & Kawahara, JASA 2002) on a ~12 kHz decimated copy, every
//    2.5 ms, refined at the full rate with a parabolic fit (sub-cent accuracy on high notes).
// 2. Note decisions: the note CENTER is a slow (two-pole) average of the sung pitch, so vibrato,
//    scoops and fast slides stay as sung; only the center is pulled toward the nearest allowed
//    note. Fast pitch movement (slides) fades the correction out; a tolerance leaves small
//    deviations alone; strength sets how far it goes. Hysteresis keeps it from flipping notes.
// 3. Pitch shifting: TD-PSOLA (Moulines & Charpentier 1990). Grains two periods long are cut at
//    pitch-synchronous marks (aligned by cross-correlation) and laid down at the new spacing, so
//    the formants - the character of the voice - stay where they are. Grains are placed with
//    windowed-sinc fractional positioning (no jitter). Ratio 1 reproduces the input exactly.
// 4. Key detection: duration-weighted pitch-class histogram matched against the Krumhansl-Kessler
//    major/minor key profiles.

#include "Base.h"
#include <atomic>

namespace vs
{
//==============================================================================
struct Scales
{
    static constexpr int kCount = 10;
    static const char* name (int i)
    {
        static const char* n[kCount] = { "Chromatic", "Major", "Minor", "Harmonic Minor", "Melodic Minor",
                                         "Major Pentatonic", "Minor Pentatonic", "Blues", "Dorian", "Mixolydian" };
        return n[std::clamp (i, 0, kCount - 1)];
    }
    // bit mask of the 12 intervals above the root
    static int mask (int i)
    {
        static const std::array<std::vector<int>, kCount> iv { {
            { 0,1,2,3,4,5,6,7,8,9,10,11 }, { 0,2,4,5,7,9,11 }, { 0,2,3,5,7,8,10 }, { 0,2,3,5,7,8,11 }, { 0,2,3,5,7,9,11 },
            { 0,2,4,7,9 }, { 0,3,5,7,10 }, { 0,3,5,6,7,10 }, { 0,2,3,5,7,9,10 }, { 0,2,4,5,7,9,10 } } };
        int m = 0;
        for (int v : iv[(size_t) std::clamp (i, 0, kCount - 1)]) m |= 1 << v;
        return m;
    }
    static const char* noteName (int pc)
    {
        static const char* n[12] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        return n[((pc % 12) + 12) % 12];
    }
};

//==============================================================================
// Key detection from a pitch-class histogram (Krumhansl & Kessler 1982 profiles).
class KeyDetector
{
public:
    void reset() { hist.fill (0.0); }
    void add (double midi, double weight)
    {
        const int pc = (((int) std::lround (midi)) % 12 + 12) % 12;
        hist[(size_t) pc] += weight;
    }
    void decay (double factor) { for (auto& h : hist) h *= factor; }
    double total() const { double s = 0; for (auto h : hist) s += h; return s; }

    // returns key 0..11, minor flag, confidence 0..1
    void best (int& key, bool& minor, float& confidence) const
    {
        static const double maj[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const double mnr[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
        double b1 = -2, b2 = -2; key = 0; minor = false;
        for (int m = 0; m < 2; ++m)
            for (int k = 0; k < 12; ++k)
            {
                double p[12];
                for (int i = 0; i < 12; ++i) p[i] = (m ? mnr : maj)[(i - k + 12) % 12];
                const double r = corr (p);
                if (r > b1) { b2 = b1; b1 = r; key = k; minor = m == 1; }
                else if (r > b2) b2 = r;
            }
        confidence = total() <= 0 ? 0.0f : (float) std::clamp ((b1 - b2) * 5.0, 0.0, 1.0);
    }

    std::array<double, 12> hist {};
private:
    double corr (const double* p) const
    {
        double mh = 0, mp = 0;
        for (int i = 0; i < 12; ++i) { mh += hist[(size_t) i]; mp += p[i]; }
        mh /= 12; mp /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i)
        {
            const double a = hist[(size_t) i] - mh, b = p[i] - mp;
            sxy += a * b; sxx += a * a; syy += b * b;
        }
        return sxx <= 0 ? 0.0 : sxy / std::sqrt (sxx * syy);
    }
};

//==============================================================================
class PitchCorrector
{
public:
    struct Params
    {
        float strength = 0.8f;     // 0..1
        float speed = 0.35f;       // 0 = very slow (only long-term intonation) .. 1 = fast
        float toleranceCents = 10.0f;
        int key = 0, scale = 0;
        int noteMask = 0xfff;      // allowed pitch classes (absolute, bit 0 = C)
        float refA4 = 440.0f;
    };

    // minHz sets the lowest pitch tracked, and with it the latency (about 3 periods of it).
    void prepare (double fs, int maxBlock, double minHz = 60.0, double maxHz = 1100.0)
    {
        sr = fs;
        dec = std::max (1, (int) std::lround (fs / 12000.0));
        const double fsd = fs / dec;
        hop = dec * std::max (1, (int) std::lround (fs * 0.0025 / dec));
        maxLagD = (int) std::ceil (fsd / minHz);
        minLagD = std::max (2, (int) std::floor (fsd / maxHz));
        winD = maxLagD;                                   // YIN window; frame spans winD + maxLagD
        pMax = fs / minHz;
        maxR = (int) std::ceil (1.25 * pMax);
        // output waits for: grain reach (1.25 P) + two marks ahead (2.3 P) + the YIN window half (1 P)
        latency = (int) std::ceil (4.6 * pMax) + kTaps + hop;
        unvStep = std::max (32, (int) std::lround (fs * 0.005));

        int s = 1; while (s < latency * 4 + maxBlock + 8192) s <<= 1;
        inBuf.assign ((size_t) s, 0.0f); inMask = s - 1;
        anBuf.assign ((size_t) s, 0.0f);
        outAcc.assign ((size_t) s, 0.0f);
        int sd = 1; while (sd < (winD + maxLagD) * 4 + 64) sd <<= 1;
        decBuf.assign ((size_t) sd, 0.0f); decMask = sd - 1;
        diff.assign ((size_t) maxLagD + 2, 0.0);
        marks.assign (kMarkRing, {});
        frames.assign (kFrameRing, {});
        // decimation low-pass: 4th-order Butterworth at 0.42 of the decimated Nyquist
        for (int i = 0; i < 2; ++i) decLp[(size_t) i].setLowPass (fs, std::min (0.42 * fsd, 0.45 * fs * 0.5), i == 0 ? 0.5412 : 1.3066);
        // analysis high-pass: 4th-order Butterworth at 0.7 x the lowest note
        for (int i = 0; i < 2; ++i) anHp[(size_t) i].setHighPass (fs, 0.7 * minHz, i == 0 ? 0.5412 : 1.3066);
        buildSinc();
        reset();
    }

    void reset()
    {
        std::fill (inBuf.begin(), inBuf.end(), 0.0f);
        std::fill (anBuf.begin(), anBuf.end(), 0.0f);
        std::fill (outAcc.begin(), outAcc.end(), 0.0f);
        std::fill (decBuf.begin(), decBuf.end(), 0.0f);
        for (auto& b : decLp) b.reset();
        for (auto& b : anHp) b.reset();
        inPos = 0; decPos = 0; hopCount = 0;
        frames.assign (kFrameRing, {});
        frameCount = 0;
        marks.assign (kMarkRing, {});
        markCount = 1; marks[0] = { 0, (float) unvStep, false };
        markSearch = 0;
        synth = 0.0; lastSpacing = (double) unvStep;
        gridE = gridPull = 0.0; gridDrift = 1.0; gridSeg = -1; gridValid = false;
        outPos = -latency;
        c1 = c2 = cPrev = 0.0; slope = 0.0; corr = 0.0; note = -1000; wasVoiced = false;
        jumpM = jumpTime = goodPeriod = mAcc = 0.0;
        curMidi.store (0.0f); curCorrCents.store (0.0f); curNote.store (-1);
    }

    void setParams (const Params& p) { params = p; }
    void setEnabled (bool e) { enabled = e; }
    int getLatency() const { return latency; }

    KeyDetector keys;            // fed with stable, voiced pitch (read on the message thread: benign race)
    std::atomic<float> curMidi { 0.0f }, curCorrCents { 0.0f };
    std::atomic<int> curNote { -1 };
    int64_t statShifted = 0, statPulled = 0;     // tests: shifted grains, and those drifting onto the grid

    // mono, in place; output = input delayed by getLatency(), pitch-corrected
    void process (float* x, int n)
    {
        int i = 0;
        while (i < n)
        {
            const int toHop = hop - (int) (inPos % hop);
            const int m = std::min (toHop, n - i);
            for (int j = 0; j < m; ++j)
            {
                const float v = x[i + j];
                inBuf[(size_t) ((inPos + j) & inMask)] = v;
                // the period is measured on a high-passed copy (below the lowest note tracked):
                // low-frequency noise drifting across the analysis window would pull it short
                const float hv = anHp[1].process (anHp[0].process (v, 0), 0);
                anBuf[(size_t) ((inPos + j) & inMask)] = hv;
                float d = decLp[1].process (decLp[0].process (hv, 0), 0);
                if (((inPos + j) % dec) == 0) { decBuf[(size_t) (decPos & decMask)] = d; ++decPos; }
            }
            inPos += m;
            if (inPos % hop == 0) analyze();
            i += m;
        }
        makeMarks();
        synthesize();
        // output n samples ending at inPos - latency
        for (int j = 0; j < n; ++j)
        {
            const int64_t t = inPos - n + j - latency;
            if (t < 0) { x[j] = 0.0f; continue; }
            float& a = outAcc[(size_t) (t & inMask)];
            x[j] = a;
            a = 0.0f;
        }
    }

private:
    static constexpr int kTaps = 16, kPhases = 512, kMarkRing = 4096, kFrameRing = 4096;

    struct Mark { int64_t pos = 0; float period = 0; bool voiced = false; int64_t seg = 0; };   // seg: first mark of its voiced run
    struct Frame { double center = 0; float period = 0; float ratio = 1; bool voiced = false; };

    //---------------------------------------------------------------- analysis (every hop)
    void analyze()
    {
        const int span = winD + maxLagD;
        Frame fr;
        // center of the analysis span, at the full rate (decimation filter delay ~ a couple of samples ignored)
        fr.center = (double) inPos - 0.5 * span * dec;
        bool voiced = false; double periodFull = 0.0;
        if (decPos >= span)
        {
            const int64_t s0 = decPos - span;
            auto X = [&] (int k) { return (double) decBuf[(size_t) ((s0 + k) & decMask)]; };
            double energy = 0.0;
            for (int k = 0; k < span; ++k) energy += X (k) * X (k);
            const double rmsv = std::sqrt (energy / span);
            if (rmsv > 1.0e-3 * 0.56)    // about -65 dBFS
            {
                // YIN difference function and cumulative mean normalization
                double run = 0.0; int best = -1; double bestV = 1e9;
                diff[0] = 1.0;
                for (int tau = 1; tau <= maxLagD; ++tau)
                {
                    double d = 0.0;
                    for (int k = 0; k < winD; ++k) { const double e = X (k) - X (k + tau); d += e * e; }
                    run += d;
                    diff[(size_t) tau] = run > 0 ? d * tau / run : 1.0;
                }
                const double thr = 0.15;
                for (int tau = minLagD; tau <= maxLagD; ++tau)
                {
                    if (diff[(size_t) tau] < thr)
                    {
                        while (tau + 1 <= maxLagD && diff[(size_t) tau + 1] < diff[(size_t) tau]) ++tau;
                        best = tau; bestV = diff[(size_t) tau]; break;
                    }
                }
                if (best < 0)
                    for (int tau = minLagD; tau <= maxLagD; ++tau)
                        if (diff[(size_t) tau] < bestV) { bestV = diff[(size_t) tau]; best = tau; }
                // a periodicity shorter than the highest sung note, as strong as the one found (a
                // whistle, a test tone, a whine far above the voice): not a voice - otherwise a
                // multiple of its period would be taken for a low note and "corrected"
                // (its dip usually falls between two lags: the depth is interpolated)
                double shortV = 1e9;
                for (int tau = 2; tau < minLagD; ++tau)
                {
                    const double a = diff[(size_t) tau - 1], b = diff[(size_t) tau], c = diff[(size_t) tau + 1];
                    if (b <= a && b <= c)
                    {
                        const double den = a - 2.0 * b + c;
                        const double off = den > 0.0 ? std::clamp (0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
                        shortV = std::min (shortV, b - 0.25 * (a - c) * off);
                    }
                    else shortV = std::min (shortV, b);
                }
                const bool tooHigh = shortV < thr && shortV <= bestV + 0.05;
                if (best > 0 && bestV < 0.3 && ! tooHigh)
                {
                    voiced = true;
                    periodFull = refine (best * dec);
                }
            }
        }
        fr.voiced = voiced && periodFull > 0.0;
        fr.ratio = (float) decide (fr.voiced, periodFull);    // may replace an octave-slip period
        fr.period = (float) periodFull;
        frames[(size_t) (frameCount & (kFrameRing - 1))] = fr;
        ++frameCount;
    }

    // full-rate difference function around the coarse lag, parabolic interpolation
    double refine (int lag0)
    {
        const int W = winD * dec;
        const int64_t end = inPos;
        const int lo = std::max (2, lag0 - dec - 1), hi = lag0 + dec + 1;
        const int64_t s0 = end - W - hi;
        if (s0 < 0) return lag0;
        double bestV = 1e30; int best = lag0;
        std::array<double, 64> dv {};
        const int count = std::min (hi - lo + 1, 64);
        for (int c = 0; c < count; ++c)
        {
            const int tau = lo + c;
            double d = 0.0;
            for (int k = 0; k < W; ++k)
            {
                const double e = (double) anBuf[(size_t) ((s0 + k) & inMask)] - anBuf[(size_t) ((s0 + k + tau) & inMask)];
                d += e * e;
            }
            dv[(size_t) c] = d;
            if (d < bestV) { bestV = d; best = tau; }
        }
        const int c = best - lo;
        if (c <= 0 || c >= count - 1) return best;
        const double a = dv[(size_t) c - 1], b = dv[(size_t) c], d2 = dv[(size_t) c + 1];
        const double den = a - 2 * b + d2;
        const double off = den > 0 ? 0.5 * (a - d2) / den : 0.0;
        return best + std::clamp (off, -0.5, 0.5);
    }

    //---------------------------------------------------------------- note decisions
    double decide (bool voiced, double& period)
    {
        const double dt = (double) hop / sr;
        if (! voiced)
        {
            wasVoiced = false; jumpTime = 0.0;
            corr += (1.0 - std::exp (-dt / 0.015)) * (0.0 - corr);
            curNote.store (-1);
            return std::pow (2.0, corr / 12.0);
        }
        const double f0 = sr / period;
        const double m = 69.0 + 12.0 * std::log2 (f0 / params.refA4);
        // A sudden jump away from the last reading doesn't move anything at once: a tracking slip in
        // noise or breath lasts a frame or two, a real note change (legato) holds - taken over after
        // 30 ms. (Slides move continuously, a little per frame, and are followed as before.)
        if (wasVoiced && voicedTime > 0.02 && std::abs (m - mAcc) > 0.8)
        {
            if (jumpTime > 0.0 && std::abs (m - jumpM) < 0.5) jumpTime += dt;
            else { jumpM = m; jumpTime = dt; }
            if (jumpTime < 0.03)
            {
                // an octave-type slip (a multiple or fraction of the period) would also misplace
                // the grain marks: keep the last good period for them
                for (double h : { 2.0, 3.0, 4.0, 5.0, 0.5 })
                    if (std::abs (m - (mAcc - 12.0 * std::log2 (h))) < 0.3) { period = goodPeriod; break; }
                return std::pow (2.0, corr / 12.0);
            }
            wasVoiced = false;                       // a new note: start over below
        }
        jumpTime = 0.0;
        mAcc = m;
        goodPeriod = period;
        curMidi.store ((float) m);
        if (! wasVoiced) { c1 = c2 = cPrev = m; slope = 0.0; note = -1000; voicedTime = 0.0; }
        wasVoiced = true;
        voicedTime += dt;
        // note center: two-pole average; speed 0..1 -> 300 ms .. 25 ms
        const double tau = 0.300 * std::pow (0.025 / 0.300, (double) params.speed);
        const double k = 1.0 - std::exp (-dt / tau);
        c1 += k * (m - c1);
        c2 += k * (c1 - c2);
        const double c = c2;
        const double sl = std::abs (c - cPrev) / dt;  cPrev = c;
        slope += (1.0 - std::exp (-dt / 0.03)) * (sl - slope);
        // slides: above ~4 semitones/s the correction fades out, gone at ~12
        const double tf = std::clamp (1.0 - (slope - 4.0) / 8.0, 0.0, 1.0);

        // nearest allowed note, with hysteresis
        const int allowed = allowedMask();
        const int cand = nearestAllowed (c, allowed);
        if (note < -100 || ! isAllowed (note, allowed) || std::abs (c - cand) < std::abs (c - note) - 0.12)
            note = cand;
        curNote.store (note);
        double dev = note - c;
        const double tol = params.toleranceCents / 100.0;
        dev = dev > 0 ? std::max (0.0, dev - tol) : std::min (0.0, dev + tol);
        // the first 60 ms of a note are left alone: scoops into notes are natural, and the slope
        // estimate needs that long to tell a held note from a glide
        const double onset = std::clamp ((voicedTime - 0.06) / 0.04, 0.0, 1.0);
        double target = enabled ? params.strength * tf * onset * dev : 0.0;
        target = std::clamp (target, -2.0, 2.0);
        corr += (1.0 - std::exp (-dt / 0.012)) * (target - corr);
        if (std::abs (corr) < 1e-7 && target == 0.0) corr = 0.0;
        curCorrCents.store ((float) (corr * 100.0));
        // key histogram: stable sung notes only
        if (slope < 2.0) keys.add (c, dt);
        return std::pow (2.0, corr / 12.0);
    }

    int allowedMask() const
    {
        const int sm = Scales::mask (params.scale);
        int abs = 0;
        for (int i = 0; i < 12; ++i) if (sm & (1 << i)) abs |= 1 << ((i + params.key) % 12);
        abs &= params.noteMask;
        return abs == 0 ? 0xfff : abs;
    }
    static bool isAllowed (int midiNote, int mask) { return (mask >> (((midiNote % 12) + 12) % 12)) & 1; }
    static int nearestAllowed (double m, int mask)
    {
        const int base = (int) std::lround (m);
        for (int d = 0; d <= 6; ++d)
        {
            const int a = base - d, b = base + d;
            const bool okA = isAllowed (a, mask), okB = isAllowed (b, mask);
            if (okA && okB) return std::abs (m - a) <= std::abs (m - b) ? a : b;
            if (okA) return a;
            if (okB) return b;
        }
        return base;
    }

    //---------------------------------------------------------------- frames lookup
    // pitch-track value at time t (linear between frames); false if not known yet
    bool track (double t, Frame& out) const
    {
        if (frameCount < 2) return false;
        const Frame& f0 = frames[0];
        juce::ignoreUnused (f0);
        // frame k center = k*hop + hop - 0.5*span*dec  (frame k analyzed when inPos = (k+1)*hop)
        const double off = (double) hop - 0.5 * (winD + maxLagD) * dec;
        const double fk = (t - off) / hop;
        int64_t k = (int64_t) std::floor (fk);
        if (k >= frameCount - 1) return false;
        if (k < 0) { out = frames[0]; out.voiced = false; out.ratio = 1.0f; return true; }
        if (k < frameCount - kFrameRing + 2) return false;
        const Frame& a = frames[(size_t) (k & (kFrameRing - 1))];
        const Frame& b = frames[(size_t) ((k + 1) & (kFrameRing - 1))];
        const double w = fk - (double) k;
        out.voiced = w < 0.5 ? a.voiced : b.voiced;
        out.ratio = (float) (a.ratio + w * (b.ratio - a.ratio));
        if (a.voiced && b.voiced) out.period = (float) (a.period + w * (b.period - a.period));
        else out.period = w < 0.5 ? a.period : b.period;
        return true;
    }
    double knownUntil() const
    {
        const double off = (double) hop - 0.5 * (winD + maxLagD) * dec;
        return (double) (frameCount - 1) * hop + off;
    }

    //---------------------------------------------------------------- analysis marks
    inline float in (int64_t p) const { return p < 0 || p >= inPos ? 0.0f : inBuf[(size_t) (p & inMask)]; }

    void makeMarks()
    {
        for (;;)
        {
            const Mark last = marks[(size_t) ((markCount - 1) & (kMarkRing - 1))];
            Frame f;
            if (! track ((double) last.pos, f)) return;
            Mark nm;
            if (f.voiced && f.period > 4.0f)
            {
                const double P = f.period;
                const int h = (int) (0.5 * P);
                const int lo = (int) std::floor (0.85 * P), hi = (int) std::ceil (1.15 * P);
                if (last.pos + hi + h + 2 > inPos) return;            // need the audio
                if ((double) (last.pos + hi) > knownUntil()) return;
                int best = (int) std::lround (P); double bestC = -1e30;
                if (last.voiced)
                {
                    for (int c = lo; c <= hi; ++c)
                    {
                        double s = 0.0, e = 0.0;
                        for (int j = -h; j <= h; j += 1)
                        {
                            const double b = in (last.pos + c + j);
                            s += (double) in (last.pos + j) * b; e += b * b;
                        }
                        const double v = e > 0 ? s / std::sqrt (e) : 0.0;
                        if (v > bestC) { bestC = v; best = c; }
                    }
                }
                nm = { last.pos + best, (float) best, true, last.voiced ? last.seg : markCount };
            }
            else
            {
                if ((double) (last.pos + unvStep) > knownUntil()) return;
                nm = { last.pos + unvStep, (float) unvStep, false, markCount };
            }
            marks[(size_t) (markCount & (kMarkRing - 1))] = nm;
            ++markCount;
        }
    }

    // index of the last mark at or before t
    int64_t markAtOrBefore (double t)
    {
        int64_t lo = std::max<int64_t> (0, markCount - kMarkRing + 1);
        if (markSearch < lo) markSearch = lo;
        while (markSearch + 1 < markCount && (double) marks[(size_t) ((markSearch + 1) & (kMarkRing - 1))].pos <= t) ++markSearch;
        while (markSearch > lo && (double) marks[(size_t) (markSearch & (kMarkRing - 1))].pos > t) --markSearch;
        return markSearch;
    }
    const Mark& mk (int64_t i) const { return marks[(size_t) (i & (kMarkRing - 1))]; }

    //---------------------------------------------------------------- synthesis (overlap-add)
    void synthesize()
    {
        const double finalUntil = (double) (inPos - latency);
        while (synth <= finalUntil + maxR)
        {
            const int64_t mi = markAtOrBefore (synth);
            if (mi + 2 >= markCount) break;                          // marks not far enough ahead yet
            const Mark& m0 = mk (mi);
            const Mark& m1 = mk (mi + 1);
            Frame f;
            if (! track (synth, f)) break;
            // nearest analysis mark
            const bool nearFirst = (synth - (double) m0.pos) <= ((double) m1.pos - synth);
            const int64_t ai = nearFirst ? mi : mi + 1;
            const Mark& a = mk (ai);
            const Mark& an = mk (ai + 1);
            if (ai + 1 >= markCount) break;
            const double local = (double) (an.pos - a.pos);         // analysis period here
            double spacing;
            const double offset = synth - (double) a.pos;
            if (! a.voiced || ! f.voiced)
                spacing = (double) an.pos - synth;                   // unvoiced: land on the next mark (resync)
            else if (f.ratio == 1.0f)
            {
                // no correction: follow the marks exactly, and if an earlier correction left the
                // waveform shifted, drift back into line by at most 0.2 % per period (3.5 cents)
                const double back = std::clamp (offset, -0.002 * local, 0.002 * local);
                spacing = local - back;
            }
            else
            {
                // Shifted: the next grain one input period / ratio further on, measured in "mark time"
                // u (marks since this voiced run began, plus the fraction of the way to the next one).
                // Where the shifted grains sit is free in PSOLA, so it would depend on everything that
                // happened before (two copies whose settings differed for a moment would never line up
                // again). Reference grid: grains where u x ratio is a whole number - a function of the
                // input and the ratio only. While the ratio holds steady (the grid isn't moving), drift
                // onto it by at most 0.2 % per period, as the uncorrected case drifts back onto the
                // marks; moving pitch (a sung voice) leaves it as it is. The grid uses the ratio
                // averaged over ~0.3 s, so tiny tracking jitter doesn't move it.
                const double len0 = (double) (m1.pos - m0.pos);
                const double u = (double) (mi - a.seg) + (synth - (double) m0.pos) / len0;
                if (a.seg != gridSeg || ! gridValid) { gridSeg = a.seg; gridDrift = 1.0; gridValid = true; gridRatio = f.ratio; }
                gridRatio += (1.0 - std::exp (-local / (double) f.ratio / (0.3 * sr))) * ((double) f.ratio - gridRatio);
                const double ph = u * gridRatio;
                const double e = ph - std::round (ph);                     // output periods off the grid
                double de = e - (gridE - gridPull);                          // grid movement not caused by us
                de -= std::round (de);
                gridDrift += 0.02 * (std::abs (de) - gridDrift);
                const double pull = gridDrift < 3.0e-4 ? std::clamp (e, -0.002, 0.002) : 0.0;
                gridE = e; gridPull = pull;
                // u -> sample position (marks mi+1 and mi+2 are known; past mi+2 its interval is extended)
                const double k1 = (synth - (double) m0.pos) / len0 + (1.0 - pull) / (double) f.ratio;
                const Mark& m2 = mk (mi + 2);
                const double len1 = (double) (m2.pos - m1.pos);
                const double pos = k1 < 1.0 ? (double) m0.pos + k1 * len0
                                            : (k1 < 2.0 ? (double) m1.pos + (k1 - 1.0) * len1 : (double) m2.pos + (k1 - 2.0) * len1);
                spacing = pos - synth;
                ++statShifted; if (pull != 0.0) ++statPulled;
            }
            if (! (f.ratio != 1.0f && a.voiced && f.voiced)) gridValid = false;
            spacing = std::clamp (spacing, 4.0, (double) maxR);
            const double left = std::clamp (lastSpacing, 4.0, (double) maxR);
            const double right = spacing;
            addGrain (synth, (double) a.pos, left, right);
            lastSpacing = spacing;
            synth += spacing;
        }
    }

    void addGrain (double s, double a, double left, double right)
    {
        const int64_t n0 = (int64_t) std::ceil (s - left + 1e-9);
        const int64_t n1 = (int64_t) std::floor (s + right - 1e-9);
        const double shift = a - s;          // read position = n + shift
        const double frac = shift - std::floor (shift);
        const int64_t ishift = (int64_t) std::floor (shift);
        const bool exact = frac < 1e-9;
        const float* row = exact ? nullptr : &sinc[(size_t) std::min (kPhases - 1, (int) std::lround (frac * kPhases)) * kTaps];
        const bool roundedUp = ! exact && (int) std::lround (frac * kPhases) >= kPhases;
        for (int64_t n = n0; n <= n1; ++n)
        {
            if (n < 0) continue;
            const double j = (double) n - s;
            const double w = j < 0 ? 0.5 + 0.5 * std::cos (kPiD * j / left) : 0.5 + 0.5 * std::cos (kPiD * j / right);
            float v;
            if (exact || roundedUp) v = in (n + ishift + (roundedUp ? 1 : 0));
            else
            {
                const int64_t base = n + ishift - kTaps / 2 + 1;
                float acc = 0.0f;
                for (int t = 0; t < kTaps; ++t) acc += row[t] * in (base + t);
                v = acc;
            }
            outAcc[(size_t) (n & inMask)] += (float) w * v;
        }
    }

    void buildSinc()
    {
        sinc.assign ((size_t) kPhases * kTaps, 0.0f);
        const double beta = 7.0;
        auto i0 = [] (double x) { double s = 1, t = 1; for (int k = 1; k < 40; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        for (int p = 0; p < kPhases; ++p)
        {
            const double frac = (double) p / kPhases;
            double sum = 0;
            for (int t = 0; t < kTaps; ++t)
            {
                const double d = (double) (t - kTaps / 2 + 1) - frac;
                const double x = 0.97 * d;   // slight low-pass so the kernel is clean at Nyquist
                const double sn = std::abs (x) < 1e-12 ? 1.0 : std::sin (kPiD * x) / (kPiD * x);
                const double r = d / (kTaps / 2);
                const double w = std::abs (r) >= 1 ? 0.0 : i0 (beta * std::sqrt (1 - r * r)) / i0 (beta);
                sinc[(size_t) p * kTaps + (size_t) t] = (float) (sn * w);
                sum += sn * w;
            }
            for (int t = 0; t < kTaps; ++t) sinc[(size_t) p * kTaps + (size_t) t] /= (float) sum;
        }
    }

    double sr = 48000.0, pMax = 800.0;
    int dec = 4, hop = 120, maxLagD = 200, minLagD = 10, winD = 200, maxR = 1000, latency = 2500, unvStep = 240;
    std::vector<float> inBuf, anBuf, outAcc, decBuf, sinc;
    std::array<Biquad, 2> anHp;
    int inMask = 0, decMask = 0;
    int64_t inPos = 0, decPos = 0, hopCount = 0, outPos = 0;
    std::vector<double> diff;
    std::array<Biquad, 2> decLp;
    std::vector<Frame> frames; int64_t frameCount = 0;
    std::vector<Mark> marks; int64_t markCount = 0, markSearch = 0;
    double synth = 0.0, lastSpacing = 240.0;
    double gridE = 0.0, gridPull = 0.0, gridDrift = 1.0, gridRatio = 1.0; int64_t gridSeg = -1; bool gridValid = false;
    // decisions
    Params params;
    bool enabled = true, wasVoiced = false;
    double c1 = 0, c2 = 0, cPrev = 0, slope = 0, corr = 0, voicedTime = 0, jumpM = 0, jumpTime = 0, goodPeriod = 0, mAcc = 0;
    int note = -1000;
};
} // namespace vs
