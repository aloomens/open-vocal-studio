#pragma once

// One STFT stage that does all the frequency-dependent cleanup, so its latency is paid once:
//  - classic noise reduction (Track mode, and as fallback when the neural engine is missing):
//    minimum-statistics noise estimate (R. Martin 2001) and a decision-directed Wiener gain
//    (Ephraim & Malah 1984) with a floor
//  - de-room: late-reverberation suppression after Lebart, Boucher & Denbigh (2001)
//  - resonance suppression: a 1/3-octave view of the spectrum compared with a 1.5-octave view;
//    narrow regions that stick out are turned down while they ring (soothe-style, but smooth in
//    frequency so the harmonics of the voice are never notched)
//  - smoothing: dynamic control of the 2-5 kHz "harsh" region against the rest of the voice
//  - de-essing: sibilant energy above ~4.5 kHz compared with the voice's long-term level; only the
//    sibilant band is reduced, so the rest of the voice is not dulled
//  - breath control: long, quiet, noise-like events between phrases are turned down
// It also measures the long-term spectrum for the Analyze (auto-EQ) function and publishes a
// gain-reduction curve for the display. sqrt-Hann windows, 75 % overlap.

#include "Base.h"
#include "Fft.h"
#include <atomic>

namespace vs
{
class SpectralStage
{
public:
    struct Params
    {
        float noise = 0.0f;       // classic noise reduction amount 0..1
        float deroom = 0.0f;      // 0..1
        float resonance = 0.0f;   // 0..1
        float smooth = 0.0f;      // 0..1
        float deess = 0.0f;       // 0..1
        float breath = 0.0f;      // 0..1
    };

    static constexpr int kDisplayPoints = 64;
    static constexpr int kEqBands = 9;           // analysis bands = EQ bands (63 Hz .. 12 kHz)
    static constexpr float kEqFreqs[kEqBands] = { 63, 125, 250, 500, 1000, 2000, 4000, 8000, 12500 };

    void prepare (double fs, bool lowLatency)
    {
        sr = fs;
        int n = lowLatency ? 256 : 2048;
        if (fs > 60000.0) n *= 2;
        if (fs > 120000.0) n *= 2;
        N = n; hop = N / 4; nb = N / 2 + 1;
        fft.prepare (N);
        win.resize ((size_t) N);
        for (int i = 0; i < N; ++i) win[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (2.0 * kPiD * i / N));
        // sqrt-Hann analysis x synthesis = Hann; Hann at 75 % overlap sums to 2
        norm = 1.0f / ((float) N * 2.0f);
        inBuf.assign ((size_t) N, 0.0f); ola.assign ((size_t) N, 0.0f); frame.assign ((size_t) N, 0.0f);
        outQ.assign ((size_t) hop, 0.0f);
        X.assign ((size_t) nb, {}); tmp.assign ((size_t) N, 0.0f);
        const size_t B = (size_t) nb;
        ps.assign (B, 0.0f); noiseEst.assign (B, 0.0f); minCur.assign (B, 1e10f); minWin.assign (B * (size_t) kMinSubs, 1e10f);
        gPrev.assign (B, 1.0f); gNr.assign (B, 1.0f); gRev.assign (B, 1.0f); gRes.assign (B, 1.0f); gTotal.assign (B, 1.0f);
        resGr.assign (B, 0.0f); resLong.assign (B, 0.0f); lDb.assign (B, -120.0f); s1.assign (B, 0.0f); s2.assign (B, 0.0f); pre.assign (B + 1, 0.0);
        const double frameSec = hop / fs;
        late = std::max (1, (int) std::lround (0.06 / frameSec));
        psHist.assign (B * (size_t) (late + 1), 0.0f);
        // log-frequency smoothing windows (bins), 1/3 octave and 1.5 octaves
        lo1.resize (B); hi1.resize (B); lo2.resize (B); hi2.resize (B);
        for (int k = 0; k < nb; ++k)
        {
            const double kk = std::max (1, k);
            lo1[(size_t) k] = std::max (1, (int) std::floor (kk * std::pow (2.0, -1.0 / 6.0)));
            hi1[(size_t) k] = std::min (nb - 1, (int) std::ceil (kk * std::pow (2.0, 1.0 / 6.0)));
            lo2[(size_t) k] = std::max (1, (int) std::floor (kk * std::pow (2.0, -0.75)));
            hi2[(size_t) k] = std::min (nb - 1, (int) std::ceil (kk * std::pow (2.0, 0.75)));
        }
        binHz = (float) (fs / N);
        // per-frame time constants
        auto fc = [&] (double sec) { return (float) (1.0 - std::exp (-frameSec / sec)); };
        cResLong = fc (2.0);
        cPs = fc (0.02); cAtt = fc (0.004); cRel = fc (0.08); cDeRel = fc (0.03); cLong = fc (3.0); cBreath = fc (0.04);
        subLen = std::max (1, (int) std::lround (0.4 / frameSec));
        gFloorLen = std::max (1, (int) std::lround (1.0 / frameSec));
        lowLen = std::max (1, (int) std::lround (2.0 / frameSec));
        cRelearn = fc (1.0);
        cGuardAtt = fc (0.008); cGuardRel = fc (0.06);
        latency = N;          // a sample leaves one full window after it came in
        reset();
    }

