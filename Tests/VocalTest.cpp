// Open Vocal Studio behaviour tests and THE_LAB-style measurements.
//   VocalTest.exe            run all tests (exit code = number of failures)
//   VocalTest.exe --lab F    also write the measurement report (Markdown) to file F
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include "../Source/OfflineRender.h"
#include "TestSignals.h"
#include <cstdio>
#include <chrono>
#include <functional>

using namespace testsig;

// Counts every C++ allocation made while the audio callback runs (checked by testAudioThread)
static std::atomic<bool> gInAudio { false };
static std::atomic<int> gAudioAllocs { 0 };
void* operator new (size_t n) { if (gInAudio.load (std::memory_order_relaxed)) gAudioAllocs.fetch_add (1); if (void* p = std::malloc (n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[] (size_t n) { if (gInAudio.load (std::memory_order_relaxed)) gAudioAllocs.fetch_add (1); if (void* p = std::malloc (n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, size_t) noexcept { std::free (p); }
void operator delete[] (void* p, size_t) noexcept { std::free (p); }

namespace
{
int failures = 0;
juce::String report;

void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}
void note (const juce::String& s) { std::printf ("      %s\n", s.toRawUTF8()); }

void set (VocalStudioAudioProcessor& p, const juce::String& id, float v)
{
    if (auto* prm = p.apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}

// Everything that changes the sound off; the chain is then a delayed copy (plus the 20 Hz rumble filter).
void neutral (VocalStudioAudioProcessor& p)
{
    for (auto [id, v] : std::initializer_list<std::pair<const char*, double>> {
             { IDs::denoise, 0 }, { IDs::deroom, 0 }, { IDs::deplosive, 0 }, { IDs::pitchOn, 0 }, { IDs::resonance, 0 },
             { IDs::smooth, 0 }, { IDs::deess, 0 }, { IDs::breath, 0 }, { IDs::air, 0 }, { IDs::rider, 0 }, { IDs::comp, 0 },
             { IDs::revType, 0 }, { IDs::dlyOn, 0 }, { IDs::doubler, 0 }, { IDs::rumble, 20 }, { IDs::inGain, 0 }, { IDs::outGain, 0 } })
        set (p, id, (float) v);
    for (int b = 0; b < vs::VocalEq::kBands; ++b) set (p, IDs::eq (b), 0);
}

struct Rig
{
    std::unique_ptr<VocalStudioAudioProcessor> p = std::make_unique<VocalStudioAudioProcessor>();
    double fs;
    Rig (double rate, bool track = false, std::function<void (VocalStudioAudioProcessor&)> setup = {}) : fs (rate)
    {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (juce::AudioChannelSet::mono());
        l.outputBuses.add (juce::AudioChannelSet::stereo());
        p->setBusesLayout (l);
        set (*p, IDs::mode, track ? 1.0f : 0.0f);
        if (setup) setup (*p);
        p->setNonRealtime (true);
        p->setRateAndBufferSizeDetails (fs, 512);
        p->prepareToPlay (fs, 512);
    }
    // mono in, stereo out (returns L), fixed block size; 'extra' samples of silence appended
    std::vector<float> run (const std::vector<float>& in, int block = 512, int extra = 0, std::vector<float>* right = nullptr,
                            std::function<void (int64_t)> beforeBlock = {})
    {
        const size_t total = in.size() + (size_t) extra;
        std::vector<float> out (total), outR (total);
        juce::AudioBuffer<float> b (2, block);
        juce::MidiBuffer midi;
        for (size_t s = 0; s < total; s += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, total - s);
            if (beforeBlock) beforeBlock ((int64_t) s);
            b.setSize (2, n, false, false, true);
            for (int i = 0; i < n; ++i) b.setSample (0, i, s + (size_t) i < in.size() ? in[s + (size_t) i] : 0.0f);
            b.clear (1, 0, n);
            p->processBlock (b, midi);
            for (int i = 0; i < n; ++i) { out[s + (size_t) i] = b.getSample (0, i); outR[s + (size_t) i] = b.getSample (1, i); }
        }
        if (right) *right = outR;
        return out;
    }
};

int bestLag (const std::vector<float>& x, const std::vector<float>& y, int from, int to, size_t a, size_t b)
{
    int best = from; double bv = -1e30;
    for (int lag = from; lag <= to; ++lag)
    {
        double c = 0;
        for (size_t i = a; i < b && i + (size_t) lag < y.size(); ++i) c += (double) x[i] * y[i + (size_t) lag];
        if (c > bv) { bv = c; best = lag; }
    }
    return best;
}

std::vector<float> loadSpeech (double fs)
{
    auto f = juce::File::getCurrentWorkingDirectory().getChildFile ("test-output/speech48.wav");
    if (! f.existsAsFile() || std::abs (fs - 48000.0) > 1) return {};
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (f.createInputStream().release(), true));
    if (rd == nullptr) return {};
    juce::AudioBuffer<float> b (1, (int) rd->lengthInSamples);
    rd->read (&b, 0, b.getNumSamples(), 0, true, false);
    std::vector<float> v (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
    for (auto& s : v) s *= 0.5f;
    return v;
}

// the chain always includes the rumble filter: compare against a reference that went through it too
std::vector<float> rumbled (std::vector<float> x, double fs, float hz = 20.0f)
{
    vs::RumbleFilter r; r.prepare (fs); r.setCutoff (hz);
    r.process (x.data(), (int) x.size());
    return x;
}

double truePeakDb (const std::vector<float>& x)
{
    // 8x oversampled peak (windowed sinc, 64 taps per phase)
    double pk = 0;
    for (float v : x) pk = std::max (pk, (double) std::abs (v));
    for (int ph = 1; ph < 8; ++ph)
    {
        const double frac = ph / 8.0;
        for (size_t i = 32; i + 32 < x.size(); ++i)
        {
            double acc = 0;
            for (int t = -31; t <= 32; ++t)
            {
                const double d = t - frac;
                const double w = 0.5 + 0.5 * std::cos (kPi * d / 33.0);
                acc += x[i + (size_t) t] * std::sin (kPi * d) / (kPi * d) * w;
            }
            pk = std::max (pk, std::abs (acc));
        }
    }
    return db (pk);
}

//==============================================================================
void testLatencyAndNull()
{
    std::printf ("\n-- latency (reported vs measured) and neutral null --\n");
    report << "## Latency\n\n| Mode | Rate | Reported | Measured | Null (neutral) |\n|---|---|---|---|---|\n";
    for (bool track : { false, true })
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            Rig r (fs, track, neutral);
            const int L = r.p->getLatencySamples();
            auto v = voice (fs, { { 0.1, 1.5, 60 }, { 1.7, 1.0, 64 } }, 3.0, 25, 2.0);
            auto nz = noise (fs, 3.0, 0.01, 3);
            for (size_t i = 0; i < v.size(); ++i) v[i] += nz[i];
            auto y = r.run (v, 512, L + 64);
            const int lag = bestLag (v, y, std::max (0, L - 400), L + 400, (size_t) (0.3 * fs), (size_t) (2.5 * fs));
            // null in the voice band (both filtered alike: 200 Hz - 15 kHz)
            std::vector<float> a = rumbled (v, fs), b2 (y.begin() + L, y.begin() + L + (long) v.size());
            for (auto* vec : { &a, &b2 })
            {
                vs::Biquad h1, h2, l1, l2;
                h1.setHighPass (fs, 200, 0.5412); h2.setHighPass (fs, 200, 1.3066); l1.setLowPass (fs, 12000, 0.5412); l2.setLowPass (fs, 12000, 1.3066);
                vs::Biquad l3 = l1, l4 = l2;   // 8th-order low-pass: the 96 kHz Mix path is band-limited to 20 kHz by design
                for (auto& s : *vec) s = l4.process (l3.process (l2.process (l1.process (h2.process (h1.process (s, 0), 0), 0), 0), 0), 0);
            }
            double e = 0, s = 0;
            for (size_t i = (size_t) (0.3 * fs); i < v.size(); ++i) { e += (b2[i] - a[i]) * (b2[i] - a[i]); s += (double) a[i] * a[i]; }
            const double nullDb = 10 * std::log10 (e / s + 1e-30);
            const juce::String m = track ? "Track" : "Mix";
            note (m + " @" + juce::String (fs, 0) + ": reported " + juce::String (L) + " (" + juce::String (1000.0 * L / fs, 1) + " ms), measured " + juce::String (lag) + ", null " + juce::String (nullDb, 1) + " dB");
            report << "| " << m << " | " << juce::String (fs / 1000.0, 1) << " kHz | " << L << " (" << juce::String (1000.0 * L / fs, 1) << " ms) | " << lag << " | " << juce::String (nullDb, 1) << " dB |\n";
            check (lag == L, "latency exact (" + m + " @" + juce::String (fs, 0) + ")");
            check (nullDb < -40.0, "neutral settings pass the voice unchanged (" + m + " @" + juce::String (fs, 0) + ")");
            if (track) check (1000.0 * L / fs < 15.0, "Track mode under 15 ms with pitch correction off");
            if (fs == 48000.0 && ! track)
            {
                double pk = 0; for (float s2 : v) pk = std::max (pk, (double) std::abs (s2));
                note ("(test voice peaks at " + juce::String (db (pk), 1) + " dBFS)");
            }
        }
    report << "\nNulls are measured from 200 Hz to 12 kHz against the input sent through the same 20 Hz rumble filter. "
              "In Mix mode at 44.1 and 96 kHz the neural path runs at 48 kHz, so the audio is resampled (passband to 20 kHz, "
              "about -100 dB stopband); that, not the processing, sets those two nulls.\n\n";
}

void testBlockSizeIndependence()
{
    std::printf ("\n-- block-size independence (default settings, Mix mode) --\n");
    const double fs = 48000.0;
    auto v = voice (fs, { { 0.1, 1.2, 57.3 }, { 1.4, 1.0, 62.6 } }, 2.6, 25, 2.0);
    auto nz = noise (fs, 2.6, 0.01, 8);
    for (size_t i = 0; i < v.size(); ++i) v[i] += nz[i];
    std::vector<std::vector<float>> outs;
    for (int bs : { 512, 64, 1001 })
    {
        Rig r (fs);
        outs.push_back (r.run (v, bs, 48000));
    }
    double worst = 0;
    for (size_t k = 1; k < outs.size(); ++k)
        for (size_t i = 0; i < outs[0].size(); ++i) worst = std::max (worst, (double) std::abs (outs[k][i] - outs[0][i]));
    note ("largest difference between block sizes 512 / 64 / 1001: " + juce::String (db (worst), 1) + " dBFS");
    check (worst < 1e-5, "same output for any host block size");

    // the same with Consonant and De-plosive busy (K bursts and low thumps): their filters glide
    // while they work, and must do so at the same moments whatever the buffer size
    {
        auto w = v;
        Rng rng (9); vs::Biquad hp, lp; hp.setHighPass (fs, 1500.0, 0.7071); lp.setLowPass (fs, 7000.0, 0.7071);
        for (double t0 : { 0.7, 1.9 })
            for (int i = 0; i < (int) (0.02 * fs); ++i) w[(size_t) (t0 * fs) + (size_t) i] += 0.4f * lp.process (hp.process (rng.gauss(), 0), 0) * (float) std::exp (-i / (0.008 * fs));
        for (int i = 0; i < (int) (0.03 * fs); ++i) w[(size_t) (1.3 * fs) + (size_t) i] += 0.8f * (float) std::sin (kPi * i / (0.03 * fs));
        std::vector<std::vector<float>> o2;
        for (int track = 0; track < 2; ++track)
        {
            o2.clear();
            for (int bs : { 512, 64, 1001 })
            {
                Rig r (fs, track == 1, [] (VocalStudioAudioProcessor& p) { set (p, IDs::consonant, 100); set (p, IDs::deplosive, 100); });
                o2.push_back (r.run (w, bs, 48000));
            }
            double worst2 = 0;
            for (size_t k = 1; k < o2.size(); ++k)
                for (size_t i = 0; i < o2[0].size(); ++i) worst2 = std::max (worst2, (double) std::abs (o2[k][i] - o2[0][i]));
            const juce::String mode = track ? "Track" : "Mix";
            note ("with Consonant and De-plosive at work (" + mode + "): largest difference " + juce::String (db (worst2), 1) + " dBFS");
            check (worst2 < 1e-7, "same output for any host block size with Consonant and De-plosive at work (" + mode + " mode)");
        }
    }
}

void testSilenceAndExtremes()
{
    std::printf ("\n-- silence, extremes, true peak --\n");
    const double fs = 48000.0;
    {
        Rig r (fs);
        std::vector<float> z ((size_t) (fs * 3), 0.0f);
        auto y = r.run (z);
        double pk = 0; for (float s : y) pk = std::max (pk, (double) std::abs (s));
        note ("silence in -> peak out " + juce::String (db (pk), 1) + " dBFS");
        check (db (pk) < -120.0, "silence in, silence out");
        report << "## Noise floor\n\nSilence in, default settings: output peak " << juce::String (db (pk), 1) << " dBFS.\n\n";
    }
    {
        // hot input (+12 dB over full scale peaks), everything at maximum
        Rig r (fs, false, [] (VocalStudioAudioProcessor& p)
        {
            for (auto* prm : p.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
                    if (rp->getParameterID() != IDs::mode && rp->getParameterID() != IDs::compare && rp->getParameterID() != IDs::outGain)
                        rp->setValueNotifyingHost (1.0f);
            set (p, IDs::outGain, 12.0f);
        });
        auto v = voice (fs, { { 0.1, 2.0, 55 }, { 2.2, 1.5, 67 } }, 4.0, 30, 30.0);
        auto y = r.run (v, 512, 48000);
        bool finite = true; for (float s : y) finite &= std::isfinite (s);
        const double tp = truePeakDb (std::vector<float> (y.begin() + (long) (fs * 0.5), y.end()));
        note ("extreme settings, hot input: true peak " + juce::String (tp, 2) + " dBTP");
        check (finite, "no NaN/Inf with every control at maximum and a hot input");
        check (tp < -0.5, "true-peak limiter holds the output below -0.5 dBTP (ceiling -1)");
        report << "## True peak\n\nEvery control at maximum, input peaks far over 0 dBFS, Output +12 dB: true peak " << juce::String (tp, 2) << " dBTP (ceiling -1.0).\n\n";
    }
}

void testClickScan()
{
    std::printf ("\n-- click scan: every continuous parameter jumped min -> max -> min --\n");
    const double fs = 48000.0;
    auto v = voice (fs, { { 0.0, 3.0, 60 } }, 3.0, 20, 2.0);
    int clicks = 0;
    juce::String worstName; double worstRatio = 0;
    VocalStudioAudioProcessor probe;
    for (auto* prm : probe.getParameters())
    {
        auto* rp = dynamic_cast<juce::AudioParameterFloat*> (prm);
        if (rp == nullptr) continue;
        const juce::String id = rp->getParameterID();
        if (id == IDs::denoise) continue;   // checked separately (spectral blend)
        Rig r (fs, true);   // Track mode: fast to run, same controls
        auto spikes = [&] (const std::vector<float>& y)
        {
            // largest 3rd difference relative to the typical 3rd difference around it (+-10 ms):
            // a click stands out from its surroundings (a control that only changes the level,
            // like Input, must not count as one because half the run is louder)
            const size_t W = (size_t) (0.01 * fs);
            std::vector<double> d3 (y.size(), 0.0), pre (y.size() + 1, 0.0);
            for (size_t i = 0; i + 3 < y.size(); ++i) d3[i] = std::abs (y[i + 3] - 3.0 * y[i + 2] + 3.0 * y[i + 1] - y[i]);
            for (size_t i = 0; i < y.size(); ++i) pre[i + 1] = pre[i] + d3[i];
            double worst = 0;
            for (size_t i = (size_t) (0.5 * fs); i + 3 < y.size() - 4800; ++i)
            {
                const double local = (pre[i + W] - pre[i - W]) / (double) (2 * W);
                worst = std::max (worst, d3[i] / (local + 1e-9));
            }
            return worst;
        };
        auto base = r.run (v);
        const double b0 = spikes (base);
        if (worstName.isEmpty())
        {
            // the detector itself: what an unsmoothed control does - an instant 6 dB gain step - must show
            auto planted = base;
            size_t at = (size_t) (1.5 * fs);
            while (at < planted.size() && std::abs (planted[at]) < 0.5f * rms (base, (size_t) (1.4 * fs), (size_t) (1.6 * fs))) ++at;  // not at a zero crossing
            for (size_t i = at; i < planted.size(); ++i) planted[i] *= 0.5f;
            const double pr = spikes (planted) / std::max (b0, 1e-9);
            note ("planted unsmoothed -6 dB gain step: x" + juce::String (pr, 1));
            check (pr > 3.0, "the click detector finds a planted click");
            worstName = "-";
        }
        Rig r2 (fs, true);
        set (*r2.p, id, rp->range.start);
        auto y = r2.run (v, 512, 0, nullptr, [&] (int64_t s)
        {
            if (s == (int64_t) (1.0 * fs) / 512 * 512) set (*r2.p, id, rp->range.end);
            if (s == (int64_t) (2.0 * fs) / 512 * 512) set (*r2.p, id, rp->range.start);
        });
        const double b1 = spikes (y);
        const double ratio = b1 / std::max (b0, 1e-9);
        if (ratio > worstRatio) { worstRatio = ratio; worstName = id; }
        if (ratio > 3.0) { ++clicks; note ("possible click: " + id + " (x" + juce::String (ratio, 1) + ")"); }
    }
    note ("worst: " + worstName + " x" + juce::String (worstRatio, 2) + " of its baseline");
    check (clicks == 0, "no clicks when any control jumps from one end to the other");
    report << "## Click scan\n\nEvery continuous control jumped from minimum to maximum and back on a sung note: worst spike " << juce::String (worstRatio, 2) << "x the note's own baseline (" << worstName << ").\n\n";
}

void testPitch()
{
    std::printf ("\n-- pitch correction through the whole plugin --\n");
    const double fs = 48000.0;
    auto v = voice (fs, { { 0.2, 1.6, 57.35 }, { 2.0, 1.6, 61.7 } }, 4.0, 25, 2.0);
    Rig r (fs, false, [] (VocalStudioAudioProcessor& p)
    {
        neutral (p);
        set (p, IDs::pitchOn, 1); set (p, IDs::pitchStrength, 100); set (p, IDs::pitchSpeed, 50); set (p, IDs::pitchTolerance, 0);
    });
    const int L = r.p->getLatencySamples();
    auto y = r.run (v, 512, L);
    std::vector<float> out (y.begin() + L, y.begin() + L + (long) v.size());
    std::vector<double> tIn, tOut;
    const auto pin = yinTrack (v, fs, 0.01, tIn), pout = yinTrack (out, fs, 0.01, tOut);
    auto centsAt = [&] (const std::vector<float>& x, double t0, double t1, double target)
    { return &x == &v ? meanCents (pin, tIn, t0, t1, target) : meanCents (pout, tOut, t0, t1, target); };
    const double in1 = centsAt (v, 1.0, 1.6, 57), out1 = centsAt (out, 1.0, 1.6, 57);
    const double in2 = centsAt (v, 2.8, 3.4, 62), out2 = centsAt (out, 2.8, 3.4, 62);
    note ("A3 sung " + juce::String (juce::roundToInt (in1)) + " c -> " + juce::String (juce::roundToInt (out1)) + " c;  D4 sung " + juce::String (juce::roundToInt (in2)) + " c -> " + juce::String (juce::roundToInt (out2)) + " c");
    check (std::abs (out1) < 6 && std::abs (out2) < 6, "off-pitch notes corrected to within 6 cents");
    report << "## Pitch correction\n\nSynthetic sung vowels with vibrato, 35 cents sharp and 30 cents flat, Strength 100 %: "
           << juce::String (juce::roundToInt (in1)) << " c -> " << juce::String (juce::roundToInt (out1)) << " c and " << juce::String (juce::roundToInt (in2)) << " c -> " << juce::String (juce::roundToInt (out2))
           << " c (mean over the note, measured with YIN). With Strength 0 the corrector is bit-transparent (PitchCheck).\n\n";
}

// Consonant: a "K" - vowel, a short closure, a 20 ms burst in 1.5-7 kHz, vowel again
void testConsonant()
{
    std::printf ("\n-- consonant: the click of a K is softened, the vowels are not --\n");
    const double fs = 48000.0;
    auto a = voice (fs, { { 0.0, 0.6, 60 } }, 0.6, 20, 2.0);
    auto b = voice (fs, { { 0.0, 0.62, 60 } }, 0.62, 20, 2.0);
    float vpk = 0; for (float s : a) vpk = std::max (vpk, std::abs (s));
    std::vector<float> x (a);
    x.resize ((size_t) (0.66 * fs), 0.0f);                          // closure
    {
        Rng rng (5); vs::Biquad hp, lp; hp.setHighPass (fs, 1500.0, 0.7071); lp.setLowPass (fs, 7000.0, 0.7071);
        const int n = (int) (0.02 * fs);
        std::vector<float> burst ((size_t) n);
        float bpk = 0;
        for (int i = 0; i < n; ++i) { burst[(size_t) i] = lp.process (hp.process (rng.gauss(), 0), 0) * (float) std::exp (-i / (0.008 * fs)); bpk = std::max (bpk, std::abs (burst[(size_t) i])); }
        for (auto& s : burst) x.push_back (s * vpk / bpk);           // as loud as the vowel's peaks
    }
    x.insert (x.end(), b.begin(), b.end());
    for (int la : { 0, (int) std::lround (0.003 * fs) })             // Track and Mix mode
    {
        auto render = [&] (float cons)
        {
            vs::Deplosive d; d.prepare (fs, la); d.setAmount (0.5f); d.setConsonant (cons);
            std::vector<float> y (x);
            for (size_t s = 0; s < y.size(); s += 512) d.process (&y[s], (int) std::min<size_t> (512, y.size() - s));
            return y;
        };
        auto off = render (0.0f), on = render (1.0f);
        auto win = [&] (const std::vector<float>& y, double t0, double t1) { return db (rms (y, (size_t) (t0 * fs) + (size_t) la, (size_t) (t1 * fs) + (size_t) la)); };
        const double burstCut = win (on, 0.66, 0.68) - win (off, 0.66, 0.68);
        const double vowel1 = win (on, 0.1, 0.55) - win (off, 0.1, 0.55), vowel2 = win (on, 0.75, 1.25) - win (off, 0.75, 1.25);
        const juce::String mode = la > 0 ? "Mix" : "Track";
        note (mode + ": K burst " + juce::String (burstCut, 1) + " dB, vowels " + juce::String (vowel1, 2) + " / " + juce::String (vowel2, 2) + " dB (Consonant 100 % vs 0)");
        check (burstCut < -4.0, "Consonant softens the burst of a K by more than 4 dB (" + mode + " mode)");
        check (std::abs (vowel1) < 0.3 && std::abs (vowel2) < 0.3, "Consonant leaves the vowels within 0.3 dB (" + mode + " mode)");
        if (la == 0) report << "## Consonant\n\nA synthetic K (a 20 ms burst in 1.5-7 kHz after a short closure, between two vowels), Consonant at 100 % against 0 %:\n\n";
        report << "- " << mode << " mode: the burst " << juce::String (burstCut, 1) << " dB, the vowels " << juce::String (vowel1, 2) << " / " << juce::String (vowel2, 2) << " dB.\n";
        if (la > 0) report << "\n";
    }
}

void testDeessAndPlosive()
{
    std::printf ("\n-- de-esser and de-plosive --\n");
    const double fs = 48000.0;
    auto v = voice (fs, { { 0.0, 4.0, 57 } }, 4.0, 20, 2.0);
    // loud sibilant bursts (5-10 kHz noise, 120 ms) and low thumps (60 Hz, 40 ms)
    Rng rng (77);
    vs::Biquad bp1, bp2; bp1.setHighPass (fs, 5000, 0.7); bp2.setLowPass (fs, 10000, 0.7);
    std::vector<float> sib (v.size(), 0.0f), thump (v.size(), 0.0f);
    for (double t : { 1.0, 2.0, 3.0 })
        for (int i = 0; i < (int) (0.12 * fs); ++i)
            sib[(size_t) (t * fs) + (size_t) i] = 1.2f * bp2.process (bp1.process (rng.gauss(), 0), 0) * (float) std::sin (kPi * i / (0.12 * fs));
    for (double t : { 1.5, 2.5 })
        for (int i = 0; i < (int) (0.04 * fs); ++i)
            thump[(size_t) (t * fs) + (size_t) i] = 0.5f * (float) (std::sin (2 * kPi * 60 * i / fs) * std::sin (kPi * i / (0.04 * fs)));
    std::vector<float> in (v.size());
    for (size_t i = 0; i < v.size(); ++i) in[i] = v[i] + sib[i] + thump[i];
    auto runWith = [&] (float de, float dp)
    {
        Rig r (fs, false, [&] (VocalStudioAudioProcessor& p) { neutral (p); set (p, IDs::deess, de); set (p, IDs::deplosive, dp); set (p, IDs::rumble, 20); });
        const int L = r.p->getLatencySamples();
        auto y = r.run (in, 512, L);
        return std::vector<float> (y.begin() + L, y.begin() + L + (long) in.size());
    };
    auto off = runWith (0, 0), on = runWith (70, 80);
    auto bandRms = [&] (const std::vector<float>& x, double t0, double t1, double lo, double hi)
    {
        vs::Biquad a, b; a.setHighPass (fs, lo, 0.7); b.setLowPass (fs, hi, 0.7);
        double e = 0; size_t n = 0;
        for (size_t i = 0; i < x.size(); ++i) { const float s = b.process (a.process (x[i], 0), 0); if (i > t0 * fs && i < t1 * fs) { e += s * s; ++n; } }
        return db (std::sqrt (e / (double) n));
    };
    const double sOff = bandRms (off, 2.02, 2.1, 5000, 10000), sOn = bandRms (on, 2.02, 2.1, 5000, 10000);
    const double vOff = bandRms (off, 0.4, 0.9, 150, 3000), vOn = bandRms (on, 0.4, 0.9, 150, 3000);
    const double pOff = bandRms (off, 1.5, 1.54, 30, 120), pOn = bandRms (on, 1.5, 1.54, 30, 120);
    note ("sibilance " + juce::String (sOff - sOn, 1) + " dB down; voice between esses changed " + juce::String (vOn - vOff, 2) + " dB; thump " + juce::String (pOff - pOn, 1) + " dB down");
    check (sOff - sOn > 6.0, "de-esser takes at least 6 dB off loud sibilance");
    check (std::abs (vOn - vOff) < 1.0, "de-esser leaves the voice between esses alone (< 1 dB)");
    check (pOff - pOn > 6.0, "de-plosive takes at least 6 dB off a low thump");
    report << "## De-esser and de-plosive\n\nDe-ess 70 %: loud 5-10 kHz bursts reduced " << juce::String (sOff - sOn, 1) << " dB, the vowel between them changed "
           << juce::String (vOn - vOff, 2) << " dB. De-plosive 80 %: a 60 Hz thump reduced " << juce::String (pOff - pOn, 1) << " dB.\n\n";
}

void testDenoise()
{
    std::printf ("\n-- denoise (speech from Windows text-to-speech + pink noise) --\n");
    auto clean = loadSpeech (48000.0);
    if (clean.empty()) { note ("test-output/speech48.wav not found - skipped"); return; }
    const double fs = 48000.0;
    clean.resize (std::min (clean.size(), (size_t) (8 * fs)));
    auto nz = noise (fs, clean.size() / fs + 0.1, 0.01, 4321);
    std::vector<float> noisy (clean.size());
    for (size_t i = 0; i < noisy.size(); ++i) noisy[i] = clean[i] + nz[i];
    const auto cleanRef = rumbled (clean, fs);
    report << "## Noise removal\n\nSpeech with pink noise (input SNR measured below), Denoise 100 %, all else neutral:\n\n| Mode | SNR in | SNR out |\n|---|---|---|\n";
    for (bool track : { false, true })
    {
        Rig r (fs, track, [] (VocalStudioAudioProcessor& p) { neutral (p); set (p, IDs::denoise, 100); });
        const int L = r.p->getLatencySamples();
        auto y = r.run (noisy, 512, L);
        std::vector<float> out (y.begin() + L, y.begin() + L + (long) noisy.size());
        auto snr = [&] (const std::vector<float>& x)
        {
            double s = 0, e = 0;
            const auto& ref = (&x == &noisy) ? clean : cleanRef;
            for (size_t i = (size_t) fs; i < clean.size(); ++i) { s += (double) ref[i] * ref[i]; e += (x[i] - ref[i]) * (x[i] - ref[i]); }
            return 10 * std::log10 (s / e);
        };
        const juce::String m = track ? "Track (spectral)" : "Mix (neural)";
        note (m + ": SNR " + juce::String (snr (noisy), 1) + " -> " + juce::String (snr (out), 1) + " dB");
        check (snr (out) > snr (noisy) + (track ? 2.0 : 6.0), m + " denoiser improves SNR");
        report << "| " << m << " | " << juce::String (snr (noisy), 1) << " dB | " << juce::String (snr (out), 1) << " dB |\n";
    }
    report << "\n";
}

void testSustainedNote()
{
    std::printf ("\n-- a long, steady sung note survives the cleanup --\n");
    const double fs = 48000.0;
    // 0.5 s room tone, then a 4 s steady note (synthetic voice, light vibrato)
    auto v = voice (fs, { { 0.5, 4.0, 64 } }, 5.0, 10, 2.0);
    auto rt = noise (fs, 5.0, 0.0003, 11);
    for (size_t i = 0; i < v.size(); ++i) v[i] += rt[i];
    const double inDb = db (rms (v, (size_t) (2.0 * fs), (size_t) (4.0 * fs)));
    auto lossWith = [&] (const juce::String& name, std::function<void (VocalStudioAudioProcessor&)> setup)
    {
        Rig r (fs, false, setup);
        const int L = r.p->getLatencySamples();
        auto y = r.run (v, 512, L);
        const double outDb = db (rms (y, (size_t) (2.0 * fs) + (size_t) L, (size_t) (4.0 * fs) + (size_t) L));
        note (name + ": " + juce::String (outDb - inDb, 1) + " dB");
        return outDb - inDb;
    };
    const double dn = lossWith ("Denoise 100 % only", [] (VocalStudioAudioProcessor& p) { neutral (p); set (p, IDs::denoise, 100); });
    lossWith ("Resonance only", [] (VocalStudioAudioProcessor& p) { neutral (p); set (p, IDs::resonance, 40); });
    lossWith ("Smooth only", [] (VocalStudioAudioProcessor& p) { neutral (p); set (p, IDs::smooth, 30); });
    lossWith ("Compress only", [] (VocalStudioAudioProcessor& p) { neutral (p); set (p, IDs::comp, 40); });
    lossWith ("Rider only", [] (VocalStudioAudioProcessor& p) { neutral (p); set (p, IDs::rider, 4); });
    const double all = lossWith ("Defaults (Natural Polish)", {});
    // Track mode: the classic noise reduction (minimum statistics) would learn the note as noise
    auto lossTrack = [&] (const juce::String& name, std::function<void (VocalStudioAudioProcessor&)> setup)
    {
        Rig r (fs, true, setup);
        const int L = r.p->getLatencySamples();
        auto y = r.run (v, 512, L);
        const double outDb = db (rms (y, (size_t) (2.0 * fs) + (size_t) L, (size_t) (4.0 * fs) + (size_t) L));
        note (name + ": " + juce::String (outDb - inDb, 1) + " dB");
        return outDb - inDb;
    };
    const double tdn = lossTrack ("Track mode, Denoise 100 % only", [] (VocalStudioAudioProcessor& p) { neutral (p); set (p, IDs::denoise, 100); });
    const double tall = lossTrack ("Track mode, defaults", {});
    check (dn > -6.5, "the neural denoiser takes no more than 6 dB off a loud sustained note");
    check (all > -9.0, "the default chain keeps a sustained note within 9 dB");
    check (tdn > -4.0, "Track mode's noise reduction takes no more than 4 dB off a sustained note");
    check (tall > -9.0, "the default chain in Track mode keeps a sustained note within 9 dB");
    report << "## Sustained note\n\nA 4 s steady note after half a second of room tone: Denoise 100 % alone changes it by " << juce::String (dn, 1)
           << " dB (Track mode " << juce::String (tdn, 1) << " dB), the whole default chain by " << juce::String (all, 1)
           << " dB (Track mode " << juce::String (tall, 1) << " dB).\n\n";
}

// Things that must come back: pitch correction steady in noise, the dynamics following a lasting
// level drop, and the pitch corrector's output lining up with an untouched copy again after its
// settings moved and came back.
void testRecovery()
{
    std::printf ("\n-- steady in noise, and back to normal after changes --\n");
    const double fs = 48000.0;
    auto peakHz = [&] (const std::vector<float>& y, double t0)
    {
        const int N = 32768;
        vs::Fft fft; fft.prepare (N);
        std::vector<float> fr ((size_t) N); std::vector<std::complex<float>> X ((size_t) N / 2 + 1);
        for (int i = 0; i < N; ++i) fr[(size_t) i] = y[(size_t) (t0 * fs) + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2 * kPi * i / N));
        fft.forwardReal (fr.data(), X.data());
        int best = 0; double bv = 0;
        for (int k = (int) (900.0 * N / fs); k < (int) (1100.0 * N / fs); ++k) if (std::abs (X[(size_t) k]) > bv) { bv = std::abs (X[(size_t) k]); best = k; }
        const double l = std::log (std::abs (X[(size_t) best - 1]) + 1e-20), c = std::log (bv + 1e-20), r = std::log (std::abs (X[(size_t) best + 1]) + 1e-20);
        return (best + 0.5 * (l - r) / (l - 2 * c + r)) * fs / N;
    };
    auto corrector = [&] (const std::vector<float>& x, std::function<float (int)> tolAt, int64_t* pulled = nullptr)
    {
        vs::PitchCorrector pc; pc.prepare (fs, 512, 100.0);
        vs::PitchCorrector::Params pp; pp.strength = 0.7f; pp.speed = 0.35f;
        pc.setEnabled (true);
        std::vector<float> y (x);
        for (int s = 0; s < (int) y.size(); s += 512)
        {
            pp.toleranceCents = tolAt (s);
            pc.setParams (pp);
            pc.process (&y[(size_t) s], std::min (512, (int) y.size() - s));
        }
        if (pulled) *pulled = pc.statPulled;
        return y;
    };
    // a 999.76 Hz tone (20.9 cents sharp of B5) with pink noise 15 dB down: 70 % of (20.9 - 8) cents off
    {
        const int n = (int) (8 * fs);
        auto x = noise (fs, 8.0, 0.03, 21);
        for (int i = 0; i < n; ++i) x[(size_t) i] += 0.25f * (float) std::sin (2 * kPi * 999.76 * i / fs);
        auto y = corrector (x, [] (int) { return 8.0f; });
        const double want = 999.76 * std::pow (2.0, -0.7 * (20.89 - 8.0) / 1200.0);
        double worst = 0;
        for (double t : { 2.0, 3.5, 5.0, 6.5 }) worst = std::max (worst, std::abs (1200.0 * std::log2 (peakHz (y, t) / want)));
        note ("tone in noise: corrected pitch within " + juce::String (worst, 1) + " cents of the target");
        check (worst < 3.0, "pitch correction holds a tone in noise (15 dB down) within 3 cents of its target");
    }
    // a clean tone; Tolerance at 50 cents (no correction) for 2 s, then back
    {
        const int n = (int) (11 * fs);
        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i) x[(size_t) i] = 0.25f * (float) std::sin (2 * kPi * 999.76 * i / fs);
        auto a = corrector (x, [] (int) { return 8.0f; });
        auto b = corrector (x, [&] (int s) { return s >= (int) (3 * fs) && s < (int) (5 * fs) ? 50.0f : 8.0f; });
        double e = 0, ea = 0;
        for (size_t i = (size_t) (9 * fs); i < (size_t) (10 * fs); ++i) { const double d = (double) b[i] - a[i]; e += d * d; ea += (double) a[i] * a[i]; }
        const double nullDb = 10 * std::log10 ((e + 1e-30) / (ea + 1e-30));
        note ("tolerance 50 cents for 2 s and back: null against an untouched copy 4 s later " + juce::String (nullDb, 1) + " dB");
        check (nullDb < -60.0, "after a pitch setting returns, a steady tone lines up with an untouched copy again");
    }
    // a sung voice (vibrato) never triggers the grid drift
    {
        auto v = voice (fs, { { 0.0, 2.0, 57.3 }, { 2.2, 2.2, 63.75 } }, 4.5, 25.0);
        int64_t pulled = 0;
        corrector (v, [] (int) { return 8.0f; }, &pulled);
        check (pulled == 0, "the grid drift leaves a sung voice (vibrato) alone");
    }
    // tones far above any sung note (30 cents off a note, so a correction would show) pass untouched
    {
        vs::PitchCorrector probe; probe.prepare (fs, 512, 100.0);
        const int L = probe.getLatency();
        double worst = -400;
        for (double hz : { 1500.0, 3000.0, 6000.0, 10000.0 })
        {
            const int n = (int) (4 * fs);
            std::vector<float> x ((size_t) n);
            const double f = hz * std::pow (2.0, 30.0 / 1200.0);
            for (int i = 0; i < n; ++i) x[(size_t) i] = 0.25f * (float) std::sin (2 * kPi * f * i / fs);
            auto y = corrector (x, [] (int) { return 0.0f; });
            double e = 0, ea = 0;
            for (int i = (int) fs; i < n; ++i) { const double d = (double) y[(size_t) i] - x[(size_t) (i - L)]; e += d * d; ea += (double) x[(size_t) i] * x[(size_t) i]; }
            const double nl = 10 * std::log10 ((e + 1e-30) / (ea + 1e-30));
            if (nl > -100.0) note ("  " + juce::String (hz, 0) + " Hz: null " + juce::String (nl, 1) + " dB");
            worst = std::max (worst, nl);
        }
        note ("tones at 1.5, 3, 6 and 10 kHz: worst null against the input " + juce::String (worst, 1) + " dB");
        check (worst < -100.0, "pitch correction leaves tones above the singing range alone");
    }
    // compressor and rider: 6 s loud, then 20 dB quieter for good; vs only the quieter part
    {
        auto loud = noise (fs, 16.0, 0.1, 31);
        std::vector<float> drop (loud), quiet (loud);
        for (size_t i = 0; i < loud.size(); ++i) { quiet[i] *= 0.1f; if (i >= (size_t) (6 * fs)) drop[i] *= 0.1f; }
        vs::VocalCompressor c1, c2; c1.prepare (fs); c2.prepare (fs); c1.setAmount (0.4f); c2.setAmount (0.4f); c1.reset(); c2.reset();
        vs::Rider r1, r2; r1.prepare (fs); r2.prepare (fs); r1.setRange (4); r2.setRange (4);
        std::vector<float> yc1 (drop), yc2 (quiet), yr1 (drop), yr2 (quiet);
        for (size_t s = 0; s < drop.size(); s += 512)
        {
            const int m = (int) std::min<size_t> (512, drop.size() - s);
            c1.process (&yc1[s], m); c2.process (&yc2[s], m); r1.process (&yr1[s], m); r2.process (&yr2[s], m);
        }
        const size_t a0 = (size_t) (14 * fs), a1 = (size_t) (16 * fs);
        const double dc = db (rms (yc1, a0, a1)) - db (rms (yc2, a0, a1)), dr = db (rms (yr1, a0, a1)) - db (rms (yr2, a0, a1));
        note ("after a lasting 20 dB drop: compressor " + juce::String (dc, 2) + " dB, rider " + juce::String (dr, 2) + " dB from a copy that only heard the quiet part");
        check (std::abs (dc) < 1.0 && std::abs (dr) < 1.0, "compressor and rider follow a lasting level drop");
    }
    // the first phrase after room tone (which the denoiser cleans to near silence) must not be
    // squashed by dynamics that learned the cleaned room tone as the voice's level
    {
        auto phrase = voice (fs, { { 0.0, 3.0, 62 } }, 3.0, 25, 6.0);    // about -15 dBFS RMS
        auto room = noise (fs, 8.0, 0.03, 17);                             // -30 dBFS
        std::vector<float> withRoom (room), withSilence (room.size(), 0.0f);
        withRoom.insert (withRoom.end(), phrase.begin(), phrase.end());
        withSilence.insert (withSilence.end(), phrase.begin(), phrase.end());
        auto render = [&] (const std::vector<float>& x)
        {
            Rig r (fs, true, [] (VocalStudioAudioProcessor& p) { set (p, IDs::pitchOn, 0); set (p, IDs::revType, 0); });
            const int L = r.p->getLatencySamples();
            auto y = r.run (x, 512, L);
            return db (rms (y, (size_t) (8.5 * fs) + (size_t) L, (size_t) (10.5 * fs) + (size_t) L));
        };
        const double diffDb = render (withRoom) - render (withSilence);
        note ("first phrase after 8 s of room tone vs after silence (Track mode, Denoise 60 %): " + juce::String (diffDb, 1) + " dB");
        check (diffDb > -3.0, "the first phrase after room tone isn't squashed");
    }
}

//==============================================================================
// THE_LAB's own kinds of signals (steady tone, sweep, noise), at the default settings.
void testLabSignals()
{
    std::printf ("\n-- THE_LAB-style signals at default settings --\n");
    const double fs = 48000.0;
    const int n = (int) (6 * fs);
    // a) steady 999.76 Hz tone at -6 dBFS: level kept, no DC, same twice
    {
        std::vector<float> tone ((size_t) n);
        for (int i = 0; i < n; ++i) tone[(size_t) i] = 0.5f * (float) std::sin (2 * kPi * 999.76 * i / fs);
        auto runOnce = [&] (float denoise)
        {
            Rig r (fs, false, [denoise] (VocalStudioAudioProcessor& p) { set (p, IDs::denoise, denoise); });
            const int L = r.p->getLatencySamples(); auto y = r.run (tone, 512, L);
            return std::vector<float> (y.begin() + L, y.end());
        };
        auto y1 = runOnce (60), y2 = runOnce (60), y0 = runOnce (0);
        double mean = 0; for (int i = (int) (3 * fs); i < (int) (5 * fs); ++i) mean += y1[(size_t) i];
        mean /= 2 * fs;
        const double lv = db (rms (y1, (size_t) (3 * fs), (size_t) (5 * fs))) - db (rms (tone, 0, tone.size()));
        const double lv0 = db (rms (y0, (size_t) (3 * fs), (size_t) (5 * fs))) - db (rms (tone, 0, tone.size()));
        const double dc = db (std::abs (mean)) - db (rms (y1, (size_t) (3 * fs), (size_t) (5 * fs)) + 1e-12);
        double diff = 0; for (size_t i = 0; i < y1.size(); ++i) diff = std::max (diff, (double) std::abs (y1[i] - y2[i]));
        note ("1 kHz tone: level " + juce::String (lv0, 1) + " dB with Denoise 0, " + juce::String (lv, 1) + " dB at the default Denoise 60 % (a constant tone is noise to the denoiser), DC "
              + juce::String (dc, 1) + " dB re tone, run-to-run " + juce::String (db (diff), 1) + " dBFS");
        check (lv0 > -6.0, "with Denoise off, a steady tone keeps its level (within 6 dB)");
        check (dc < -60.0 || lv < -20.0, "no DC offset on a steady tone");
        check (diff == 0.0, "same output on every run");
        report << "## THE_LAB-style signals (default settings)" << juce::newLine << juce::newLine
               << "- Steady 999.76 Hz tone at -6 dBFS: level " << juce::String (lv0, 1) << " dB with Denoise at 0 (pitch correction pulls it toward B5); at the default Denoise 60 % "
               << juce::String (lv, 1) << " dB, because the denoiser removes constant tones (hum, buzz, whine) as noise. DC "
               << juce::String (dc, 1) << " dB re the tone, two runs identical: " << (diff == 0.0 ? "yes" : "no") << "." << juce::newLine;
    }
    // b) polarity on a sine sweep
    {
        std::vector<float> sw ((size_t) n);
        double ph = 0;
        for (int i = 0; i < n; ++i)
        {
            const double f = 20.0 * std::pow (1000.0, (double) i / n);
            ph += 2 * kPi * f / fs;
            sw[(size_t) i] = 0.25f * (float) std::sin (ph);
        }
        for (bool neut : { false, true })
        {
            Rig r (fs, false, neut ? std::function<void (VocalStudioAudioProcessor&)> (neutral) : std::function<void (VocalStudioAudioProcessor&)> {});
            const int L = r.p->getLatencySamples();
            auto y = r.run (sw, 512, L);
            double c = 0, e1 = 0, e2 = 0;
            for (int i = (int) fs; i < n; ++i) { c += (double) sw[(size_t) i] * y[(size_t) (i + L)]; e1 += (double) sw[(size_t) i] * sw[(size_t) i]; e2 += (double) y[(size_t) (i + L)] * y[(size_t) (i + L)]; }
            const double corr = c / std::sqrt (e1 * e2 + 1e-30);
            note (juce::String (neut ? "neutral" : "defaults") + ": sweep correlation with the input " + juce::String (corr, 3));
            check (corr > 0.3, juce::String (neut ? "neutral" : "default") + " settings keep the polarity");
            report << "- Sine sweep 20 Hz - 20 kHz, " << (neut ? "neutral" : "default") << " settings: correlation with the input " << juce::String (corr, 2) << " (positive = polarity kept)." << juce::newLine;
        }
    }
    // c) rumble filter excursion (Track mode, pitch off): the sound comes back afterwards
    {
        const int n2 = (int) (12 * fs);
        auto x = noise (fs, 12.0, 0.03, 21);
        for (int i = 0; i < n2; ++i) x[(size_t) i] += 0.25f * (float) std::sin (2 * kPi * 999.76 * i / fs);
        auto setup = [] (VocalStudioAudioProcessor& p) { set (p, IDs::pitchOn, 0); };
        Rig ref (fs, true, setup);
        auto a = ref.run (x);
        Rig r (fs, true, setup);
        auto b = r.run (x, 512, 0, nullptr, [&] (int64_t s)
        {
            if (s == (int64_t) (1 * fs) / 512 * 512) set (*r.p, IDs::rumble, 20);
            if (s == (int64_t) (2 * fs) / 512 * 512) set (*r.p, IDs::rumble, 300);
            if (s == (int64_t) (3 * fs) / 512 * 512) set (*r.p, IDs::rumble, 80);
        });
        auto levelAt = [&] (double t0, double t1) { return db (rms (b, (size_t) (t0 * fs), (size_t) (t1 * fs))) - db (rms (a, (size_t) (t0 * fs), (size_t) (t1 * fs))); };
        const double lv2 = levelAt (5.0, 6.0), lv7 = levelAt (10.0, 12.0);
        note ("Rumble 80 -> 20 -> 300 -> 80 Hz (Track mode, all adaptive stages on): level " + juce::String (lv2, 2) + " dB off 2 s after it's back, "
              + juce::String (lv7, 2) + " dB off 7-9 s after");
        check (std::abs (lv2) < 3.0 && std::abs (lv7) < 1.5, "Rumble Filter recovers after its extremes (within 3 dB at once, 1.5 dB once the adaptive stages settle)");
        report << "- Rumble Filter 80 -> 20 -> 300 -> 80 Hz on noise + tone (Track mode, defaults): level " << juce::String (lv2, 2) << " dB off 2 s after it returns, "
               << juce::String (lv7, 2) << " dB off 7-9 s after (the denoiser's noise estimate, the rider and the compressor relearn the level)." << juce::newLine;
    }
    // d) pink noise response, 96 vs 48 kHz (third octaves, 100 Hz - 10 kHz)
    {
        auto bands = [] (double rate)
        {
            const int len = (int) (8 * rate);
            auto x = noise (rate, 8.0, 0.05, 31);
            Rig r (rate);
            const int L = r.p->getLatencySamples();
            auto y = r.run (x, 512, L);
            std::vector<double> out;
            for (double fc = 100; fc <= 10000; fc *= std::pow (2.0, 1.0 / 3.0))
            {
                vs::Biquad a, b, c, d; a.setHighPass (rate, fc / 1.12, 0.7); b = a; c.setLowPass (rate, fc * 1.12, 0.7); d = c;
                vs::Biquad a2 = a, b2 = a, c2 = c, d2 = c;
                double ei = 0, eo = 0;
                for (int i = (int) (3 * rate); i < len; ++i)
                {
                    const float vi = d.process (c.process (b.process (a.process (x[(size_t) i], 0), 0), 0), 0);
                    const float vo = d2.process (c2.process (b2.process (a2.process (y[(size_t) (i + L)], 0), 0), 0), 0);
                    ei += vi * vi; eo += vo * vo;
                }
                out.push_back (10 * std::log10 ((eo + 1e-20) / (ei + 1e-20)));
            }
            return out;
        };
        auto r48 = bands (48000.0), r96 = bands (96000.0);
        double worst = 0; int wi = 0;
        for (size_t i = 0; i < r48.size(); ++i) if (std::abs (r48[i] - r96[i]) > worst) { worst = std::abs (r48[i] - r96[i]); wi = (int) i; }
        note ("pink noise, 96 vs 48 kHz: worst band difference " + juce::String (worst, 2) + " dB at " + juce::String ((int) (100 * std::pow (2.0, wi / 3.0))) + " Hz");
        check (worst < 1.0, "same response on noise at 96 kHz as at 48 kHz");
        report << "- Pink noise, third octaves 100 Hz - 10 kHz: 96 kHz vs 48 kHz worst difference " << juce::String (worst, 2) << " dB." << juce::newLine << juce::newLine;
    }
}

// --diag: which stage does what to a steady tone and a sweep (from neutral, one stage on at a time)
void diagnose()
{
    const double fs = 48000.0;
    const int n = (int) (6 * fs);
    std::vector<float> tone ((size_t) n), sw ((size_t) n);
    double ph = 0;
    for (int i = 0; i < n; ++i)
    {
        tone[(size_t) i] = 0.5f * (float) std::sin (2 * kPi * 999.76 * i / fs);
        ph += 2 * kPi * 20.0 * std::pow (1000.0, (double) i / n) / fs;
        sw[(size_t) i] = 0.25f * (float) std::sin (ph);
    }
    std::vector<std::pair<juce::String, std::vector<std::pair<const char*, double>>>> cases {
        { "neutral", {} }, { "denoise 60", { { IDs::denoise, 60 } } }, { "deplosive 50", { { IDs::deplosive, 50 } } },
        { "pitch 70", { { IDs::pitchOn, 1 } } }, { "resonance 40", { { IDs::resonance, 40 } } }, { "smooth 30", { { IDs::smooth, 30 } } },
        { "deess 40", { { IDs::deess, 40 } } }, { "air 1.5", { { IDs::air, 1.5f } } }, { "rider 4", { { IDs::rider, 4 } } },
        { "comp 40", { { IDs::comp, 40 } } }, { "plate 18", { { IDs::revType, 1 } } }, { "rumble 80", { { IDs::rumble, 80 } } } };
    for (auto& [name, vals] : cases)
    {
        auto setup = [&vals] (VocalStudioAudioProcessor& p) { neutral (p); for (auto& [id, v] : vals) set (p, id, (float) v); };
        Rig r1 (fs, false, setup); const int L = r1.p->getLatencySamples();
        auto y = r1.run (tone, 512, L);
        double mean = 0; for (int i = (int) (3 * fs); i < (int) (5 * fs); ++i) mean += y[(size_t) (i + L)];
        mean /= 2 * fs;
        const double r = rms (y, (size_t) (3 * fs) + (size_t) L, (size_t) (5 * fs) + (size_t) L);
        Rig r2 (fs, false, setup);
        auto z = r2.run (sw, 512, L);
        double c = 0, e1 = 0, e2 = 0;
        for (int i = (int) fs; i < n; ++i) { c += (double) sw[(size_t) i] * z[(size_t) (i + L)]; e1 += (double) sw[(size_t) i] * sw[(size_t) i]; e2 += (double) z[(size_t) (i + L)] * z[(size_t) (i + L)]; }
        std::printf ("%-14s tone %6.1f dB  DC %6.1f dB  sweep corr %6.3f\n", name.toRawUTF8(), db (r) - db (0.5 / std::sqrt (2.0)),
                     db (std::abs (mean)) - db (r), c / std::sqrt (e1 * e2 + 1e-30));
    }
}

void diagnoseRumble()
{
    const double fs = 48000.0;
    const int n = (int) (6 * fs);
    auto x = noise (fs, 6.0, 0.03, 21);
    for (int i = 0; i < n; ++i) x[(size_t) i] += 0.25f * (float) std::sin (2 * kPi * 999.76 * i / fs);
    std::vector<std::pair<juce::String, std::vector<std::pair<const char*, double>>>> cases {
        { "neutral", {} }, { "denoise 60", { { IDs::denoise, 60 } } }, { "resonance 40", { { IDs::resonance, 40 } } },
        { "smooth 30", { { IDs::smooth, 30 } } }, { "deess 40", { { IDs::deess, 40 } } }, { "rider 4", { { IDs::rider, 4 } } },
        { "comp 40", { { IDs::comp, 40 } } }, { "plate 18", { { IDs::revType, 1 } } }, { "deplosive 50", { { IDs::deplosive, 50 } } } };
    for (auto& [name, vals] : cases)
    {
        auto setup = [&vals] (VocalStudioAudioProcessor& p) { neutral (p); set (p, IDs::rumble, 80); for (auto& [id, v] : vals) set (p, id, (float) v); };
        Rig ref (fs, true, setup);
        auto a = ref.run (x);
        Rig r (fs, true, setup);
        auto b = r.run (x, 512, 0, nullptr, [&] (int64_t s)
        {
            if (s == (int64_t) (1 * fs) / 512 * 512) set (*r.p, IDs::rumble, 20);
            if (s == (int64_t) (2 * fs) / 512 * 512) set (*r.p, IDs::rumble, 300);
            if (s == (int64_t) (3 * fs) / 512 * 512) set (*r.p, IDs::rumble, 80);
        });
        double e = 0, s = 0;
        for (size_t i = (size_t) (5 * fs); i < (size_t) n; ++i) { e += (a[i] - b[i]) * (a[i] - b[i]); s += (double) a[i] * a[i]; }
        std::printf ("%-14s difference 2 s after %6.1f dB\n", name.toRawUTF8(), 10 * std::log10 (e / s + 1e-30));
    }
}

// A stopped DAW (digital silence) must not change how the plugin treats what comes after it,
// and a constant buzz must be removed as noise, not protected as a "voice".
void testSilenceAndBuzz()
{
    std::printf ("\n-- after silence, and a constant buzz --\n");
    const double fs = 48000.0;
    {
        const int n = (int) (16 * fs);
        auto x = noise (fs, 16.0, 0.1, 5);
        for (int i = 0; i < n; ++i) x[(size_t) i] += 0.25f * (float) std::sin (2 * kPi * 440.0 * i / fs);
        std::vector<float> gap (x);
        for (int i = (int) (6 * fs); i < (int) (8 * fs); ++i) gap[(size_t) i] = 0.0f;
        Rig a (fs); auto ya = a.run (x);
        Rig b (fs); auto yb = b.run (gap);
        double worst = 0;
        for (double t : { 9.0, 10.0, 11.0, 13.0, 15.0 })
            worst = std::max (worst, std::abs (db (rms (yb, (size_t) (t * fs), (size_t) ((t + 0.5) * fs))) - db (rms (ya, (size_t) (t * fs), (size_t) ((t + 0.5) * fs)))));
        note ("2 s of silence in the middle (a stopped DAW): level afterwards differs by at most " + juce::String (worst, 2) + " dB from 1 to 7 s later");
        check (worst < 1.0, "a stretch of silence doesn't change the level that follows (< 1 dB)");
        report << "## After silence" << juce::newLine << juce::newLine << "2 s of digital silence in the middle of noise + tone, default settings: from 1 to 7 s later the level differs by at most "
               << juce::String (worst, 2) << " dB from an uninterrupted run (the denoiser, rider and compressor keep their state through silence)." << juce::newLine << juce::newLine;
    }
    {
        // 100 Hz buzz (harmonics to 5 kHz) at about -30 dBFS, plus faint room noise, for 8 s
        const int n = (int) (8 * fs);
        auto x = noise (fs, 8.0, 0.002, 6);
        for (int h = 1; h * 100 <= 5000; ++h)
            for (int i = 0; i < n; ++i) x[(size_t) i] += (float) (0.03 / h * std::sin (2 * kPi * 100.0 * h * i / fs + h));
        Rig r (fs, false, [] (VocalStudioAudioProcessor& p) { neutral (p); set (p, IDs::denoise, 100); });
        const int L = r.p->getLatencySamples();
        auto y = r.run (x, 512, L);
        const double red = db (rms (x, (size_t) (4 * fs), (size_t) (8 * fs))) - db (rms (y, (size_t) (4 * fs) + (size_t) L, (size_t) (8 * fs) + (size_t) L));
        note ("constant 100 Hz buzz, Denoise 100 %: " + juce::String (red, 1) + " dB removed");
        check (red > 10.0, "a constant buzz is removed (more than 10 dB), not protected as a voice");
        report << "A constant 100 Hz buzz with harmonics to 5 kHz, Denoise 100 %: " << juce::String (red, 1) << " dB removed." << juce::newLine << juce::newLine;
    }
}

void testAudioThread()
{
    std::printf ("\n-- audio thread: no allocation while processing --\n");
    const double fs = 48000.0;
    for (bool track : { false, true })
        for (int preset : { 0, 1, 3 })
        {
            Rig r (fs, track, [preset] (VocalStudioAudioProcessor& p) { p.setCurrentProgram (preset); set (p, IDs::doubler, 30); set (p, IDs::dlyOn, 1); });
            auto v = voice (fs, { { 0.1, 2.0, 57.3 }, { 2.3, 2.0, 62.6 } }, 5.0, 25, 2.0);
            auto nz = noise (fs, 5.0, 0.01, 77);
            for (size_t i = 0; i < v.size(); ++i) v[i] += nz[i];
            juce::AudioBuffer<float> b (2, 512);
            juce::MidiBuffer midi;
            int allocBlocks = 0, blocks = 0;
            for (size_t s = 0; s + 512 <= v.size(); s += 512)
            {
                for (int i = 0; i < 512; ++i) { b.setSample (0, i, v[s + (size_t) i]); b.setSample (1, i, v[s + (size_t) i]); }
                if (s == (size_t) (2.5 * fs) / 512 * 512) { set (*r.p, IDs::comp, 70); set (*r.p, IDs::eq (3), -3); }   // control moves too
                gAudioAllocs = 0; gInAudio = true;
                r.p->processBlock (b, midi);
                gInAudio = false;
                if (s > (size_t) fs) { ++blocks; if (gAudioAllocs.load() > 0) ++allocBlocks; }
            }
            const juce::String name = juce::String (track ? "Track" : "Mix") + ", " + r.p->getProgramName (preset);
            note (name + ": allocations in " + juce::String (allocBlocks) + " of " + juce::String (blocks) + " buffers");
            check (allocBlocks == 0, "no memory allocation in the audio callback (" + name + ")");
            if (! track && preset == 0) report << "## Audio thread" << juce::newLine << juce::newLine;
            report << "- " << name << " (plus doubler and delay, two controls moved midway): allocations in " << allocBlocks << " of " << blocks << " buffers." << juce::newLine;
            if (track && preset == 3) report << juce::newLine;
        }
}

// --diag-rumble2: defaults with one stage off at a time (Track mode): level 7-9 s after the excursion
void diagnoseRumble2()
{
    const double fs = 48000.0;
    const int n = (int) (12 * fs);
    auto x = noise (fs, 12.0, 0.03, 21);
    for (int i = 0; i < n; ++i) x[(size_t) i] += 0.25f * (float) std::sin (2 * kPi * 999.76 * i / fs);
    std::vector<std::pair<const char*, std::pair<const char*, double>>> cases {
        { "defaults", { IDs::outGain, 0 } }, { "- rider", { IDs::rider, 0 } }, { "- comp", { IDs::comp, 0 } },
        { "- denoise", { IDs::denoise, 0 } }, { "- resonance", { IDs::resonance, 0 } }, { "- smooth", { IDs::smooth, 0 } },
        { "- deess", { IDs::deess, 0 } }, { "- reverb", { IDs::revType, 0 } }, { "- deplosive", { IDs::deplosive, 0 } } };
    for (auto& [name, off] : cases)
    {
        auto setup = [&off] (VocalStudioAudioProcessor& p) { set (p, IDs::pitchOn, 0); set (p, off.first, (float) off.second); };
        Rig ref (fs, true, setup); auto a = ref.run (x);
        Rig r (fs, true, setup);
        auto b = r.run (x, 512, 0, nullptr, [&] (int64_t s)
        {
            if (s == (int64_t) (1 * fs) / 512 * 512) set (*r.p, IDs::rumble, 20);
            if (s == (int64_t) (2 * fs) / 512 * 512) set (*r.p, IDs::rumble, 300);
            if (s == (int64_t) (3 * fs) / 512 * 512) set (*r.p, IDs::rumble, 80);
        });
        std::printf ("%-12s level off: 2 s after %5.2f dB, 7-9 s after %5.2f dB\n", name,
                     db (rms (b, (size_t) (5 * fs), (size_t) (6 * fs))) - db (rms (a, (size_t) (5 * fs), (size_t) (6 * fs))),
                     db (rms (b, (size_t) (10 * fs), (size_t) n)) - db (rms (a, (size_t) (10 * fs), (size_t) n)));
    }
}

// --diag-loudness: level change of music-like material (pink noise bursts peaking near -1 dBFS)
// through the default chain with Denoise 0 and pitch off, with one stage off at a time
void diagnoseLoudness()
{
    const double fs = 48000.0;
    const int n = (int) (12 * fs);
    auto x = noise (fs, 12.0, 0.12, 41);
    for (int i = 0; i < n; ++i) x[(size_t) i] *= (float) (0.6 + 0.4 * std::sin (2 * kPi * 1.7 * i / fs));   // some movement
    float pk = 0; for (float v : x) pk = std::max (pk, std::abs (v));
    for (auto& v : x) v *= 0.89f / pk;
    auto kw = [&] (const std::vector<float>& y, size_t from)
    {
        vs::Biquad hp, sh; hp.setHighPass (fs, 60.0, 0.5); sh.setHighShelf (fs, 1500.0, 4.0);
        double e = 0; for (size_t i = from; i < y.size(); ++i) { const float k = sh.process (hp.process (y[i], 0), 0); if (i > from + (size_t) (4 * fs)) e += k * k; }
        return 10 * std::log10 (e / (double) (y.size() - from - (size_t) (4 * fs)));
    };
    const double inL = kw (x, 0);
    std::vector<std::pair<const char*, std::pair<const char*, double>>> cases {
        { "as measured", { IDs::outGain, 0 } }, { "- rumble", { IDs::rumble, 20 } }, { "- compress", { IDs::comp, 0 } },
        { "- rider", { IDs::rider, 0 } }, { "- resonance", { IDs::resonance, 0 } }, { "- smooth", { IDs::smooth, 0 } },
        { "- de-ess", { IDs::deess, 0 } }, { "- air", { IDs::air, 0 } }, { "- reverb", { IDs::revType, 0 } }, { "- de-plosive", { IDs::deplosive, 0 } } };
    for (auto& [name, off] : cases)
    {
        Rig r (fs, false, [&off] (VocalStudioAudioProcessor& p) { set (p, IDs::denoise, 0); set (p, IDs::pitchOn, 0); set (p, off.first, (float) off.second); });
        const int L = r.p->getLatencySamples();
        auto y = r.run (x, 512, L);
        std::printf ("%-13s %6.2f dB (K-weighted, 4-12 s)\n", name, kw (y, (size_t) L) - inL);
    }
}

// --diag-rates: response on noise at 48 vs 96 kHz, per stage, 1/6 octave 30 Hz - 10 kHz
static double gDiagFrom = 4.0, gDiagTo = 10.0;
static bool gDiagPink = false, gDiagPitch = false;
void diagnoseRates()
{
    struct Case { const char* name; std::vector<std::pair<const char*, double>> vals; bool fromNeutral; };
    std::vector<Case> cases {
        { "defaults", {}, false }, { "neutral", {}, true }, { "denoise 60", { { IDs::denoise, 60 } }, true },
        { "pitch 70", { { IDs::pitchOn, 1 } }, true }, { "resonance 40", { { IDs::resonance, 40 } }, true },
        { "smooth 30", { { IDs::smooth, 30 } }, true }, { "deess 40", { { IDs::deess, 40 } }, true },
        { "deplosive 50", { { IDs::deplosive, 50 } }, true }, { "rider+comp", { { IDs::rider, 4 }, { IDs::comp, 40 } }, true },
        { "plate", { { IDs::revType, 1 } }, true } };
    for (double rms0 : gDiagPink ? std::vector<double> { 0.1, 0.01 } : std::vector<double> { 0.25, 0.05 })
        for (auto& cs : cases)
        {
            auto bands = [&] (double rate)
            {
                const int len = (int) (10 * rate);
                std::vector<float> x ((size_t) len);
                Rng rng (99);
                if (gDiagPink) x = noise (rate, 10.0, rms0, 99); else for (auto& v : x) v = (float) (rms0 * rng.gauss());
                Rig r (rate, false, [&] (VocalStudioAudioProcessor& p) { if (cs.fromNeutral) neutral (p); for (auto& [id, v] : cs.vals) set (p, id, (float) v); });
                const int L = r.p->getLatencySamples();
                auto y = r.run (x, 512, L);
                std::vector<std::pair<double, double>> out;
                for (double fc = 30; fc <= 10000; fc *= std::pow (2.0, 1.0 / 6.0))
                {
                    vs::Biquad a, c; a.setHighPass (rate, fc / 1.06, 2.0); c.setLowPass (rate, fc * 1.06, 2.0);
                    vs::Biquad b = a, d = c, a2 = a, b2 = a, c2 = c, d2 = c;
                    double ei = 0, eo = 0;
                    for (int i = 0; i < len; ++i)
                    {
                        const float vi = d.process (c.process (b.process (a.process (x[(size_t) i], 0), 0), 0), 0);
                        const float vo = d2.process (c2.process (b2.process (a2.process (y[(size_t) (i + L)], 0), 0), 0), 0);
                        if (i > (int) (gDiagFrom * rate) && i < (int) (gDiagTo * rate)) { ei += vi * vi; eo += vo * vo; }
                    }
                    out.push_back ({ fc, 10 * std::log10 ((eo + 1e-20) / (ei + 1e-20)) });
                }
                return out;
            };
            auto r48 = bands (48000.0), r96 = bands (96000.0);
            double worst = 0, wf = 0, a48 = 0, a96 = 0;
            for (size_t i = 0; i < r48.size(); ++i)
                if (std::abs (r48[i].second - r96[i].second) > worst) { worst = std::abs (r48[i].second - r96[i].second); wf = r48[i].first; a48 = r48[i].second; a96 = r96[i].second; }
            std::printf ("noise %5.1f dBFS  %-13s worst %5.2f dB at %6.0f Hz (48k %6.1f, 96k %6.1f)\n", db (rms0), cs.name, worst, wf, a48, a96);
        }
}

// --diag-tone: where does pitch correction put a 999.76 Hz tone at each sample rate?
void diagnoseTone()
{
    for (double rate : { 44100.0, 48000.0, 96000.0 })
        for (double lvl : { 0.5, 0.0316 })
        {
            const int n = (int) (6 * rate);
            std::vector<float> x ((size_t) n);
            auto nz = noise (rate, 6.0, lvl * 0.3, 7);
            for (int i = 0; i < n; ++i) x[(size_t) i] = (float) (lvl * std::sin (2 * kPi * 999.76 * i / rate)) + nz[(size_t) i];
            for (bool defaults : { false, true })
            {
                Rig r (rate, false, [defaults] (VocalStudioAudioProcessor& p) { if (! defaults) { neutral (p); if (gDiagPitch) set (p, IDs::pitchOn, 1); } });
                const int L = r.p->getLatencySamples();
                auto y = r.run (x, 512, L);
                std::vector<float> out (y.begin() + L, y.end());
                std::vector<double> tm;
                auto tr = yinTrack (out, rate, 0.02, tm);
                double sum = 0; int cnt = 0;
                for (size_t i = 0; i < tr.size(); ++i) if (tm[i] > 2.0 && tm[i] < 5.5 && tr[i] > 0) { sum += tr[i]; ++cnt; }
                std::printf ("%6.0f Hz rate, tone %5.1f dBFS + noise, %-8s: output pitch %8.2f Hz (%d voiced frames), level %6.1f dB\n", rate, db (lvl / std::sqrt (2.0)),
                             defaults ? "defaults" : "pitch", cnt ? sum / cnt : 0.0, cnt,
                             db (rms (out, (size_t) (2 * rate), (size_t) (5.5 * rate))) - db (rms (x, (size_t) (2 * rate), (size_t) (5.5 * rate))));
            }
        }
}

// --diag-rider: the rider's gain and learned level through a 2 s gap
void diagnoseRider()
{
    const double fs = 48000.0;
    const int n = (int) (16 * fs);
    auto x = noise (fs, 16.0, 0.1, 5);
    for (int i = 0; i < n; ++i) x[(size_t) i] += 0.25f * (float) std::sin (2 * kPi * 440.0 * i / fs);
    std::vector<float> gap (x);
    for (int i = (int) (6 * fs); i < (int) (8 * fs); ++i) gap[(size_t) i] = 0.0f;
    vs::Rider a, b; a.prepare (fs); b.prepare (fs); a.setRange (4); b.setRange (4);
    for (int s = 0; s < n; s += 480)
    {
        a.process (&x[(size_t) s], 480); b.process (&gap[(size_t) s], 480);
        const double t = (s + 480) / fs;
        if (std::abs (t - std::round (t * 2) / 2) < 0.005 && t > 5.4)
            std::printf ("%5.1f s  continuous: gain %5.2f long %6.2f   gap: gain %5.2f long %6.2f\n", t, a.gainDb.load(), a.getLongDb(), b.gainDb.load(), b.getLongDb());
    }
}

// --diag-transport: does a silent stretch (a stopped DAW) change the level afterwards?
void diagnoseTransport()
{
    const double fs = 48000.0;
    const int n = (int) (16 * fs);
    auto x = noise (fs, 16.0, 0.1, 5);
    for (int i = 0; i < n; ++i) x[(size_t) i] += 0.25f * (float) std::sin (2 * kPi * 440.0 * i / fs);
    std::vector<float> gap (x);
    for (int i = (int) (6 * fs); i < (int) (8 * fs); ++i) gap[(size_t) i] = 0.0f;
    struct Case { const char* name; std::vector<std::pair<const char*, double>> vals; bool fromNeutral; };
    std::vector<Case> cases { { "defaults", {}, false }, { "denoise 60", { { IDs::denoise, 60 } }, true },
        { "pitch 70", { { IDs::pitchOn, 1 } }, true }, { "resonance 40", { { IDs::resonance, 40 } }, true },
        { "smooth+deess", { { IDs::smooth, 30 }, { IDs::deess, 40 } }, true }, { "rider 4", { { IDs::rider, 4 } }, true },
        { "comp 40", { { IDs::comp, 40 } }, true }, { "plate", { { IDs::revType, 1 } }, true } };
    for (auto& cs : cases)
    {
        auto setup = [&] (VocalStudioAudioProcessor& p) { if (cs.fromNeutral) neutral (p); for (auto& [id, v] : cs.vals) set (p, id, (float) v); };
        Rig a (fs, false, setup); auto ya = a.run (x);
        Rig b (fs, false, setup); auto yb = b.run (gap);
        std::printf ("%-13s level change after 2 s of silence:", cs.name);
        for (double t : { 8.5, 9.5, 11.0, 13.0, 15.0 })
            std::printf (" %5.1f", db (rms (yb, (size_t) (t * fs), (size_t) ((t + 0.5) * fs))) - db (rms (ya, (size_t) (t * fs), (size_t) ((t + 0.5) * fs))));
        std::printf (" dB (at 8.5 / 9.5 / 11 / 13 / 15 s)\n");
    }
}

// --diag-steps: stepped sine at -12 dBFS (each frequency held 1 s), Track mode, pitch off, reverb off:
// level per step - does the denoiser take held tones for hum once the whole run passes ~6 s?
void diagnoseSteps()
{
    const double fs = 48000.0;
    std::vector<double> freqs;
    for (double f = 100; f < 20000; f *= std::pow (2.0, 0.5)) freqs.push_back (f);
    const int step = (int) fs;
    std::vector<float> x ((size_t) step * freqs.size());
    double ph = 0;
    for (size_t s = 0; s < freqs.size(); ++s)
        for (int i = 0; i < step; ++i) { ph += 2 * kPi * freqs[s] / fs; x[s * (size_t) step + (size_t) i] = 0.251f * (float) std::sin (ph); }
    for (double dn : { 0.0, 60.0 })
    {
        Rig r (fs, true, [dn] (VocalStudioAudioProcessor& p) { set (p, IDs::pitchOn, 0); set (p, IDs::revType, 0); set (p, IDs::denoise, (float) dn);
                                                            set (p, IDs::rider, 0); set (p, IDs::comp, 0); });
        const int L = r.p->getLatencySamples();
        auto y = r.run (x, 512, L);
        std::printf ("denoise %3.0f, rider/comp off:", dn);
        for (size_t s = 0; s < freqs.size(); ++s)
        {
            const size_t a = s * (size_t) step + (size_t) (0.5 * fs), b = (s + 1) * (size_t) step;
            std::printf (" %.0f:%.1f", freqs[s], db (rms (y, a + (size_t) L, b + (size_t) L)) - db (rms (x, a, b)));
        }
        std::printf ("\n");
    }
}

// --diag-entry: first phrase after room tone vs after silence, several levels (Track, Denoise 60)
void diagnoseEntry()
{
    const double fs = 48000.0;
    for (double lvl : { 0.3, 1.0 })
    {
        auto v = voice (fs, { { 0.0, 3.0, 62 } }, 3.0, 25, lvl);
        std::printf ("voice level %.1f: RMS %.1f dBFS (pink room tone 0.01 / 0.03 / 0.1 = %.1f / %.1f / %.1f dBFS)\n", lvl,
                     db (rms (v, (size_t) (0.5 * fs), (size_t) (2.5 * fs))), db (0.01), db (0.03), db (0.1));
    }
    for (double roomRms : { 0.03, 0.06, 0.1 })
        for (double lvl : { 0.3, 1.0 })
        {
            auto phrase = voice (fs, { { 0.0, 3.0, 62 } }, 3.0, 25, lvl);
            auto room = noise (fs, 8.0, roomRms, 17);
            std::vector<float> a (room), b (room.size(), 0.0f);
            a.insert (a.end(), phrase.begin(), phrase.end());
            b.insert (b.end(), phrase.begin(), phrase.end());
            auto render = [&] (const std::vector<float>& x, double t0, double t1)
            {
                Rig r (fs, true, [] (VocalStudioAudioProcessor& p) { set (p, IDs::pitchOn, 0); set (p, IDs::revType, 0); });
                const int L = r.p->getLatencySamples();
                auto y = r.run (x, 512, L);
                return db (rms (y, (size_t) (t0 * fs) + (size_t) L, (size_t) (t1 * fs) + (size_t) L));
            };
            std::printf ("room %.2f, voice %.1f: first second %5.1f dB, 1-3 s %5.1f dB\n", roomRms, lvl,
                         render (a, 8.05, 9.0) - render (b, 8.05, 9.0), render (a, 9.0, 11.0) - render (b, 9.0, 11.0));
        }
    // realistic: the room tone goes on under the voice; voice level kept? (output minus the room-only
    // output's power, vs the voice through the chain with Denoise 0, same window)
    // a held 6 s note over continuous room tone (-40 dBFS), dynamics off: voice kept early and late
    for (double lvl : { 1.0, 2.0 })
    {
        auto ph = voice (fs, { { 0.0, 6.0, 62 } }, 6.0, 15, lvl);
        auto rm = noise (fs, 14.0, 0.01, 17);
        std::vector<float> mix (rm), roomOnly (rm), voiceOnly (rm.size(), 0.0f);
        for (size_t i = 0; i < ph.size(); ++i) { mix[(size_t) (8 * fs) + i] += ph[i]; voiceOnly[(size_t) (8 * fs) + i] = ph[i]; }
        auto pw = [&] (const std::vector<float>& x, float dn, double t0, double t1)
        {
            Rig r (fs, true, [dn] (VocalStudioAudioProcessor& p) { set (p, IDs::pitchOn, 0); set (p, IDs::revType, 0); set (p, IDs::denoise, dn); set (p, IDs::rider, 0); set (p, IDs::comp, 0); });
            const int L = r.p->getLatencySamples();
            auto y = r.run (x, 512, L);
            const double v = rms (y, (size_t) (t0 * fs) + (size_t) L, (size_t) (t1 * fs) + (size_t) L);
            return v * v;
        };
        auto kept = [&] (double t0, double t1)
        { return 10 * std::log10 (std::max (1e-12, pw (mix, 60, t0, t1) - pw (roomOnly, 60, t0, t1)) / pw (voiceOnly, 0, t0, t1)); };
        std::printf ("held note %.1f dB above the room tone: voice kept %5.1f dB (0.5-1.5 s), %5.1f dB (3-5.5 s); room tone %5.1f dB\n",
                     db (rms (ph, (size_t) fs, (size_t) (5 * fs))) - db (0.01), kept (8.5, 9.5), kept (11.0, 13.5),
                     10 * std::log10 (pw (roomOnly, 60, 2.0, 7.5) / pw (roomOnly, 0, 2.0, 7.5)));
    }
    // one stage off at a time (room 0.1, voice 0.3)
    auto phrase = voice (fs, { { 0.0, 3.0, 62 } }, 3.0, 25, 0.3);
    auto room = noise (fs, 8.0, 0.1, 17);
    std::vector<float> a (room), b (room.size(), 0.0f);
    a.insert (a.end(), phrase.begin(), phrase.end());
    b.insert (b.end(), phrase.begin(), phrase.end());
    std::vector<std::pair<const char*, std::vector<std::pair<const char*, double>>>> offs {
        { "as set", {} }, { "denoise 0", { { IDs::denoise, 0 } } }, { "rider+comp 0", { { IDs::rider, 0 }, { IDs::comp, 0 } } },
        { "smooth/deess/res 0", { { IDs::smooth, 0 }, { IDs::deess, 0 }, { IDs::resonance, 0 } } }, { "deplosive 0", { { IDs::deplosive, 0 } } },
        { "air 0", { { IDs::air, 0 } } } };
    for (auto& [name, vals] : offs)
    {
        auto render = [&] (const std::vector<float>& x)
        {
            Rig r (fs, true, [&vals] (VocalStudioAudioProcessor& p) { set (p, IDs::pitchOn, 0); set (p, IDs::revType, 0); for (auto& [id, v] : vals) set (p, id, (float) v); });
            const int L = r.p->getLatencySamples();
            auto y = r.run (x, 512, L);
            return db (rms (y, (size_t) (9.0 * fs) + (size_t) L, (size_t) (11.0 * fs) + (size_t) L));
        };
        std::printf ("  %-20s 1-3 s %5.1f dB\n", name, render (a) - render (b));
    }
}

// --diag-context: the same 5.5 s sweep, preceded by different things in the same instance (no gap),
// Track mode, pitch off: which context makes Denoise take a sweep down ~16 dB?
void diagnoseContext()
{
    const double fs = 48000.0;
    const int len = 1 << 18;
    std::vector<float> sw ((size_t) len);
    const double f1 = 20.0, f2 = 22000.0, T = len / fs, K = T / std::log (f2 / f1);
    for (int i = 0; i < len; ++i)
        sw[(size_t) i] = (float) (0.251 * std::min ({ 1.0, i / 480.0, (len - i) / 480.0 }) * std::sin (2 * kPi * f1 * K * (std::exp (i / fs / K) - 1.0)));
    const int pre = (int) (8 * fs);
    auto tone = [&] { std::vector<float> v ((size_t) pre); for (int i = 0; i < pre; ++i) v[(size_t) i] = 0.5f * (float) std::sin (2 * kPi * 999.76 * i / fs); return v; };
    auto pink = [&] (double r) { return noise (fs, 8.0, r, 3); };
    std::vector<std::pair<const char*, std::vector<float>>> ctx {
        { "fresh", {} }, { "after 8 s tone -6 dBFS", tone() }, { "after 8 s pink -12 dBFS", pink (0.25) },
        { "after 8 s pink -30 dBFS", pink (0.03) }, { "after 3 sweeps", {} } };
    ctx[4].second.reserve ((size_t) len * 3);
    for (int k = 0; k < 3; ++k) ctx[4].second.insert (ctx[4].second.end(), sw.begin(), sw.end());
    const int N = len * 2;
    vs::Fft fft; fft.prepare (N);
    std::vector<std::complex<float>> X ((size_t) N / 2 + 1), Y ((size_t) N / 2 + 1);
    { std::vector<float> xc ((size_t) N, 0.0f); std::copy (sw.begin(), sw.end(), xc.begin()); fft.forwardReal (xc.data(), X.data()); }
    for (auto& [name, before] : ctx)
        for (double dn : { 0.0, 60.0 })
        {
            std::vector<float> x (before);
            const size_t start = x.size();
            x.insert (x.end(), sw.begin(), sw.end());
            x.resize (x.size() + (size_t) len, 0.0f);
            Rig r (fs, true, [dn] (VocalStudioAudioProcessor& p) { set (p, IDs::pitchOn, 0); set (p, IDs::revType, 0); set (p, IDs::denoise, (float) dn); });
            const int L = r.p->getLatencySamples();
            auto y = r.run (x, 512, L);
            std::vector<float> ya ((size_t) N, 0.0f);
            for (int i = 0; i < N; ++i) ya[(size_t) i] = y[start + (size_t) i + (size_t) L];
            fft.forwardReal (ya.data(), Y.data());
            std::printf ("%-24s denoise %2.0f:", name, dn);
            for (double f : { 100.0, 1000.0, 4000.0, 10000.0 })
            {
                std::complex<double> cxy = 0; double ex = 0;
                for (int k = (int) (f / std::pow (2.0, 1.0 / 12) * N / fs); k <= (int) (f * std::pow (2.0, 1.0 / 12) * N / fs); ++k)
                { cxy += std::complex<double> (Y[(size_t) k]) * std::conj (std::complex<double> (X[(size_t) k])); ex += std::norm (X[(size_t) k]); }
                std::printf ("  %5.0f Hz %6.1f", f, 20 * std::log10 (std::abs (cxy) / ex));
            }
            std::printf ("\n");
        }
}

// --diag-sweep: THE_LAB-style frequency response - log sweep at -12 dBFS from a fresh instance,
// deconvolved (output spectrum / input spectrum, latency removed), Track mode, reverb off
void diagnoseSweep()
{
    const double fs = 48000.0;
    struct Cfg { const char* name; std::vector<std::pair<const char*, double>> vals; };
    std::vector<Cfg> cfgs {
        { "as reported", { { IDs::revType, 0 } } },
        { "pitch off", { { IDs::revType, 0 }, { IDs::pitchOn, 0 } } },
        { "denoise 0", { { IDs::revType, 0 }, { IDs::denoise, 0 } } },
        { "rider+comp 0", { { IDs::revType, 0 }, { IDs::rider, 0 }, { IDs::comp, 0 } } },
        { "both off", { { IDs::revType, 0 }, { IDs::denoise, 0 }, { IDs::pitchOn, 0 } } } };
    for (int len : { 1 << 18, 1 << 20 })   // 5.5 s and 22 s: a slow sweep is corrected like a sung glide
    {
        const int N = len * 2;
        std::vector<float> x ((size_t) N, 0.0f);
        const double f1 = 20.0, f2 = 22000.0, T = len / fs, K = T / std::log (f2 / f1);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / fs;
            const double fade = std::min ({ 1.0, i / 480.0, (len - i) / 480.0 });
            x[(size_t) i] = (float) (0.251 * fade * std::sin (2 * kPi * f1 * K * (std::exp (t / K) - 1.0)));
        }
        vs::Fft fft; fft.prepare (N);
        std::vector<std::complex<float>> X ((size_t) N / 2 + 1), Y ((size_t) N / 2 + 1);
        auto xc = x; fft.forwardReal (xc.data(), X.data());
        for (auto& cf : cfgs)
        {
            Rig r (fs, true, [&cf] (VocalStudioAudioProcessor& p) { for (auto& [id, v] : cf.vals) set (p, id, (float) v); });
            const int L = r.p->getLatencySamples();
            auto y = r.run (x, 512, L);
            std::vector<float> ya ((size_t) N, 0.0f);
            for (int i = 0; i < N; ++i) ya[(size_t) i] = y[(size_t) (i + L)];
            fft.forwardReal (ya.data(), Y.data());
            std::printf ("sweep %4.1f s  %-13s", T, cf.name);
            for (double f : { 100.0, 1000.0, 4000.0, 10000.0 })
            {
                // 1/6-octave power average of |Y|^2 / |X|^2
                double ey = 0, ex = 0;
                for (int k = (int) (f / std::pow (2.0, 1.0 / 12) * N / fs); k <= (int) (f * std::pow (2.0, 1.0 / 12) * N / fs); ++k)
                { ey += std::norm (Y[(size_t) k]); ex += std::norm (X[(size_t) k]); }
                // coherent part: |sum Y X*| / sum |X|^2
                std::complex<double> cxy = 0;
                for (int k = (int) (f / std::pow (2.0, 1.0 / 12) * N / fs); k <= (int) (f * std::pow (2.0, 1.0 / 12) * N / fs); ++k)
                    cxy += std::complex<double> (Y[(size_t) k]) * std::conj (std::complex<double> (X[(size_t) k]));
                std::printf ("  %5.0f Hz %6.1f (coh %6.1f)", f, 10 * std::log10 (ey / ex), 20 * std::log10 (std::abs (cxy) / ex));
            }
            std::printf ("\n");
        }
    }
}

// --diag-click: the click scan's Input jump (-24 -> +24 -> -24 dB on a sung note, Track mode), stage by stage
void diagnoseClick()
{
    const double fs = 48000.0;
    auto v = voice (fs, { { 0.0, 3.0, 60 } }, 3.0, 20, 2.0);
    auto spikes = [&] (const std::vector<float>& y, double& at)
    {
        double mx = 0, sum = 0; size_t n = 0;
        for (size_t i = (size_t) (0.5 * fs); i + 3 < y.size() - 4800; ++i)
        {
            const double d = y[i + 3] - 3.0 * y[i + 2] + 3.0 * y[i + 1] - y[i];
            if (std::abs (d) > mx) { mx = std::abs (d); at = i / fs; }
            sum += std::abs (d); ++n;
        }
        return mx / (sum / (double) n + 1e-12);
    };
    std::vector<std::pair<const char*, std::vector<std::pair<const char*, double>>>> cases {
        { "defaults", {} }, { "pitch off", { { IDs::pitchOn, 0 } } }, { "denoise 0", { { IDs::denoise, 0 } } },
        { "rider 0", { { IDs::rider, 0 } } }, { "comp 0", { { IDs::comp, 0 } } }, { "smooth+deess+res 0", { { IDs::smooth, 0 }, { IDs::deess, 0 }, { IDs::resonance, 0 } } },
        { "deplosive 0", { { IDs::deplosive, 0 } } }, { "reverb off", { { IDs::revType, 0 } } } };
    for (auto& [name, vals] : cases)
    {
        auto setup = [&vals] (VocalStudioAudioProcessor& p) { for (auto& [id, val] : vals) set (p, id, (float) val); };
        Rig r (fs, true, setup); double at0 = 0; const double b0 = spikes (r.run (v), at0);
        Rig r2 (fs, true, setup);
        set (*r2.p, IDs::inGain, -24);
        auto y = r2.run (v, 512, 0, nullptr, [&] (int64_t s)
        {
            if (s == (int64_t) (1.0 * fs) / 512 * 512) set (*r2.p, IDs::inGain, 24);
            if (s == (int64_t) (2.0 * fs) / 512 * 512) set (*r2.p, IDs::inGain, -24);
        });
        double at = 0; const double b1 = spikes (y, at);
        double pk = 0; for (size_t i = (size_t) (1.2 * fs); i < (size_t) (1.9 * fs); ++i) pk = std::max (pk, (double) std::abs (y[i]));
        double mx = 0, ref = 0; for (size_t i = (size_t) (1.2 * fs); i < (size_t) (1.9 * fs); ++i) { mx = std::max (mx, (double) std::abs (y[i + 3] - 3 * y[i + 2] + 3 * y[i + 1] - y[i])); ref = std::max (ref, (double) std::abs (y[i])); }
        std::printf ("%-20s x%.2f (worst spike at %.3f s); steady +24 dB stretch: peak %.2f, largest 3rd difference %.3f\n", name, b1 / b0, at, pk, mx);
    }
}

// --diag-input: Input +24 dB for 2 s, Track mode, pitch off, denoise 0; one stage off at a time
void diagnoseInput()
{
    const double fs = 48000.0;
    const int n = (int) (16 * fs);
    auto x = noise (fs, 16.0, 0.03, 21);
    for (int i = 0; i < n; ++i) x[(size_t) i] += 0.25f * (float) std::sin (2 * kPi * 999.76 * i / fs);
    std::vector<std::pair<const char*, std::pair<const char*, double>>> cases {
        { "as set", { IDs::outGain, 0 } }, { "- rider", { IDs::rider, 0 } }, { "- comp", { IDs::comp, 0 } },
        { "- resonance", { IDs::resonance, 0 } }, { "- smooth", { IDs::smooth, 0 } }, { "- deess", { IDs::deess, 0 } },
        { "- deplosive", { IDs::deplosive, 0 } }, { "- air", { IDs::air, 0 } } };
    for (auto& [name, off] : cases)
    {
        auto setup = [&off] (VocalStudioAudioProcessor& p) { set (p, IDs::pitchOn, 0); set (p, IDs::denoise, 0); set (p, IDs::revType, 0); set (p, off.first, (float) off.second); };
        Rig ref (fs, true, setup); auto a = ref.run (x);
        Rig r (fs, true, setup);
        auto b = r.run (x, 512, 0, nullptr, [&] (int64_t s)
        {
            if (s == (int64_t) (3 * fs) / 512 * 512) set (*r.p, IDs::inGain, 24);
            if (s == (int64_t) (5 * fs) / 512 * 512) set (*r.p, IDs::inGain, 0);
        });
        std::printf ("%-12s level after return:", name);
        for (double t : { 5.5, 6.5, 8.0, 10.0, 13.0 })
            std::printf (" %5.1f", db (rms (b, (size_t) (t * fs), (size_t) ((t + 1) * fs))) - db (rms (a, (size_t) (t * fs), (size_t) ((t + 1) * fs))));
        std::printf (" dB\n");
    }
}

// --diag-extremes: THE_LAB-style extremes (a control to its minimum or maximum for 2 s, then back)
// with noise + tone in; level and null against an instance whose control never moved
void diagnoseExtremes()
{
    const double fs = 48000.0;
    const int n = (int) (16 * fs);
    auto x = noise (fs, 16.0, 0.03, 21);
    for (int i = 0; i < n; ++i) x[(size_t) i] += 0.25f * (float) std::sin (2 * kPi * 999.76 * i / fs);
    struct Cfg { const char* name; bool track; std::vector<std::pair<const char*, double>> vals; };
    std::vector<Cfg> cfgs {
        { "Track, denoise 60, pitch on", true, { { IDs::revType, 0 } } },
        { "Track, denoise 0, pitch on", true, { { IDs::revType, 0 }, { IDs::denoise, 0 } } },
        { "Track, denoise 60, pitch off", true, { { IDs::revType, 0 }, { IDs::pitchOn, 0 } } },
        { "Track, denoise 0, pitch off", true, { { IDs::revType, 0 }, { IDs::denoise, 0 }, { IDs::pitchOn, 0 } } },
        { "Mix, defaults, reverb off", false, { { IDs::revType, 0 } } } };
    struct Ctl { const char* id; double lo, hi, def; };
    std::vector<Ctl> ctls { { IDs::inGain, -24, 24, 0 }, { IDs::pitchTolerance, 0, 50, 8 }, { IDs::rumble, 20, 300, 80 },
                            { IDs::denoise, 0, 100, 60 }, { IDs::pitchStrength, 0, 100, 70 }, { IDs::comp, 0, 100, 40 },
                            { IDs::rider, 0, 12, 4 }, { IDs::outGain, -24, 12, 0 } };
    for (auto& cf : cfgs)
    {
        std::printf ("== %s\n", cf.name);
        auto setup = [&cf] (VocalStudioAudioProcessor& p) { for (auto& [id, v] : cf.vals) set (p, id, (float) v); };
        Rig ref (fs, cf.track, setup); auto a = ref.run (x);
        for (auto& c : ctls)
            for (double ext : { c.lo, c.hi })
            {
                double def = c.def;
                for (auto& [id, v] : cf.vals) if (juce::String (id) == c.id) def = v;
                Rig r (fs, cf.track, setup);
                auto b = r.run (x, 512, 0, nullptr, [&] (int64_t s)
                {
                    if (s == (int64_t) (3 * fs) / 512 * 512) set (*r.p, c.id, (float) ext);
                    if (s == (int64_t) (5 * fs) / 512 * 512) set (*r.p, c.id, (float) def);
                });
                std::printf ("  %-16s %6.1f  level dB / null dB after return:", c.id, ext);
                for (double t : { 5.5, 6.5, 8.0, 10.0, 13.0 })
                {
                    const size_t s0 = (size_t) (t * fs), s1 = (size_t) ((t + 1.0) * fs);
                    double e = 0, ea = 0;
                    for (size_t i = s0; i < s1; ++i) { const double d = (double) b[i] - a[i]; e += d * d; ea += (double) a[i] * a[i]; }
                    std::printf ("  %5.1f/%6.1f", db (rms (b, s0, s1)) - db (rms (a, s0, s1)), 10 * std::log10 ((e + 1e-30) / (ea + 1e-30)));
                }
                std::printf ("\n");
            }
    }
}

// --diag-psola: the pitch corrector alone; tolerance 50 cents for 2 s, then back: null vs untouched
void diagnosePsola()
{
    const double fs = 48000.0;
    const int n = (int) (14 * fs);
    for (double minHz : { 100.0, 60.0 })
        for (double nz : { 0.0, 0.03 })
        {
            auto x = noise (fs, 14.0, nz > 0 ? nz : 1e-9, 21);
            for (int i = 0; i < n; ++i) x[(size_t) i] += 0.25f * (float) std::sin (2 * kPi * 999.76 * i / fs);
            int64_t shifted = 0, pulled = 0;
            auto render = [&] (bool moved)
            {
                vs::PitchCorrector pc; pc.prepare (fs, 512, minHz);
                struct S { vs::PitchCorrector& p; int64_t& a; int64_t& b; ~S() { a += p.statShifted; b += p.statPulled; } } st { pc, shifted, pulled };
                vs::PitchCorrector::Params pp; pp.strength = 0.7f; pp.speed = 0.35f; pp.toleranceCents = 8.0f;
                pc.setParams (pp); pc.setEnabled (true);
                std::vector<float> y (x);
                for (int s = 0; s < n; s += 512)
                {
                    pp.toleranceCents = (moved && s >= (int) (3 * fs) && s < (int) (5 * fs)) ? 50.0f : 8.0f;
                    pc.setParams (pp);
                    pc.process (&y[(size_t) s], std::min (512, n - s));
                    if (! moved && nz > 0 && minHz > 99 && s % 12288 == 0 && s < (int) (3 * fs))
                        std::printf ("    t %.2f s: detected %.2f Hz, correction %.2f cents\n", s / fs, 440.0 * std::pow (2.0, (pc.curMidi.load() - 69.0) / 12.0), pc.curCorrCents.load());
                }
                return y;
            };
            auto a = render (false), b = render (true);
            std::printf ("\n");
            for (auto* y : { &a, &b })
            {
                std::printf ("  %s peak Hz (from 1.5, 5.5, 7, 9, 11 s):", y == &a ? "untouched" : "moved    ");
                const int N = 32768;
                vs::Fft fft; fft.prepare (N);
                std::vector<float> fr ((size_t) N); std::vector<std::complex<float>> X ((size_t) N / 2 + 1);
                for (double t0 : { 1.5, 5.5, 7.0, 9.0, 11.0 })
                {
                    for (int i = 0; i < N; ++i) fr[(size_t) i] = (*y)[(size_t) (t0 * fs) + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2 * kPi * i / N));
                    fft.forwardReal (fr.data(), X.data());
                    int best = 0; double bv = 0;
                    for (int k = (int) (900.0 * N / fs); k < (int) (1100.0 * N / fs); ++k) if (std::abs (X[(size_t) k]) > bv) { bv = std::abs (X[(size_t) k]); best = k; }
                    const double l = std::log (std::abs (X[(size_t) best - 1]) + 1e-20), c = std::log (bv + 1e-20), r = std::log (std::abs (X[(size_t) best + 1]) + 1e-20);
                    std::printf (" %8.2f", (best + 0.5 * (l - r) / (l - 2 * c + r)) * fs / N);
                }
                std::printf ("\n");
            }
            std::printf ("minHz %3.0f noise %.2f  null after return:", minHz, nz);
            for (double t : { 2.0, 5.5, 6.5, 8.0, 10.0, 12.5 })
            {
                const size_t s0 = (size_t) (t * fs), s1 = (size_t) ((t + 1.0) * fs);
                double e = 0, ea = 0;
                for (size_t i = s0; i < s1; ++i) { const double d = (double) b[i] - a[i]; e += d * d; ea += (double) a[i] * a[i]; }
                std::printf (" %6.1f", 10 * std::log10 ((e + 1e-30) / (ea + 1e-30)));
            }
            std::printf ("   (grains shifted %lld, drifting %lld)\n", (long long) shifted, (long long) pulled);
        }
    // a sung voice (vibrato, glides): does the grid drift ever engage?
    {
        auto v = voice (fs, { { 0.0, 2.0, 57.3 }, { 2.2, 2.2, 63.75 } }, 4.5);
        auto flat = voice (fs, { { 0.0, 2.0, 57.3 }, { 2.2, 2.2, 63.75 } }, 4.5, 0.0);
        v.insert (v.end(), flat.begin(), flat.end());
        vs::PitchCorrector pc; pc.prepare (fs, 512, 60.0);
        vs::PitchCorrector::Params pp; pp.strength = 0.7f; pp.speed = 0.35f; pp.toleranceCents = 8.0f;
        pc.setParams (pp); pc.setEnabled (true);
        for (size_t s = 0; s < v.size(); s += 512) pc.process (&v[s], (int) std::min<size_t> (512, v.size() - s));
        std::printf ("sung notes (with vibrato, then straight): grains shifted %lld, drifting %lld\n", (long long) pc.statShifted, (long long) pc.statPulled);
    }
}

// --diag-trackfx: a steady 999.76 Hz tone and a sine sweep through Track mode, stage by stage
void diagnoseTrackFx()
{
    const double fs = 48000.0;
    struct Cfg { const char* name; std::vector<std::pair<const char*, double>> vals; };
    std::vector<Cfg> cfgs {
        { "as reported", { { IDs::revType, 0 } } },
        { "pitch off", { { IDs::revType, 0 }, { IDs::pitchOn, 0 } } },
        { "denoise 0", { { IDs::revType, 0 }, { IDs::denoise, 0 } } },
        { "both off", { { IDs::revType, 0 }, { IDs::denoise, 0 }, { IDs::pitchOn, 0 } } } };
    // tone: spectrum of 65536 samples from 5 s on
    const int N = 65536;
    vs::Fft fft; fft.prepare (N);
    for (auto& cf : cfgs)
    {
        const int n = (int) (8 * fs);
        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i) x[(size_t) i] = 0.501f * (float) std::sin (2 * kPi * 999.76 * i / fs);
        Rig r (fs, true, [&cf] (VocalStudioAudioProcessor& p) { for (auto& [id, v] : cf.vals) set (p, id, (float) v); });
        const int L = r.p->getLatencySamples();
        auto y = r.run (x, 512, L);
        std::vector<float> fr ((size_t) N), in ((size_t) N);
        std::vector<std::complex<float>> X ((size_t) N / 2 + 1), XI ((size_t) N / 2 + 1);
        const size_t s0 = (size_t) (5 * fs);
        for (int i = 0; i < N; ++i)
        {
            const float w = (float) (0.5 - 0.5 * std::cos (2 * kPi * i / N));
            fr[(size_t) i] = y[s0 + (size_t) L + (size_t) i] * w; in[(size_t) i] = x[s0 + (size_t) i] * w;
        }
        fft.forwardReal (fr.data(), X.data()); fft.forwardReal (in.data(), XI.data());
        const double binHz = fs / N;
        const int kt = (int) std::lround (999.76 / binHz);
        const double ref = std::abs (XI[(size_t) kt]);
        // peaks between 900 and 1100 Hz
        std::vector<std::pair<double, double>> pk;
        for (int k = (int) (900 / binHz); k < (int) (1100 / binHz); ++k)
        {
            const double v = std::abs (X[(size_t) k]);
            if (v > std::abs (X[(size_t) k - 1]) && v >= std::abs (X[(size_t) k + 1])) pk.push_back ({ db (v / ref), k * binHz });
        }
        std::sort (pk.begin(), pk.end(), [] (auto& a, auto& b) { return a.first > b.first; });
        std::printf ("%-12s tone: level %6.1f dB; peaks (dB re input tone @Hz):", cf.name, db (rms (y, s0 + (size_t) L, s0 + (size_t) L + (size_t) N)) - db (rms (x, s0, s0 + (size_t) N)));
        for (size_t i = 0; i < std::min<size_t> (6, pk.size()); ++i) std::printf (" %.1f@%.1f", pk[i].first, pk[i].second);
        double dc = 0; for (int i = 0; i < N; ++i) dc += y[s0 + (size_t) L + (size_t) i];
        std::printf ("  DC %.1f dBFS\n", db (std::abs (dc / N)));

        // log sweep 20 Hz - 22 kHz, -12 dBFS, 6 s, after 1 s of silence: gain by time window
        const double T = 6.0, f1 = 20.0, f2 = 22000.0;
        const int ns = (int) ((T + 2) * fs);
        std::vector<float> sw ((size_t) ns, 0.0f);
        for (int i = 0; i < (int) (T * fs); ++i)
        {
            const double t = i / fs, K = T / std::log (f2 / f1);
            sw[(size_t) i + (size_t) fs] = 0.251f * (float) std::sin (2 * kPi * f1 * K * (std::exp (t / K) - 1.0));
        }
        Rig r2 (fs, true, [&cf] (VocalStudioAudioProcessor& p) { for (auto& [id, v] : cf.vals) set (p, id, (float) v); });
        auto ys = r2.run (sw, 512, L);
        std::printf ("%-12s sweep gain:", cf.name);
        for (double f : { 100.0, 200.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 })
        {
            const double t = T * std::log (f / f1) / std::log (f2 / f1);
            const size_t c0 = (size_t) ((1.0 + t - 0.02) * fs), c1 = (size_t) ((1.0 + t + 0.02) * fs);
            std::printf (" %.0fHz %5.1f", f, db (rms (ys, c0 + (size_t) L, c1 + (size_t) L)) - db (rms (sw, c0, c1)));
        }
        std::printf ("\n");
    }
}

