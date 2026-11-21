#pragma once

// Standalone "SAVE PROCESSED": the loaded file through a second copy of the chain with the current
// settings, at the file's own sample rate (no resampling), with the latency trimmed off so the
// result lines up with the original, plus the reverb / delay tail. Written as 24-bit WAV.
// The copy is set up and torn down on the message thread; the processing runs on a worker thread,
// so live monitoring through the app keeps running meanwhile.

#include "PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <thread>

class OfflineRender
{
public:
    static constexpr int kBlock = 512;

    // message thread
    OfflineRender (VocalStudioAudioProcessor& live, const vs::FilePlayer::Take& take, const juce::File& destination)
        : in (take.audio), rate (take.rate), dest (destination)
    {
        juce::MemoryBlock state;
        live.getStateInformation (state);
        copy = std::make_unique<VocalStudioAudioProcessor>();
        copy->setStateInformation (state.getData(), (int) state.getSize());
        if (auto* c = copy->apvts.getParameter (IDs::compare)) c->setValueNotifyingHost (0.0f);   // never the A/B dry signal
        copy->setNonRealtime (true);
        copy->setRateAndBufferSizeDetails (rate, kBlock);
        copy->prepareToPlay (rate, kBlock);
        const bool fx = copy->apvts.getRawParameterValue (IDs::revType)->load() > 0.5f
                        || copy->apvts.getRawParameterValue (IDs::dlyOn)->load() > 0.5f;
        tail = (int) std::lround ((fx ? 4.0 : 0.0) * rate);
    }
    ~OfflineRender() { cancel.store (true); if (worker.joinable()) worker.join(); }

    void start() { worker = std::thread ([this] { run(); }); }

    // the work itself (any thread; tests call it directly)
    void run()
    {
        const int len = in.getNumSamples(), inCh = in.getNumChannels();
        const int lat = copy->getLatencySamples();
        const int total = len + lat + tail;
        result.setSize (2, len + tail);
        result.clear();
        juce::AudioBuffer<float> b (2, kBlock);
        juce::MidiBuffer midi;
        for (int s = 0; s < total && ! cancel.load(); s += kBlock)
        {
            const int n = std::min (kBlock, total - s);
            b.setSize (2, n, false, false, true);
            for (int c = 0; c < 2; ++c)
            {
                const float* src = in.getReadPointer (std::min (c, inCh - 1));
                float* d = b.getWritePointer (c);
                for (int i = 0; i < n; ++i) d[i] = s + i < len ? src[s + i] : 0.0f;
            }
            copy->processBlock (b, midi);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    if (const int o = s + i - lat; o >= 0 && o < result.getNumSamples()) result.setSample (c, o, b.getSample (c, i));
            progress.store ((float) (s + n) / (float) total);
        }
        if (cancel.load()) error = "Cancelled.";
        else if (dest != juce::File())
        {
            dest.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (dest.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (os != nullptr ? wav.createWriterFor (os.get(), rate, 2, 24, {}, 0) : nullptr);
            if (w == nullptr) error = "Can't write " + dest.getFullPathName();
            else
            {
                os.release();                          // the writer owns the stream now
                if (! w->writeFromAudioSampleBuffer (result, 0, result.getNumSamples())) error = "Writing " + dest.getFileName() + " failed.";
            }
        }
        done.store (true);
    }

    std::atomic<float> progress { 0.0f };
    std::atomic<bool> done { false };
    juce::String error;                    // set before 'done'
    juce::AudioBuffer<float> result;       // stereo, the original's length plus the tail
    const juce::File& destination() const { return dest; }
    int latencyTrimmed() const { return copy->getLatencySamples(); }

private:
    juce::AudioBuffer<float> in;
    double rate;
    juce::File dest;
    int tail = 0;
    std::unique_ptr<VocalStudioAudioProcessor> copy;
    std::atomic<bool> cancel { false };
    std::thread worker;
};