    void reset()
    {
        std::fill (inBuf.begin(), inBuf.end(), 0.0f);
        std::fill (ola.begin(), ola.end(), 0.0f);
        std::fill (outQ.begin(), outQ.end(), 0.0f);
        std::fill (ps.begin(), ps.end(), 0.0f);
        std::fill (noiseEst.begin(), noiseEst.end(), 1e-9f);
        std::fill (minCur.begin(), minCur.end(), 1e10f);
        std::fill (minWin.begin(), minWin.end(), 1e10f);
        std::fill (gPrev.begin(), gPrev.end(), 1.0f);
        std::fill (resGr.begin(), resGr.end(), 0.0f);
        std::fill (resLong.begin(), resLong.end(), 0.0f);
        std::fill (psHist.begin(), psHist.end(), 0.0f);
        for (auto* g : { &gNr, &gRev, &gRes, &gTotal }) std::fill (g->begin(), g->end(), 1.0f);
        fill = 0; frames = 0; subPos = 0; subIdx = 0; histIdx = 0;
        gFloorWin.fill (1000.0f); gFloorCur = 1000.0f; gFloorSub = 0; gFloorIdx = 0; guardMix = 0.0f; lowFrames = 0;
        vLong = -30.0f; smoothGr = 0.0f; deessGr = 0.0f; breathGr = 0.0f; breathHold = 0.0f; harshLong = 0.0f;
        qPos = 0;
    }

    void setParams (const Params& p) { prm = p; }
    int getLatency() const { return latency; }

    // Analyze (auto-EQ): long-term spectrum of the processed voice in the EQ bands
    void setAnalyzing (bool on) { if (on && ! analyzing) { ltas.fill (0.0); ltasFrames = 0; } analyzing = on; }
    bool isAnalyzing() const { return analyzing; }
    int analyzedFrames() const { return ltasFrames; }
    double analyzedSeconds() const { return ltasFrames * (double) hop / sr; }
    std::array<double, kEqBands> getLtas() const { return ltas; }

    // display: gain reduction (dB, <= 0) at 64 log-spaced points 50 Hz .. 16 kHz
    std::array<std::atomic<float>, kDisplayPoints> displayGr {};
    std::atomic<float> deessDb { 0.0f }, breathDb { 0.0f };

