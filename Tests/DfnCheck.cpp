// Developer check of the neural denoiser on its own:
//  - the streaming engine (Source/DfnEngine.h) matches DeepFilterNet3 run through ONNX Runtime
//  - with Denoise at 0 the path is transparent (latency exact, null residual) at 48 and 44.1 kHz
//  - noise removal on speech (Tools\make_speech.ps1), repeatability, cost per 10 ms frame
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Source/DeepFilter.h"
#include "OrtReference.h"
#include "TestSignals.h"
#include <cstdio>
#include <chrono>

using namespace testsig;

static std::vector<float> loadSpeech()
{
    auto f = juce::File::getCurrentWorkingDirectory().getChildFile ("test-output/speech48.wav");
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> rd (f.existsAsFile() ? wav.createReaderFor (f.createInputStream().release(), true) : nullptr);
    if (rd == nullptr) return {};
    juce::AudioBuffer<float> b (1, (int) rd->lengthInSamples);
    rd->read (&b, 0, b.getNumSamples(), 0, true, false);
    std::vector<float> v (b.getReadPointer (0), b.getReadPointer (0) + std::min (b.getNumSamples(), 8 * 48000));
    for (auto& s : v) s *= 0.5f;
    return v;
}

static std::vector<float> stream (vs::DeepFilter& d, const std::vector<float>& in, int block, int extra)
{
    std::vector<float> x (in);
    x.resize (in.size() + (size_t) extra, 0.0f);
    for (size_t p = 0; p < x.size(); p += (size_t) block)
        d.process (&x[p], (int) std::min<size_t> ((size_t) block, x.size() - p));
    return x;
}

int main()
{
    int fails = 0;
    auto check = [&] (bool ok, const juce::String& what) { std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8()); if (! ok) ++fails; };
    auto clean = loadSpeech();
    if (clean.empty()) { std::printf ("run Tools\\make_speech.ps1 first\n"); return 1; }
    const double fs = 48000.0;
    auto nz = noise (fs, clean.size() / fs + 0.1, 0.01, 1234);
    std::vector<float> noisy (clean.size());
    for (size_t i = 0; i < noisy.size(); ++i) noisy[i] = clean[i] + nz[i];

    vs::DeepFilter d;
    d.prepare (fs, 512);
    std::printf ("status: %s\n", vs::DeepFilter::status().message.toRawUTF8());
    check (d.isLoaded(), "network built from the embedded model");
    if (! d.isLoaded()) return 1;

    // ---- engine vs ONNX Runtime, same features ----
    {
        std::vector<float> fe, fsp, m1, c1, m2, c2;
        d.featuresForTest (noisy, fe, fsp);
        const auto t0 = std::chrono::steady_clock::now();
        d.runNetworkForTest (fe, fsp, m1, c1);
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        const size_t T = fe.size() / 32;
        std::printf ("engine: %zu frames, %.3f ms per 10 ms frame (%.1f%% of a core)\n", T, 1000.0 * secs / (double) T, 100.0 * secs / ((double) T * 0.01));
        juce::String err;
        if (vs::ortReference (fe, fsp, m2, c2, err))
        {
            double dm = 0, dc = 0, cm = 0;
            for (size_t i = 0; i < m1.size(); ++i) dm = std::max (dm, (double) std::abs (m1[i] - m2[i]));
            for (size_t i = 0; i < c1.size(); ++i) { dc = std::max (dc, (double) std::abs (c1[i] - c2[i])); cm = std::max (cm, (double) std::abs (c2[i])); }
            std::printf ("largest difference vs ONNX Runtime: gains %.2e, coefficients %.2e (coefficients up to %.2f)\n", dm, dc, cm);
            check (dm < 1e-3 && dc < 1e-3, "streaming engine matches DeepFilterNet3 in ONNX Runtime");
        }
        else std::printf ("ONNX Runtime reference not available (%s) - comparison skipped\n", err.toRawUTF8());
    }

    // ---- transparency at amount 0 ----
    for (double rate : { 48000.0, 44100.0 })
    {
        vs::DeepFilter t;
        t.prepare (rate, 512);
        t.setAmount (0.0f); t.reset();
        const int L = t.getLatency();
        auto v = voice (rate, { { 0.2, 1.5, 60 }, { 1.9, 1.0, 64.3 } }, 3.0, 25, 2.0);
        auto n2 = noise (rate, 3.0, 0.003);
        for (size_t i = 0; i < v.size(); ++i) v[i] += n2[i];
        auto y = stream (t, v, 512, L + 10);
        std::vector<float> a (v), b (y.begin() + L, y.begin() + L + (long) v.size());
        if (rate != 48000.0)
            for (double q : { 0.5098, 0.6013, 0.9000, 2.5629 })
            {
                vs::Biquad f1; f1.setLowPass (rate, 16000.0, q); vs::Biquad f2 = f1;
                for (auto& s : a) s = f1.process (s, 0);
                for (auto& s : b) s = f2.process (s, 0);
            }
        double e = 0, s = 0;
        for (size_t i = 0; i < v.size(); ++i) { e += (b[i] - a[i]) * (b[i] - a[i]); s += (double) a[i] * a[i]; }
        const double nullDb = 10.0 * std::log10 (e / s + 1e-30);
        std::printf ("fs %.0f: latency %d samples (%.1f ms), amount-0 null %.1f dB\n", rate, L, 1000.0 * L / rate, nullDb);
        check (nullDb < (rate == 48000.0 ? -100.0 : -60.0) && t.underruns == 0, "transparent at amount 0, latency exact @" + juce::String (rate));
    }

    // ---- noise removal on speech ----
    {
        d.setAmount (1.0f);
        auto out = d.processWholeForTest (noisy);
        auto snr = [&] (const std::vector<float>& x)
        {
            double s = 0, e = 0;
            for (size_t i = 0; i < clean.size(); ++i) { s += (double) clean[i] * clean[i]; e += (x[i] - clean[i]) * (x[i] - clean[i]); }
            return 10.0 * std::log10 (s / e);
        };
        std::printf ("speech SNR: noisy %.1f dB -> %.1f dB; level clean %.1f / out %.1f dBFS\n", snr (noisy), snr (out),
                     db (rms (clean, 0, clean.size())), db (rms (out, 0, out.size())));
        check (snr (out) > snr (noisy) + 6.0, "speech: SNR improves by more than 6 dB");
        check (std::abs (db (rms (out, 0, out.size())) - db (rms (clean, 0, clean.size()))) < 2.0, "speech: level kept within 2 dB");
        auto out2 = d.processWholeForTest (noisy);
        double diff = 0; for (size_t i = 0; i < out.size(); ++i) diff = std::max (diff, (double) std::abs (out[i] - out2[i]));
        check (diff == 0.0, "two runs give identical output");
    }
    std::printf ("%d failure(s)\n", fails);
    return fails == 0 ? 0 : 1;
}
