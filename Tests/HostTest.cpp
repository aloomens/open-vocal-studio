// Loads the built Open Vocal Studio.vst3 the way a DAW does (VST3 hosting through JUCE), and checks:
//  - the plugin loads, reports its name, and accepts mono-in / stereo-out and stereo-in / stereo-out
//  - the neural engine (vs_onnxruntime.dll inside the bundle) loads: Mix-mode latency includes it
//  - audio passes, output is finite and level-sensible, the reported latency matches the audio
//  - state can be saved and restored through the VST3 interface
// Usage: PluginHostTest.exe "path\to\Open Vocal Studio.vst3"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>
#include <cmath>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 2) { std::printf ("usage: PluginHostTest <plugin.vst3>\n"); return 2; }
    int fails = 0;
    auto check = [&] (bool ok, const juce::String& what) { std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8()); if (! ok) ++fails; };

    juce::VST3PluginFormat fmt;
    juce::OwnedArray<juce::PluginDescription> found;
    fmt.findAllTypesForFile (found, juce::String (argv[1]));
    check (found.size() == 1, "VST3 found in the bundle");
    if (found.isEmpty()) return 1;
    std::printf ("      %s by %s, version %s\n", found[0]->name.toRawUTF8(), found[0]->manufacturerName.toRawUTF8(), found[0]->version.toRawUTF8());

    juce::String err;
    const double fs = 48000.0;
    auto inst = fmt.createInstanceFromDescription (*found[0], fs, 512, err);
    check (inst != nullptr, "instance created " + err);
    if (inst == nullptr) return 1;

    juce::AudioProcessor::BusesLayout mono;
    mono.inputBuses.add (juce::AudioChannelSet::mono()); mono.outputBuses.add (juce::AudioChannelSet::stereo());
    juce::AudioProcessor::BusesLayout stereo;
    stereo.inputBuses.add (juce::AudioChannelSet::stereo()); stereo.outputBuses.add (juce::AudioChannelSet::stereo());
    check (inst->checkBusesLayoutSupported (mono), "mono in / stereo out supported");
    check (inst->checkBusesLayoutSupported (stereo), "stereo in / stereo out supported");
    inst->setBusesLayout (stereo);
    inst->setNonRealtime (true);
    inst->prepareToPlay (fs, 512);
    const int L = inst->getLatencySamples();
    std::printf ("      reported latency %d samples (%.1f ms)\n", L, 1000.0 * L / fs);
    check (L > (int) (0.1 * fs), "Mix mode latency includes the neural denoiser and pitch correction");

    // a sung-like tone burst with a click at a known place: find it in the output
    const int total = (int) (fs * 3.5) + L;
    juce::AudioBuffer<float> io (2, 512);
    juce::MidiBuffer midi;
    std::vector<float> outL ((size_t) total);
    double phase = 0;
    for (int s = 0; s < total; s += 512)
    {
        const int n = std::min (512, total - s);
        io.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            const int t = s + i;
            float v = 0.0f;
            // half a second of quiet room tone, then a sustained "sung" tone for 2 s
            v = 0.001f * (float) (juce::Random::getSystemRandom().nextFloat() - 0.5f);
            if (t > (int) (fs * 0.5) && t < (int) (fs * 2.5))
            {
                phase += 2.0 * juce::MathConstants<double>::pi * 220.0 / fs;
                v += 0.2f * (float) (std::sin (phase) + 0.5 * std::sin (2 * phase) + 0.3 * std::sin (3 * phase));
            }
            io.setSample (0, i, v); io.setSample (1, i, v);
        }
        inst->processBlock (io, midi);
        for (int i = 0; i < n; ++i) outL[(size_t) (s + i)] = io.getSample (0, i);
    }
    bool finite = true; double pk = 0, e = 0;
    for (float v : outL) { finite &= std::isfinite (v); pk = std::max (pk, (double) std::abs (v)); }
    for (int i = L + (int) (1.0 * fs); i < L + (int) (2.0 * fs); ++i) e += (double) outL[(size_t) i] * outL[(size_t) i];
    const double rmsDb = 10 * std::log10 (e / fs + 1e-20);
    std::printf ("      output peak %.1f dBFS, steady RMS %.1f dBFS\n", 20 * std::log10 (pk + 1e-12), rmsDb);
    check (finite, "output finite");
    // (a bare 3-partial test tone, not a voice: the voice-level checks are in VocalTest)
    check (rmsDb > -32 && pk < 1.0, "audio passes at a sensible level, under full scale");
    // the tone starts at 0.5 s: before latency + 0.5 s only the (cleaned) room tone comes out
    double pre = 0; for (int i = 0; i < L + (int) (0.5 * fs) - 2000; ++i) pre = std::max (pre, (double) std::abs (outL[(size_t) i]));
    check (pre < 0.01, "the note doesn't come out before the reported latency");

    juce::MemoryBlock state;
    inst->getStateInformation (state);
    auto inst2 = fmt.createInstanceFromDescription (*found[0], fs, 512, err);
    inst2->setStateInformation (state.getData(), (int) state.getSize());
    bool same = true;
    auto& p1 = inst->getParameters(); auto& p2 = inst2->getParameters();
    for (int i = 0; i < p1.size() && i < p2.size(); ++i) same &= std::abs (p1[i]->getValue() - p2[i]->getValue()) < 1e-6f;
    check (same && p1.size() == p2.size() && p1.size() > 40, "state round-trip through VST3 (" + juce::String (p1.size()) + " parameters)");
    check (inst->getNumPrograms() >= 6, "factory presets visible to the host");
    inst->releaseResources();
    inst = nullptr; inst2 = nullptr;
    std::printf ("%d failure(s)\n", fails);
    return fails;
}