    void process (float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            inBuf[(size_t) (N - hop + fill)] = x[i];
            x[i] = outQ[(size_t) fill];
            if (++fill == hop)
            {
                fill = 0;
                doFrame();
                std::copy (inBuf.begin() + hop, inBuf.end(), inBuf.begin());
            }
        }
    }

private:
    void doFrame()
    {
        for (int i = 0; i < N; ++i) frame[(size_t) i] = inBuf[(size_t) i] * win[(size_t) i];
        fft.forwardReal (frame.data(), X.data());
        ++frames;
        const bool active = prm.noise > 0 || prm.deroom > 0 || prm.resonance > 0 || prm.smooth > 0
                            || prm.deess > 0 || prm.breath > 0 || analyzing;
        const float scale = 4.0f / ((float) N * (float) N);   // power, roughly dBFS for a full-scale sine
        double eTot = 0, eSib = 0, eHarsh = 0, eRef = 0, logSum = 0, linSum = 0; int flatN = 0;
        for (int k = 0; k < nb; ++k)
        {
            const float p = std::norm (X[(size_t) k]) * scale;
            ps[(size_t) k] += cPs * (p - ps[(size_t) k]);
            const float f = k * binHz;
            if (f > 80 && f < 16000) eTot += p;
            if (f > 4500 && f < 13000) eSib += p;
            if (f > 2000 && f < 5000) eHarsh += p;
            if (f > 300 && f < 2000) eRef += p;
            if (f > 500 && f < 8000) { logSum += std::log (p + 1e-14); linSum += p; ++flatN; }
        }
        const float totDb = 10.0f * std::log10 ((float) eTot + 1e-12f);
        const float flat = flatN > 0 ? (float) std::exp (logSum / flatN) / (float) (linSum / flatN + 1e-14) : 0.0f;
        // long-term level of the voice: only loud, non-sibilant frames teach it (an S must not raise it)
        const bool sibilantFrame = eSib > 0.3 * eTot;
        if (! sibilantFrame)
        {
            if (totDb > vLong - 10.0f) { vLong += cLong * (totDb - vLong); lowFrames = 0; }
            // 2 s of tonal (not noise-like) frames far below it: the voice has got quieter for good
            // (a quieter take, the input turned down) - follow it down
            else if (totDb > -70.0f && flat < 0.25f && ++lowFrames > lowLen) vLong += cRelearn * (totDb - vLong);
            if (totDb > vLong + 6.0f) vLong += 0.3f * (totDb - vLong - 6.0f);   // catch up quickly on a louder voice
        }

        frameLoud = totDb > vLong - 12.0f;
        if (active)
        {
            noiseReduction();
            deRoom();
            resonances();
            const float sibDb = 10.0f * std::log10 ((float) eSib + 1e-12f);
            const float sibRatio = (float) (eSib / (eTot + 1e-12));
            // de-ess: sibilance louder than the voice's usual level, and dominating the frame
            float dTarget = 0.0f;
            if (prm.deess > 0.0f && sibRatio > 0.3f)
            {
                const float thr = vLong - 6.0f + 10.0f * (1.0f - prm.deess);
                dTarget = std::min (16.0f * prm.deess + 2.0f, std::max (0.0f, (sibDb - thr) * (0.8f + 0.6f * prm.deess)));
            }
            deessGr += (dTarget > deessGr ? 1.0f : cDeRel) * (dTarget - deessGr);
            // smoothing: 2-5 kHz relative to 300 Hz-2 kHz, above its own long-term balance
            const float harsh = 10.0f * std::log10 ((float) ((eHarsh + 1e-12) / (eRef + 1e-12)));
            float sTarget = 0.0f;
            if (totDb > vLong - 15.0f)
            {
                harshLong += cLong * (harsh - harshLong);
                sTarget = std::clamp ((harsh - harshLong - 1.0f) * 1.2f * prm.smooth + 1.5f * prm.smooth, 0.0f, 6.0f * prm.smooth);
            }
            smoothGr += (sTarget > smoothGr ? cAtt * 4.0f : cRel) * (sTarget - smoothGr);
            // breath: quiet, noisy (flat), not sibilant, and lasting
            const bool breathy = prm.breath > 0 && totDb < vLong - 14.0f && totDb > vLong - 50.0f && flat > 0.25f && sibRatio < 0.4f;
            breathHold = breathy ? std::min (1.0f, breathHold + (float) hop / (float) (0.08 * sr)) : 0.0f;
            const float bTarget = breathHold >= 1.0f ? 14.0f * prm.breath : 0.0f;
            breathGr += cBreath * (bTarget - breathGr);
            deessDb.store (-deessGr); breathDb.store (-breathGr);

            const float sGain = dbToGain (-breathGr);
            for (int k = 0; k < nb; ++k)
            {
                const float f = k * binHz;
                float gdb = 0.0f;
                // de-ess band with a soft edge from 3.5 to 4.5 kHz
                if (deessGr > 0.0f && f > 3500.0f) gdb -= deessGr * std::min (1.0f, (f - 3500.0f) / 1000.0f);
                // smoothing region 1.8-5.5 kHz, raised-cosine shape
                if (smoothGr > 0.0f && f > 1800.0f && f < 5500.0f)
                    gdb -= smoothGr * (float) (0.5 - 0.5 * std::cos (2.0 * kPiD * (std::log2 (f / 1800.0f) / std::log2 (5500.0 / 1800.0))));
                const float g = gNr[(size_t) k] * gRev[(size_t) k] * gRes[(size_t) k] * dbToGain (gdb) * sGain;
                gTotal[(size_t) k] = std::max (g, 0.003f);
            }
            for (int k = 0; k < nb; ++k) X[(size_t) k] *= gTotal[(size_t) k];
            if (analyzing && totDb > vLong - 12.0f) measure();
            publish();
        }
        else if ((frames & 7) == 0)
        {
            for (auto& d : displayGr) d.store (0.0f);
            deessDb.store (0.0f); breathDb.store (0.0f);
        }

        fft.inverseReal (X.data(), tmp.data());
        for (int i = 0; i < N; ++i) ola[(size_t) i] += tmp[(size_t) i] * win[(size_t) i] * norm;
        std::copy (ola.begin(), ola.begin() + hop, outQ.begin());
        std::copy (ola.begin() + hop, ola.end(), ola.begin());
        std::fill (ola.end() - hop, ola.end(), 0.0f);
    }

