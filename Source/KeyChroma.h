#pragma once

// Key detection from the whole spectrum, so it works on a solo voice and on a full mix alike.
//
// Every ~43 ms (8192-point FFT at 44.1/48 kHz) the spectral peaks between 70 Hz and 5 kHz are mapped
// to the 12 pitch classes: a harmonic pitch-class profile (HPCP, E. Gomez, "Tonal description of
// music audio signals", PhD thesis, UPF 2006). Each peak also counts, more weakly, toward the pitch
// classes of f/2, f/3 ... so a note's overtones vote for its fundamental instead of for its fifth and
// third. The tuning (how far the music sits from A = 440 Hz) is estimated from the same peaks, so a
// recording tuned to 432 Hz or a few cents off still lands in the right pitch classes. Each loud
// enough frame is normalized and added to a running profile, which is then matched against key
// profiles (see KeyProfiles).
//
// Real-time safe: everything is allocated in prepare(); a frame costs one FFT and a peak scan.

#include "Fft.h"
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>

namespace vs
{
//==============================================================================
// Major / minor key profiles and the Krumhansl-Schmuckler matching (Pearson correlation with each of
// the 24 rotated profiles). The profiles are Temperley's, from the Kostka-Payne corpus (D. Temperley,
// "Music and Probability", MIT Press 2007). Compared with Krumhansl & Kessler's 1982 probe-tone
// profiles (used up to 1.3.0) they weigh the flat 7th of a minor key far less - with the 1982 profiles
// a C-major melody that leans on D, F and A read as D minor - and, unlike Albrecht & Shanahan's 2013
// profiles, they give a major key's 2nd degree real weight: pop melodies often hover on it. On a real
// lead vocal in C that sits on D 38 % of the time, Krumhansl-Kessler said D minor, Albrecht-Shanahan
// G minor, Temperley C major. VocalTest --diag-key compares the profile sets on melodies, chords and
// songs in all 12 keys; --diag-keyfile reads recordings.
struct KeyProfiles
{
    // returns key 0..11 (C..B), minor flag, and the margin over the runner-up (0 = a tie)
    static void best (const std::array<double, 12>& h, int& key, bool& minor, double& margin,
                      const double* majP = nullptr, const double* minP = nullptr)
    {
        static const double maj[12] = { 0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400 };
        static const double mnr[12] = { 0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330 };
        if (majP == nullptr) majP = maj;
        if (minP == nullptr) minP = mnr;
        double b1 = -2.0, b2 = -2.0; key = 0; minor = false;
        for (int m = 0; m < 2; ++m)
            for (int k = 0; k < 12; ++k)
            {
                const double r = pearson (h, m ? minP : majP, k);
                if (r > b1) { b2 = b1; b1 = r; key = k; minor = m == 1; }
                else if (r > b2) b2 = r;
            }
        margin = b1 - b2;
    }
    static double pearson (const std::array<double, 12>& h, const double* p, int key)
    {
        double mh = 0, mp = 0;
        for (int i = 0; i < 12; ++i) { mh += h[(size_t) i]; mp += p[(i - key + 12) % 12]; }
        mh /= 12; mp /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i)
        {
            const double a = h[(size_t) i] - mh, b = p[(i - key + 12) % 12] - mp;
            sxy += a * b; sxx += a * a; syy += b * b;
        }
        return sxx <= 0 || syy <= 0 ? 0.0 : sxy / std::sqrt (sxx * syy);
    }
};

//==============================================================================
class ChromaAnalyzer
{
public:
    int harmonics = 4;              // peaks also count toward f/2 .. f/harmonics
    double harmonicDecay = 0.6;     // weight of f/h: decay^(h-1)