void testAnalyze()
{
    std::printf ("\n-- Analyze: auto EQ and key detection --\n");
    const double fs = 48000.0;
    // a melody in D major (D E F# G A B C# D), with a boxy, dull tone to correct
    std::vector<Note> mel;
    const double ns[] = { 62, 64, 66, 67, 69, 71, 73, 74, 69, 66, 62, 67, 71, 74, 69, 62 };
    double t = 0.1;
    for (double m : ns) { mel.push_back ({ t, 0.55, m }); t += 0.65; }
    auto v = voice (fs, mel, t + 0.5, 20, 2.0);
    vs::Biquad box; box.setPeak (fs, 400, 1.2, 6.0);
    vs::Biquad dull; dull.setHighShelf (fs, 4000, -6.0);
    for (auto& s : v) s = dull.process (box.process (s, 0), 0);
    Rig r (fs, false, [] (VocalStudioAudioProcessor& p) { set (p, IDs::pitchStrength, 0); });
    // USE DETECTED before anything was sung: Key and Scale stay as they are (it used to set C major)
    {
        set (*r.p, IDs::key, 4); set (*r.p, IDs::scale, 2);
        const bool applied = r.p->applyDetectedKey();
        const bool kept = (int) r.p->apvts.getRawParameterValue (IDs::key)->load() == 4 && (int) r.p->apvts.getRawParameterValue (IDs::scale)->load() == 2;
        check (! applied && kept && ! r.p->detectedKeyUsable(), "USE DETECTED does nothing until a key has been detected");
        set (*r.p, IDs::key, 0); set (*r.p, IDs::scale, 0);
    }
    r.p->startAnalyze();
    r.run (v);
    const auto msg = r.p->stopAnalyzeAndApply();
    note (msg);
    {
        set (*r.p, IDs::key, 0); set (*r.p, IDs::scale, 0);
        const bool applied = r.p->applyDetectedKey();
        check (applied && (int) r.p->apvts.getRawParameterValue (IDs::key)->load() == 2 && (int) r.p->apvts.getRawParameterValue (IDs::scale)->load() == 1,
               "USE DETECTED sets the detected key (D major)");
    }
    std::array<float, vs::VocalEq::kBands> g {};
    juce::String gs;
    for (int b = 0; b < vs::VocalEq::kBands; ++b) { g[(size_t) b] = r.p->apvts.getRawParameterValue (IDs::eq (b))->load(); gs << juce::String (g[(size_t) b], 1) << " "; }
    note ("EQ gains (63 Hz .. 12.5 kHz): " + gs);
    const int key = (int) r.p->apvts.getRawParameterValue (IDs::key)->load();
    const int scale = (int) r.p->apvts.getRawParameterValue (IDs::scale)->load();
    check (key == 2 && scale == 1, "detects D major");
    check (g[3] < -1.0f || g[2] < -1.0f, "cuts the boxy 250-500 Hz region");
    check (g[6] > 1.0f || g[7] > 1.0f || g[8] > 1.0f, "brightens the dull top");
    report << "## Analyze\n\nA D-major melody with a +6 dB boxy bump at 400 Hz and a -6 dB dull top: \"" << msg << "\" EQ gains (63 Hz to 12.5 kHz): " << gs << "dB.\n\n";
}