    void noiseReduction()
    {
        // minimum statistics: minimum of the smoothed power over 4 sub-windows of 0.4 s (a longer window
        // spares long held notes, but after every start leaves the noise in for that long)
        for (int k = 0; k < nb; ++k) minCur[(size_t) k] = std::min (minCur[(size_t) k], ps[(size_t) k]);
        if (++subPos >= subLen)
        {
            subPos = 0;
            for (int k = 0; k < nb; ++k) { minWin[(size_t) (subIdx * nb + k)] = minCur[(size_t) k]; minCur[(size_t) k] = ps[(size_t) k]; }
            subIdx = (subIdx + 1) % kMinSubs;
        }
        // voice guard floor (below): the quietest frame of the last ~6 s, learned from every frame
        const float scale = 4.0f / ((float) N * (float) N);
        double ein = 0, lg = 0, ln = 0; int nf = 0;
        for (int k = 0; k < nb; ++k)
        {
            const float f = k * binHz;
            const float p = std::norm (X[(size_t) k]) * scale;
            if (f > 150 && f < 8000) ein += p;
            if (f > 500 && f < 8000) { lg += std::log (p + 1e-14); ln += p; ++nf; }
        }
        const float inDb = 10.0f * std::log10 ((float) ein + 1e-14f);
        if (inDb > -120.0f) gFloorCur = std::min (gFloorCur, inDb);
        if (++gFloorSub >= gFloorLen)
        {
            gFloorSub = 0;
            gFloorWin[(size_t) gFloorIdx] = gFloorCur; gFloorCur = 1000.0f;
            gFloorIdx = (gFloorIdx + 1) % (int) gFloorWin.size();
        }
        if (prm.noise <= 0.0f) { std::fill (gNr.begin(), gNr.end(), 1.0f); guardMix = 0.0f; return; }
        const float floorG = dbToGain (-30.0f * prm.noise);
        for (int k = 0; k < nb; ++k)
        {
            float m = minCur[(size_t) k];
            for (int s = 0; s < kMinSubs; ++s) m = std::min (m, minWin[(size_t) (s * nb + k)]);
            const float nEst = std::max (1e-14f, 1.6f * m);       // bias compensation
            noiseEst[(size_t) k] = nEst;
            const float p = std::norm (X[(size_t) k]) * scale;
            const float gamma = p / nEst;
            const float xi = 0.98f * gPrev[(size_t) k] * gPrev[(size_t) k] * gamma + 0.02f * std::max (gamma - 1.0f, 0.0f);
            const float g = std::max (floorG, xi / (1.0f + xi));
            gPrev[(size_t) k] = g;
        }
        // smooth across 3 bins against musical noise
        for (int k = 0; k < nb; ++k)
        {
            const float a = gPrev[(size_t) std::max (0, k - 1)], b = gPrev[(size_t) k], c = gPrev[(size_t) std::min (nb - 1, k + 1)];
            gNr[(size_t) k] = 0.25f * a + 0.5f * b + 0.25f * c;
        }

        // Voice guard (as in the neural denoiser): minimum statistics learns anything that lasts
        // longer than its window as noise - a held note too. A frame well above the quietest
        // moments of the last ~6 s and with tonal structure (a sung note) may lose at most 2 dB,
        // a very loud one 3 dB. A constant hum or buzz sets that floor itself, so it still goes.
        double eout = 0;
        for (int k = 0; k < nb; ++k)
        {
            const float f = k * binHz;
            if (f > 150 && f < 8000) eout += (double) gNr[(size_t) k] * gNr[(size_t) k] * std::norm (X[(size_t) k]) * scale;
        }
        const float flat = nf > 0 ? (float) (std::exp (lg / nf) / (ln / nf + 1e-14)) : 1.0f;
        float floorDb = gFloorCur;
        for (float v : gFloorWin) floorDb = std::min (floorDb, v);
        const float snr = floorDb > 500.0f ? 100.0f : inDb - floorDb;
        float allowed = snr >= 35.0f ? 3.0f : (snr <= 18.0f ? 80.0f : 3.0f + (35.0f - snr) / 17.0f * 77.0f);
        if (flat < 0.25f && snr >= 10.0f) allowed = 2.0f;
        const float lossDb = 10.0f * std::log10 ((float) ((ein + 1e-20) / (eout + 1e-20)));
        float want = 0.0f;
        if (lossDb > allowed && ein > 0)
        {
            const double a1 = std::sqrt (eout), a2 = std::sqrt (ein), t = a2 * std::pow (10.0, -allowed / 20.0);
            want = (float) std::clamp ((t - a1) / (a2 - a1 + 1e-20), 0.0, 1.0);
        }
        guardMix += (want > guardMix ? cGuardAtt : cGuardRel) * (want - guardMix);
        if (guardMix > 1e-4f)
            for (int k = 0; k < nb; ++k) gNr[(size_t) k] += guardMix * (1.0f - gNr[(size_t) k]);
    }