    void prepare (double fs)
    {
        sr = fs;
        N = 1; while (N < (int) (fs * 0.17)) N <<= 1;          // 8192 at 44.1 / 48 kHz: 5.9 Hz bins
        hop = N / 4;
        mask = N - 1;
        fft.prepare (N);
        ring.assign ((size_t) N, 0.0f);
        frame.assign ((size_t) N, 0.0f);
        X.assign ((size_t) N / 2 + 1, {});
        mag.assign ((size_t) N / 2 + 1, 0.0f);
        win.resize ((size_t) N);
        for (int i = 0; i < N; ++i) win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / N));
        bLo = std::max (2, (int) std::ceil (70.0 * N / fs));
        bHi = std::min (N / 2 - 2, (int) std::floor (5000.0 * N / fs));
        reset();
    }

    void reset()
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        pos = 0; sinceHop = 0; filled = 0;
        chroma.fill (0.0); notes.fill (0.0);
        tuneC = tuneS = 0.0;
        activeFrames = monoFrames = 0.0;
    }

    // from another thread (ANALYZE starts): the audio thread forgets what it heard at its next push
    void requestClear() { clearRequest.store (true); }

    // the detected key: 0..11 (C..B), minor flag, confidence 0..1 (the lead over the runner-up key)
    void detect (int& key, bool& minor, float& confidence) const
    {
        double margin = 0.0;
        const auto c = keyHistogram();                          // (read while the audio thread may add: a benign race)
        KeyProfiles::best (c, key, minor, margin);
        // lead over the runner-up: 0.1 or more is typical when it is right (VocalTest --diag-key) = 100 %
        confidence = activeFrames <= 0.0 ? 0.0f : (float) std::clamp (margin * 10.0, 0.0, 1.0);
    }

    void push (const float* x, int n)
    {
        if (clearRequest.exchange (false)) { chroma.fill (0.0); notes.fill (0.0); tuneC = tuneS = 0.0; activeFrames = monoFrames = 0.0; }
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) pos] = x[i];
            pos = (pos + 1) & mask;
            if (filled < N) ++filled;
            if (++sinceHop >= hop) { sinceHop = 0; if (filled == N) analyzeFrame(); }
        }
    }

    // forget older material gradually (the running "detected" key follows the music)
    void decay (double factor)
    {
        for (auto& c : chroma) c *= factor;
        for (auto& c : notes) c *= factor;
        activeFrames *= factor; monoFrames *= factor; tuneC *= factor; tuneS *= factor;
    }

    double activeSeconds() const { return activeFrames * hop / sr; }
    double tuningCents() const { return (tuneC == 0.0 && tuneS == 0.0) ? 0.0 : 100.0 * std::atan2 (tuneS, tuneC) / (2.0 * 3.14159265358979323846); }

    std::array<double, 12> chroma {};       // sum of normalized frame profiles, C = 0
    std::array<double, 12> notes {};        // fundamentals of the frames that are one voice, C = 0

    // share of the music heard that was a single voice (or instrument)
    double monoShare() const { return activeFrames > 0.0 ? monoFrames / activeFrames : 0.0; }
    // what the key is read from: the sung notes when it was mostly a solo voice, else the whole spectrum
    std::array<double, 12> keyHistogram() const { return monoShare() > kSoloShare ? notes : chroma; }
    static constexpr double kSoloShare = 0.7;      // test material: solo melodies 0.96-0.99, songs 0.13-0.79, chords up to 0.5