// Standalone file player and SAVE PROCESSED
void testFilePlayer()
{
    std::printf ("\n-- standalone: playing a file, and saving it processed --\n");
    const double fs = 48000.0;
    auto v = voice (fs, { { 0.1, 1.0, 57.3 }, { 1.3, 1.0, 62.6 } }, 2.5, 25, 2.0);
    auto nz = noise (fs, 2.5, 0.01, 4);
    for (size_t i = 0; i < v.size(); ++i) v[i] += nz[i];
    const int len = (int) v.size();
    auto setup = [] (VocalStudioAudioProcessor& p) { set (p, IDs::revType, 0); set (p, IDs::doubler, 40); };
    auto makeTake = [&] (const std::vector<float>& x, double rate)
    {
        auto t = std::make_unique<vs::FilePlayer::Take>();
        t->audio.setSize (1, (int) x.size());
        for (int i = 0; i < (int) x.size(); ++i) t->audio.setSample (0, i, x[(size_t) i]);
        t->rate = rate; t->name = "test.wav";
        return t;
    };
    for (int track = 0; track < 2; ++track)
    {
        const juce::String mode = track ? "Track" : "Mix";
        Rig live (fs, track == 1, setup);
        const int L = live.p->getLatencySamples();
        auto ref = live.run (v, 512, L);
        // playing the file = singing the same thing into the input
        Rig fp (fs, track == 1, setup);
        fp.p->player.swapTake (makeTake (v, fs));
        fp.p->player.source.store (true);
        fp.p->player.playing.store (true);
        auto out = fp.run (std::vector<float> ((size_t) len + (size_t) L, 0.0f), 333);
        double d = 0; for (size_t i = 0; i < out.size(); ++i) d = std::max (d, (double) std::abs (out[i] - ref[i]));
        note (mode + ": file played through the chain vs the same audio as live input: " + juce::String (db (d), 1) + " dBFS");
        check (d < 1e-6, "playing a file gives exactly what the same audio through the input gives (" + mode + " mode)");
        check (! fp.p->player.playing.load(), "playback stops at the end of the file (" + mode + " mode)");

        // SAVE PROCESSED: the whole file through a copy of the chain, lined up with the original
        auto take = makeTake (v, fs);
        const auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("vs_render_test.wav");
        OfflineRender rd (*live.p, *take, f);
        rd.run();
        double e = 0;
        for (int i = 0; i < len; ++i) e = std::max (e, (double) std::abs (rd.result.getSample (0, i) - ref[(size_t) (i + L)]));
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> rr (wav.createReaderFor (f.createInputStream().release(), true));
        const bool fileOk = rr != nullptr && rr->lengthInSamples == rd.result.getNumSamples() && rr->numChannels == 2 && (int) rr->sampleRate == (int) fs;
        double fe = 1;
        if (fileOk)
        {
            juce::AudioBuffer<float> back (2, (int) rr->lengthInSamples);
            rr->read (&back, 0, back.getNumSamples(), 0, true, true);
            fe = 0; for (int i = 0; i < back.getNumSamples(); ++i) fe = std::max (fe, (double) std::abs (back.getSample (0, i) - rd.result.getSample (0, i)));
        }
        rr.reset(); f.deleteFile();
        note (mode + ": saved file vs live processing, latency (" + juce::String (rd.latencyTrimmed()) + " samples) trimmed: " + juce::String (db (e), 1)
              + " dBFS; written WAV vs render " + juce::String (db (fe), 1) + " dBFS");
        check (rd.error.isEmpty() && e < 1e-6, "SAVE PROCESSED matches live processing sample for sample, latency trimmed (" + mode + " mode)");
        check (fileOk && fe < 2e-7, "SAVE PROCESSED writes a 24-bit stereo WAV at the file's rate (" + mode + " mode)");
    }
    // a 44.1 kHz file played while the app runs at 48 kHz: converted, pitch and level kept
    {
        const double fr = 44100.0;
        std::vector<float> tone ((size_t) (3 * fr));
        for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.25f * (float) std::sin (2 * kPi * 1000.0 * (double) i / fr);
        Rig fp (fs, true, [] (VocalStudioAudioProcessor& p) { neutral (p); });
        fp.p->player.swapTake (makeTake (tone, fr));
        fp.p->player.source.store (true);
        fp.p->player.playing.store (true);
        auto out = fp.run (std::vector<float> ((size_t) (3.2 * fs), 0.0f));
        int zc = 0; const size_t a = (size_t) (1.0 * fs), b = (size_t) (2.0 * fs);
        for (size_t i = a + 1; i < b; ++i) if (out[i - 1] < 0 && out[i] >= 0) ++zc;
        const double lvl = db (rms (out, a, b)) - db (0.25 / std::sqrt (2.0));
        note ("44.1 kHz file at 48 kHz: " + juce::String (zc) + " Hz, level " + juce::String (lvl, 2) + " dB");
        check (std::abs (zc - 1000) <= 1 && std::abs (lvl) < 0.5, "a file at another sample rate plays at the right pitch and level");
    }
}