    void deRoom()
    {
        // keep a history of the smoothed power, 'late' frames back (~60 ms)
        const size_t B = (size_t) nb;
        std::copy (ps.begin(), ps.end(), psHist.begin() + (ptrdiff_t) ((size_t) histIdx * B));
        const int old = (histIdx + 1) % (late + 1);
        histIdx = old;
        if (prm.deroom <= 0.0f) { std::fill (gRev.begin(), gRev.end(), 1.0f); return; }
        const double rt60 = 0.5;
        const double delta = 3.0 * std::log (10.0) / rt60;
        const float decay = (float) std::exp (-2.0 * delta * late * hop / sr);
        const float beta = 1.6f * prm.deroom;
        const float floorG = dbToGain (-14.0f * prm.deroom);
        for (int k = 0; k < nb; ++k)
        {
            const float pl = decay * psHist[(size_t) old * B + (size_t) k];
            const float p = ps[(size_t) k] + 1e-14f;
            const float g = std::max (floorG, 1.0f - beta * pl / p);
            float& gr = gRev[(size_t) k];
            gr += (g < gr ? 0.6f : 0.25f) * (g - gr);
        }
    }

    void resonances()
    {
        if (prm.resonance <= 0.0f) { std::fill (gRes.begin(), gRes.end(), 1.0f); return; }
        // prefix sums of the spectrum in dB, for the two log-frequency averages
        pre[0] = 0.0;
        for (int k = 0; k < nb; ++k)
        {
            lDb[(size_t) k] = 10.0f * std::log10 (std::norm (X[(size_t) k]) + 1e-12f);
            pre[(size_t) k + 1] = pre[(size_t) k] + lDb[(size_t) k];
        }
        for (int k = 1; k < nb; ++k)
        {
            s1[(size_t) k] = (float) ((pre[(size_t) hi1[(size_t) k] + 1] - pre[(size_t) lo1[(size_t) k]]) / (hi1[(size_t) k] - lo1[(size_t) k] + 1));
            s2[(size_t) k] = (float) ((pre[(size_t) hi2[(size_t) k] + 1] - pre[(size_t) lo2[(size_t) k]]) / (hi2[(size_t) k] - lo2[(size_t) k] + 1));
        }
        const float depth = prm.resonance;
        const float thr = 5.0f - 2.0f * depth;
        for (int k = 0; k < nb; ++k)
        {
            const float f = k * binHz;
            float w = 0.0f;                    // where resonances matter: weight 1 from 1 to 8 kHz
            if (f > 150.0f && f < 14000.0f) w = f < 1000.0f ? 0.35f + 0.65f * (f - 150.0f) / 850.0f : (f > 8000.0f ? 1.0f - (f - 8000.0f) / 6000.0f : 1.0f);
            // the voice's own steady shape (formants, a held note) is part of its sound and the EQ's
            // job: only what rises above its usual shape (slow average, ~2 s) is a resonance to tame
            const float raw = s1[(size_t) k] - s2[(size_t) k];
            float& lt = resLong[(size_t) k];
            if (frameLoud) lt += cResLong * (raw - lt);
            const float excess = raw - std::max (0.0f, lt) - thr;
            const float target = (k == 0 || excess <= 0.0f) ? 0.0f : std::min (10.0f * depth, excess * (0.5f + 0.5f * depth) * w);
            float& gr = resGr[(size_t) k];
            gr += (target > gr ? cAtt * 3.0f : cRel) * (target - gr);
            gRes[(size_t) k] = dbToGain (-gr);
        }
    }