private:
    void analyzeFrame()
    {
        double energy = 0.0;
        for (int k = 0; k < N; ++k)
        {
            const float s = ring[(size_t) ((pos + k) & mask)];
            frame[(size_t) k] = s * win[(size_t) k];
            energy += (double) s * s;
        }
        if (energy / N < 1.0e-7) return;                       // below about -70 dBFS: nothing to learn
        fft.forwardReal (frame.data(), X.data());
        float mx = 0.0f;
        for (int b = bLo - 1; b <= bHi + 1; ++b) { mag[(size_t) b] = std::abs (X[(size_t) b]); mx = std::max (mx, mag[(size_t) b]); }
        if (mx <= 0.0f) return;
        const float thr = mx * 0.01f;                          // peaks within 40 dB of the loudest
        const double tune = tuningCents() / 100.0;             // semitones
        std::array<double, 12> c {};
        npk = 0;
        for (int b = bLo; b <= bHi; ++b)
        {
            const float m = mag[(size_t) b];
            if (m < thr || m < mag[(size_t) b - 1] || m <= mag[(size_t) b + 1]) continue;
            // parabolic interpolation on the log magnitude: frequency and height of the peak
            const double a = std::log (mag[(size_t) b - 1] + 1e-12f), bb = std::log (m), cc = std::log (mag[(size_t) b + 1] + 1e-12f);
            const double den = a - 2.0 * bb + cc;
            const double off = den < 0.0 ? std::clamp (0.5 * (a - cc) / den, -0.5, 0.5) : 0.0;
            const double f = (b + off) * sr / N;
            const double amp = std::exp (bb - 0.25 * (a - cc) * off);
            if (npk < kMaxPeaks) { pkF[(size_t) npk] = (float) f; pkA[(size_t) npk] = (float) amp; ++npk; }
            const double st = 12.0 * std::log2 (f / 440.0);
            const double dev = st - std::round (st);
            tuneC += amp * std::cos (2.0 * 3.14159265358979323846 * dev);
            tuneS += amp * std::sin (2.0 * 3.14159265358979323846 * dev);
            double w = amp;
            for (int h = 1; h <= harmonics; ++h, w *= harmonicDecay)
            {
                const double sth = st - 12.0 * std::log2 ((double) h) - tune + 9.0;      // semitones above C
                const double pc = sth - 12.0 * std::floor (sth / 12.0);                   // 0 .. 12
                // cos^2 window 4/3 semitone wide around each pitch class
                const int lo = (int) std::floor (pc);
                for (int q : { lo, lo + 1 })
                {
                    const double d = std::abs (pc - q);
                    if (d >= 2.0 / 3.0) continue;
                    const double cw = std::cos (0.5 * 3.14159265358979323846 * d / (2.0 / 3.0));
                    c[(size_t) (((q % 12) + 12) % 12)] += w * cw * cw;
                }
            }
        }
        const double cmax = *std::max_element (c.begin(), c.end());
        if (cmax <= 0.0) return;
        for (int i = 0; i < 12; ++i) chroma[(size_t) i] += c[(size_t) i] / cmax;
        activeFrames += 1.0;
        countSungNote (tune);
    }

    // One voice, or several? If a single harmonic series explains nearly all the peak energy, the frame
    // is a solo voice (or instrument): its fundamental's pitch class goes into 'notes'. A melody's own
    // notes tell its key better than its spectrum, where every note's overtones also count.
    void countSungNote (double tune)
    {
        if (npk < 2) return;
        double tot = 0.0;
        for (int p = 0; p < npk; ++p) tot += (double) pkA[(size_t) p] * pkA[(size_t) p];
        if (tot <= 0.0) return;
        // candidate fundamentals: the strongest peaks and their 1/2, 1/3, 1/4
        std::array<int, 6> strong {}; int ns = 0;
        for (int s = 0; s < 6; ++s)
        {
            int bi = -1;
            for (int p = 0; p < npk; ++p)
            {
                bool used = false;
                for (int q = 0; q < ns; ++q) used = used || strong[(size_t) q] == p;
                if (! used && (bi < 0 || pkA[(size_t) p] > pkA[(size_t) bi])) bi = p;
            }
            if (bi < 0) break;
            strong[(size_t) ns++] = bi;
        }
        double bestR = 0.0, bestF = 0.0;
        std::array<std::pair<double, double>, 24> cand {}; int nc = 0;
        for (int s = 0; s < ns; ++s)
            for (int d = 1; d <= 4; ++d)
            {
                const double f0 = pkF[(size_t) strong[(size_t) s]] / d;
                if (f0 < 70.0 || f0 > 1100.0) continue;
                double ex = 0.0;
                for (int p = 0; p < npk; ++p)
                {
                    const double h = std::round (pkF[(size_t) p] / f0);
                    if (h >= 1.0 && std::abs (pkF[(size_t) p] - h * f0) < 0.03 * f0) ex += (double) pkA[(size_t) p] * pkA[(size_t) p];
                }
                cand[(size_t) nc++] = { f0, ex / tot };
                bestR = std::max (bestR, ex / tot);
            }
        if (bestR < kMonoFit) return;
        // the highest fundamental that explains about as much (its sub-octaves always explain as much)
        for (int i = 0; i < nc; ++i)
            if (cand[(size_t) i].second >= bestR - 0.03 && cand[(size_t) i].first > bestF) bestF = cand[(size_t) i].first;
        const double st0 = 12.0 * std::log2 (bestF / 440.0) - tune + 9.0;
        const int pc = (((int) std::lround (st0)) % 12 + 12) % 12;
        notes[(size_t) pc] += 1.0;
        monoFrames += 1.0;
    }

    double sr = 48000.0;
    int N = 8192, hop = 2048, mask = 8191, pos = 0, sinceHop = 0, filled = 0, bLo = 2, bHi = 100;
    Fft fft;
    std::vector<float> ring, frame, win, mag;
    std::vector<Fft::cpx> X;
    static constexpr int kMaxPeaks = 160;
    static constexpr double kMonoFit = 0.85;      // share of the peak energy one harmonic series must explain
    std::array<float, kMaxPeaks> pkF {}, pkA {};
    int npk = 0;
    double tuneC = 0.0, tuneS = 0.0, activeFrames = 0.0, monoFrames = 0.0;
    std::atomic<bool> clearRequest { false };
};
} // namespace vs