void testDelaySync()
{
    std::printf ("\n-- delay sync and reverb --\n");
    const double fs = 48000.0;
    Rig r (fs, true, [] (VocalStudioAudioProcessor& p)
    {
        neutral (p);
        set (p, IDs::dlyOn, 1); set (p, IDs::dlySync, 1); set (p, IDs::dlyNote, 5); set (p, IDs::dlyMix, 50); set (p, IDs::dlyFeedback, 0);
        set (p, IDs::dlyDuck, 0); set (p, IDs::dlyWidth, 0);
    });
    const int L = r.p->getLatencySamples();
    std::vector<float> imp ((size_t) fs, 0.0f);
    for (int i = 0; i < 48; ++i) imp[(size_t) (4800 + i)] = (float) std::sin (kPi * i / 48.0) * 0.5f;   // short burst
    auto y = r.run (imp, 512, 0);
    // find the echo after the direct sound
    size_t direct = 4800 + (size_t) L, best = 0; double bv = 0;
    for (size_t i = direct + 1000; i < y.size(); ++i) if (std::abs (y[i]) > bv) { bv = std::abs (y[i]); best = i; }
    const double ms = (double) (best - direct) * 1000.0 / fs;
    note ("1/16 at 120 BPM: echo after " + juce::String (ms, 1) + " ms");
    check (std::abs (ms - 125.0) < 2.0, "tempo-synced delay: 1/16 note at 120 BPM = 125 ms");

    for (int type : { 1, 2 })
    {
        Rig rr (fs, true, [type] (VocalStudioAudioProcessor& p) { neutral (p); set (p, IDs::revType, (float) type); set (p, IDs::revMix, 50); set (p, IDs::revDuck, 0); });
        auto yr = rr.run (imp, 512, 0);
        const double tail = rms (yr, (size_t) (0.4 * fs), (size_t) (0.6 * fs));
        note (juce::String (type == 1 ? "Plate" : "Room") + " tail level 0.3 s after: " + juce::String (db (tail), 1) + " dBFS");
        check (db (tail) > -80.0, juce::String (type == 1 ? "plate" : "room") + " reverb produces a tail");
    }
}

