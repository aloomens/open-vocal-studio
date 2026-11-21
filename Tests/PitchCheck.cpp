// Developer check of the pitch corrector: transparency, correction accuracy, vibrato kept.
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Source/Pitch.h"
#include "TestSignals.h"
#include <cstdio>
#include <chrono>

using namespace testsig;

// offline reference pitch: YIN at full rate, parabolic interpolation, 2048-sample windows
static std::vector<double> pitchTrack (const std::vector<float>& x, double fs, double hopSec, std::vector<double>& times)
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
        if (best < 0)
        {
            double bv = 0.35;
            for (int tau = (int) (fs / 1200); tau <= maxLag; ++tau) if (d[(size_t) tau] < bv) { bv = d[(size_t) tau]; best = tau; }
        }
        double f = 0;
        if (best > 1 && best < maxLag)
        {
            const double a = d[(size_t) best - 1], b = d[(size_t) best], c = d[(size_t) best + 1];
            const double off = 0.5 * (a - c) / (a - 2 * b + c);
            f = fs / (best + off);
        }
        out.push_back (f);
        times.push_back ((s + (W + maxLag) / 2.0) / fs);
    }
    return out;
}

static std::vector<float> run (vs::PitchCorrector& p, const std::vector<float>& in, int block)
{
    std::vector<float> x (in);
    x.resize (in.size() + (size_t) p.getLatency() + 16, 0.0f);
    for (size_t s = 0; s < x.size(); s += (size_t) block)
        p.process (&x[s], (int) std::min<size_t> ((size_t) block, x.size() - s));
    return std::vector<float> (x.begin() + p.getLatency(), x.begin() + p.getLatency() + (long) in.size());
}

static void writeWav (const juce::String& name, const std::vector<float>& x, double fs)
{
    auto f = juce::File::getCurrentWorkingDirectory().getChildFile ("test-output").getChildFile (name);
    f.getParentDirectory().createDirectory(); f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), fs, 1, 24, {}, 0));
    if (w == nullptr) return;
    os.release();
    const float* ch[1] = { x.data() };
    w->writeFromFloatArrays (ch, 1, (int) x.size());
}

int main()
{
    int fails = 0;
    auto check = [&] (bool ok, const juce::String& what) { std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8()); if (! ok) ++fails; };

    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        // notes sung 40 cents sharp / 30 flat, with vibrato; noise between them
        auto v = voice (fs, { { 0.2, 1.4, 57.4 }, { 1.8, 1.2, 61.7 }, { 3.2, 1.5, 64.4 } }, 5.0, 30.0, 2.5);
        auto nz = noise (fs, 5.0, 0.001, 5);
        for (size_t i = 0; i < v.size(); ++i) v[i] += nz[i];

        vs::PitchCorrector p;
        p.prepare (fs, 512);
        vs::PitchCorrector::Params prm; prm.strength = 0.0f;
        p.setParams (prm);
        auto y0 = run (p, v, 333);
        double e = 0, s = 0;
        for (size_t i = 0; i < v.size(); ++i) { e += (y0[i] - v[i]) * (y0[i] - v[i]); s += (double) v[i] * v[i]; }
        const double nullDb = 10 * std::log10 (e / s + 1e-30);
        std::printf ("fs %.0f: latency %d (%.1f ms), strength-0 null %.1f dB\n", fs, p.getLatency(), 1000.0 * p.getLatency() / fs, nullDb);
        check (nullDb < -120, "strength 0 is bit-transparent (delayed copy)");

        // full correction, chromatic
        prm.strength = 1.0f; prm.speed = 0.5f; prm.toleranceCents = 0.0f;
        p.setParams (prm);
        p.reset();
        auto t0 = std::chrono::steady_clock::now();
        auto y = run (p, v, 512);
        const double cpu = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / 5.0 * 100.0;
        if (fs == 48000.0) { writeWav ("pitch_in.wav", v, fs); writeWav ("pitch_out.wav", y, fs); }

        std::vector<double> tIn, tOut;
        auto pin = pitchTrack (v, fs, 0.01, tIn), pout = pitchTrack (y, fs, 0.01, tOut);
        // per note: mean cents off the nearest semitone in the steady part, and vibrato depth (std)
        struct Seg { double a, b, target; };
        for (auto sg : { Seg { 0.9, 1.5, 57 }, Seg { 2.4, 2.9, 62 }, Seg { 3.9, 4.6, 64 } })
        {
            auto stats = [&] (const std::vector<double>& tr, const std::vector<double>& tm, double& mean, double& sd)
            {
                double acc = 0, acc2 = 0; int n = 0;
                for (size_t i = 0; i < tr.size(); ++i)
                    if (tm[i] > sg.a && tm[i] < sg.b && tr[i] > 0)
                    {
                        const double c = 100.0 * (69.0 + 12.0 * std::log2 (tr[i] / 440.0) - sg.target);
                        acc += c; acc2 += c * c; ++n;
                    }
                mean = n ? acc / n : 999; sd = n ? std::sqrt (std::max (0.0, acc2 / n - mean * mean)) : 0;
            };
            double mi, si, mo, so;
            stats (pin, tIn, mi, si); stats (pout, tOut, mo, so);
            std::printf ("  note %d: in %+.1f c (vibrato sd %.1f) -> out %+.1f c (sd %.1f)\n", (int) sg.target, mi, si, mo, so);
            check (std::abs (mo) < 6.0, juce::String ("  corrected to within 6 cents @") + juce::String (fs));
            check (std::abs (so - si) < 0.25 * si + 2.0, "  vibrato depth kept (within 25%)");
        }
        std::printf ("  CPU %.2f%% of a core\n", cpu);
    }
    std::printf ("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