    void measure()
    {
        ++ltasFrames;
        for (int b = 0; b < kEqBands; ++b)
        {
            const double fc = kEqFreqs[b];
            const double lo = b == kEqBands - 1 ? 10000.0 : fc / std::sqrt (2.0), hi = b == kEqBands - 1 ? 16000.0 : fc * std::sqrt (2.0);
            const int k0 = std::max (1, (int) std::ceil (lo / binHz)), k1 = std::min (nb - 1, (int) std::floor (hi / binHz));
            double e = 0;
            for (int k = k0; k <= k1; ++k) e += std::norm (X[(size_t) k]);
            ltas[(size_t) b] += e / std::max (1, k1 - k0 + 1) * (k1 - k0 + 1);   // band power
        }
    }

    void publish()
    {
        if ((frames & 3) != 0) return;
        for (int i = 0; i < kDisplayPoints; ++i)
        {
            const double f = 50.0 * std::pow (16000.0 / 50.0, i / (double) (kDisplayPoints - 1));
            const int k = std::clamp ((int) std::lround (f / binHz), 0, nb - 1);
            displayGr[(size_t) i].store (gainToDb (gTotal[(size_t) k]));
        }
    }

    double sr = 48000.0;
    int N = 2048, hop = 512, nb = 1025, latency = 1536, fill = 0, late = 6, histIdx = 0;
    static constexpr int kMinSubs = 4;
    int subLen = 40, subPos = 0, subIdx = 0;
    std::array<float, 6> gFloorWin {};          // voice guard: quietest frame per second, last 6 s
    float gFloorCur = 1000.0f, guardMix = 0.0f, cGuardAtt = 0.1f, cGuardRel = 0.02f;
    int gFloorLen = 750, gFloorSub = 0, gFloorIdx = 0, lowFrames = 0, lowLen = 1500;
    float cRelearn = 0.001f;
    int64_t frames = 0;
    float norm = 1.0f, binHz = 23.4f;
    Fft fft;
    std::vector<float> win, inBuf, ola, frame, outQ, tmp;
    std::vector<std::complex<float>> X;
    std::vector<float> ps, noiseEst, minCur, minWin, gPrev, gNr, gRev, gRes, gTotal, resGr, resLong, lDb, s1, s2, psHist;
    bool frameLoud = false;
    float cResLong = 0.01f;
    std::vector<double> pre;
    std::vector<int> lo1, hi1, lo2, hi2;
    float cPs = 0.1f, cAtt = 0.5f, cRel = 0.05f, cDeRel = 0.1f, cLong = 0.001f, cBreath = 0.1f;
    float vLong = -30.0f, smoothGr = 0, deessGr = 0, breathGr = 0, breathHold = 0, harshLong = 0;
    int qPos = 0;
    Params prm;
    bool analyzing = false;
    std::array<double, kEqBands> ltas {};
    int ltasFrames = 0;
};
} // namespace vs