void testStateAndModes()
{
    std::printf ("\n-- state recall, presets, mode switch --\n");
    VocalStudioAudioProcessor a;
    a.setCurrentProgram (3);
    set (a, IDs::eq (4), -3.3f);
    set (a, IDs::note (6), 0);
    juce::MemoryBlock mb;
    a.getStateInformation (mb);
    VocalStudioAudioProcessor b;
    b.setStateInformation (mb.getData(), (int) mb.getSize());
    bool same = true;
    for (auto* prm : a.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
            same &= std::abs (rp->getValue() - b.apvts.getParameter (rp->getParameterID())->getValue()) < 1e-6f;
    check (same, "every parameter restored exactly from saved state");
    check (b.getCurrentProgram() == 3, "preset number restored");

    Rig r (48000.0);
    const int mixL = r.p->getLatencySamples();
    set (*r.p, IDs::pitchOn, 0);
    set (*r.p, IDs::mode, 1);
    r.p->reconfigureNow();
    const int trL = r.p->getLatencySamples();
    set (*r.p, IDs::pitchOn, 1);
    r.p->reconfigureNow();
    const int trPitchL = r.p->getLatencySamples();
    note ("latency: Mix " + juce::String (mixL / 48.0, 1) + " ms, Track " + juce::String (trL / 48.0, 1) + " ms, Track + pitch " + juce::String (trPitchL / 48.0, 1) + " ms");
    check (trL < mixL && trPitchL > trL, "mode and pitch switches reconfigure the latency");
    report << "## Latency by mode (48 kHz, defaults)\n\nMix " << juce::String (mixL / 48.0, 1) << " ms (reported to the DAW, which compensates), Track "
           << juce::String (trL / 48.0, 1) << " ms with pitch correction off, " << juce::String (trPitchL / 48.0, 1) << " ms with it on.\n\n";
}

void testCpu()
{
    std::printf ("\n-- CPU (48 kHz, percent of one core) --\n");
    report << "## CPU\n\n48 kHz, 512-sample blocks, sung voice, real-time mode (denoiser on its worker thread):\n\n| Setting | % of one core |\n|---|---|\n";
    const double fs = 48000.0, T = 8.0;
    auto v = voice (fs, { { 0.1, 3.5, 57 }, { 4.0, 3.5, 62 } }, T, 25, 2.0);
    auto measure = [&] (const juce::String& name, bool track, std::function<void (VocalStudioAudioProcessor&)> setup)
    {
        Rig r (fs, track, setup);
        r.p->setNonRealtime (false);
        auto t0 = std::chrono::steady_clock::now();
        r.run (v);
        const double pct = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / T * 100.0;
        note (name + ": " + juce::String (pct, 1) + " %");
        report << "| " << name << " | " << juce::String (pct, 1) << " % |\n";
        return pct;
    };
    const double full = measure ("Mix mode, defaults (Natural Polish)", false, {});
    measure ("Mix mode, Pop Lead (delay on)", false, [] (VocalStudioAudioProcessor& p) { p.setCurrentProgram (1); });
    measure ("Track mode, defaults", true, {});
    measure ("Neutral (everything off)", false, neutral);
    check (full < 50.0, "whole plugin under 50 % of one core in Mix mode");
    report << "\nThe neural denoiser's own thread adds about 5 % of a core in Mix mode (DfnCheck).\n\n";
}
} // namespace

