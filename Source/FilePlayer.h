#pragma once

// Standalone app only: plays a loaded audio file into the chain instead of the sound card's input.
// The whole file is held in memory (no disk access on the audio thread) at its own sample rate;
// a windowed-sinc interpolator converts it to the device's rate on the fly when they differ.
// In a DAW the plugin never turns this on.

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>

namespace vs
{
class FilePlayer
{
public:
    struct Take
    {
        juce::AudioBuffer<float> audio;     // 1 or 2 channels, at 'rate'
        double rate = 48000.0;
        juce::String name;
    };

    // message thread, with the audio callback locked: swap in a new take (the old one is returned
    // so it can be freed outside the lock)
    std::unique_ptr<Take> swapTake (std::unique_ptr<Take> t)
    {
        playing.store (false);
        take.swap (t);
        pos = 0; seekTo.store (-1);
        for (auto& i : interp) i.reset();
        posSeconds.store (0.0);
        return t;
    }
    const Take* getTake() const { return take.get(); }
    bool hasTake() const { return take != nullptr && take->audio.getNumSamples() > 0; }

    void prepare (double deviceRate) { devRate = deviceRate; for (auto& i : interp) i.reset(); }

    // any thread: requests, picked up by the audio thread
    std::atomic<bool> source { false }, playing { false }, loop { false };
    std::atomic<int64_t> seekTo { -1 };
    std::atomic<double> posSeconds { 0.0 };
    double lengthSeconds() const { return hasTake() ? take->audio.getNumSamples() / take->rate : 0.0; }

    // audio thread: replace the input channels with the file (silence while stopped)
    void render (float* const* io, int numCh, int n)
    {
        if (numCh <= 0) return;
        if (! hasTake())
        {
            for (int c = 0; c < numCh; ++c) juce::FloatVectorOperations::clear (io[c], n);
            return;
        }
        const auto len = (int64_t) take->audio.getNumSamples();
        if (const auto s = seekTo.exchange (-1); s >= 0) { pos = std::min<int64_t> (s, len); for (auto& i : interp) i.reset(); }
        int done = 0;
        while (done < n)
        {
            if (! playing.load() || pos >= len)
            {
                for (int c = 0; c < numCh; ++c) juce::FloatVectorOperations::clear (io[c] + done, n - done);
                break;
            }
            const int want = n - done;
            const int srcCh = take->audio.getNumChannels();
            int used = 0, made = want;
            if (std::abs (take->rate - devRate) < 1e-6)
            {
                made = (int) std::min<int64_t> (want, len - pos);
                for (int c = 0; c < std::min (numCh, 2); ++c)
                    juce::FloatVectorOperations::copy (io[c] + done, take->audio.getReadPointer (std::min (c, srcCh - 1), (int) pos), made);
                used = made;
            }
            else
            {
                const double ratio = take->rate / devRate;         // file samples per output sample
                const int avail = (int) std::min<int64_t> (len - pos, std::numeric_limits<int>::max());
                // produce no more than the remaining input supports (the interpolator would pad zeros)
                made = std::max (1, std::min (want, (int) std::floor ((avail - 1) / ratio)));
                for (int c = 0; c < std::min (numCh, 2); ++c)
                    used = interp[(size_t) c].process (ratio, take->audio.getReadPointer (std::min (c, srcCh - 1), (int) pos),
                                                       io[c] + done, made, avail, 0);
            }
            for (int c = 2; c < numCh; ++c) juce::FloatVectorOperations::clear (io[c] + done, made);
            pos += std::max (1, used);
            done += made;
            if (pos >= len)
            {
                if (loop.load()) { pos = 0; for (auto& i : interp) i.reset(); }
                else playing.store (false);
            }
        }
        posSeconds.store ((double) pos / take->rate);
    }

private:
    std::unique_ptr<Take> take;
    int64_t pos = 0;
    double devRate = 48000.0;
    std::array<juce::WindowedSincInterpolator, 2> interp;
};
} // namespace vs