// Renders the editor (after some audio, so meters and readouts show something) to a PNG.
static bool gShotReverbOff = false;
void screenshot (const juce::String& file)
{
    const double fs = 48000.0;
    Rig r (fs, false, [] (VocalStudioAudioProcessor& p) { if (gShotReverbOff) { set (p, IDs::revType, 0); set (p, IDs::dlyOn, 1); } });
    auto v = voice (fs, { { 0.1, 2.5, 64.3 } }, 2.6, 25, 2.0);
    auto nz = noise (fs, 2.6, 0.003, 9);
    for (size_t i = 0; i < v.size(); ++i) v[i] += nz[i];
    r.p->setNonRealtime (false);
    r.run (v);
    std::unique_ptr<juce::AudioProcessorEditor> ed (r.p->createEditorIfNeeded());
    ed->setSize (VocalStudioAudioProcessorEditor::kBaseW, static_cast<VocalStudioAudioProcessorEditor*> (ed.get())->getBaseHeight());
    for (int i = 0; i < 5; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (40);
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
    juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile (file);
    f.getParentDirectory().createDirectory(); f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat().writeImageToStream (img, os);
    r.p->editorBeingDeleted (ed.get());
    std::printf ("screenshot written to %s\n", f.getFullPathName().toRawUTF8());
}

// --diag-load F...: open files with the standalone's loader; length, rate, channels, level, and the
// largest difference from the first file (encode the same audio in several formats to compare them)
void diagnoseLoad (const juce::StringArray& files)
{
    std::vector<float> first;
    for (auto& path : files)
    {
        VocalStudioAudioProcessor p;
        const auto err = p.loadFileTake (juce::File (path));
        const auto* t = p.player.getTake();
        if (err.isNotEmpty() || t == nullptr) { std::printf ("%-40s FAILED: %s\n", juce::File (path).getFileName().toRawUTF8(), err.toRawUTF8()); continue; }
        std::vector<float> x ((size_t) t->audio.getNumSamples());
        for (size_t i = 0; i < x.size(); ++i) x[i] = t->audio.getSample (0, (int) i);
        double diff = 0;
        if (first.empty()) first = x;
        else for (size_t i = 0; i < std::min (x.size(), first.size()); ++i) diff = std::max (diff, (double) std::abs (x[i] - first[i]));
        std::printf ("%-40s %.3f s, %.0f Hz, %d ch, RMS %.1f dBFS, vs first file %.1f dB\n", t->name.toRawUTF8(),
                     (double) x.size() / t->rate, t->rate, t->audio.getNumChannels(), db (rms (x, 0, x.size())), db (diff));
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-load")
    {
        juce::StringArray f; for (int j = i + 1; j < argc; ++j) f.add (argv[j]);
        diagnoseLoad (f); return 0;
    }
    juce::String labFile;
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--lab" && i + 1 < argc) labFile = argv[i + 1];
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag") { diagnose(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-rumble") { diagnoseRumble(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-loudness") { diagnoseLoudness(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-rumble2") { diagnoseRumble2(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-rates") { for (int j = 1; j < argc; ++j) if (juce::String (argv[j]) == "--pink") gDiagPink = true; if (i + 2 < argc) { gDiagFrom = std::atof (argv[i + 1]); gDiagTo = std::atof (argv[i + 2]); } diagnoseRates(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-transport") { diagnoseTransport(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-tone") { for (int j = 1; j < argc; ++j) if (juce::String (argv[j]) == "--pitch") gDiagPitch = true; diagnoseTone(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-rider") { diagnoseRider(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-extremes") { diagnoseExtremes(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-trackfx") { diagnoseTrackFx(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-psola") { diagnosePsola(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-input") { diagnoseInput(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-click") { diagnoseClick(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-sweep") { diagnoseSweep(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-steps") { diagnoseSteps(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-context") { diagnoseContext(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--diag-entry") { diagnoseEntry(); return 0; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--only-recovery") { testRecovery(); return failures; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--only-consonant") { testConsonant(); return failures; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--only-blocks") { testBlockSizeIndependence(); return failures; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--only-file") { testFilePlayer(); testAnalyze(); return failures; }
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--screenshot" && i + 1 < argc)
    {
        for (int j = 1; j < argc; ++j) if (juce::String (argv[j]) == "--reverb-off") gShotReverbOff = true;   // and the delay on
        for (int j = 1; j < argc; ++j) if (juce::String (argv[j]) == "--standalone") VocalStudioAudioProcessor::forceStandaloneUi = true;
        screenshot (argv[i + 1]); return 0;
    }

    report << "# " << VS_PRODUCT_NAME << " " << VS_VERSION << " - measurements\n\n"
           << "Generated by `VocalTest.exe --lab` (Tests/VocalTest.cpp), in the spirit of THE_LAB: latency reported vs measured, "
              "nulls, true peak, clicks, and what each processor actually does to test signals.\n\n";
    testLatencyAndNull();
    testBlockSizeIndependence();
    testSilenceAndExtremes();
    testPitch();
    testDeessAndPlosive();
    testConsonant();
    testDenoise();
    testSustainedNote();
    testRecovery();
    testLabSignals();
    testSilenceAndBuzz();
    testAudioThread();
    testAnalyze();
    testFilePlayer();
    testDelaySync();
    testStateAndModes();
    testClickScan();
    testCpu();

    std::printf ("\n%d failure(s)\n", failures);
    if (labFile.isNotEmpty())
    {
        juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile (labFile);
        f.getParentDirectory().createDirectory();
        f.replaceWithText (report);
        std::printf ("report written to %s\n", f.getFullPathName().toRawUTF8());
    }
    return failures;
}
