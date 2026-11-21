#pragma once

// Neural noise removal with DeepFilterNet3 (H. Schroeter et al., MIT / Apache-2.0).
//
// The model runs at 48 kHz on a 960-point STFT (hop 480, Vorbis window), exactly as in libDF:
// ERB-band and complex features with exponential mean normalization go in; ERB gains (for the
// upper spectrum) and a 5-tap complex "deep filter" per bin (for the lowest 96 bins, 0-4.8 kHz)
// come out. The network (DfnEngine) streams frame by frame on the audio thread: no extra
// threads, no allocation, and the same result for the same input every time.
//
// Signal path: host rate -> 48 kHz -> STFT -> features -> network -> enhanced spectrum -> ISTFT
// -> host rate. Latency is fixed and exactly known: one STFT window plus the model's 2-frame
// look-ahead (about 40 ms), plus the resampler's filter when the host isn't at 48 kHz.

#include "Base.h"
#include "Fft.h"
#include "Resampler.h"
#include "DfnEngine.h"

namespace vs
{
struct DfnStatus
{
    bool loaded = false;
    juce::String message;
};

class DeepFilter
{
public:
    static constexpr int kSr = 48000, kFft = 960, kHop = 480, kBins = kFft / 2 + 1;
    static constexpr int kErb = 32, kDf = 96, kOrder = 5, kLook = 2;

    // Builds the network (once per instance) and sizes everything. Not real-time safe.
    void prepare (double hostRate, int maxBlock);
    void reset();

    // Amount 0..1 (0 = untouched, 1 = full removal).
    void setAmount (float a) { amountT = std::clamp (a, 0.0f, 1.0f); }

    // Mono, in place, host rate. Output = input delayed by getLatency() samples, denoised.
    void process (float* x, int n);

    int getLatency() const { return latency; }
    bool isLoaded() const { return engine.isLoaded(); }
    static DfnStatus status();

    // Tests: the same path over a whole 48 kHz signal, time-aligned with the input.
    std::vector<float> processWholeForTest (const std::vector<float>& in48);
    // Tests: the features fed to the network for a 48 kHz signal ([frames][32], [frames][96 x re,im])
    void featuresForTest (const std::vector<float>& in48, std::vector<float>& erb, std::vector<float>& spec);
    // Tests: the network's raw outputs for those features, frame by frame
    void runNetworkForTest (const std::vector<float>& erb, const std::vector<float>& spec, std::vector<float>& mask, std::vector<float>& coefs);

    int underruns = 0;               // host FIFO ran dry (must stay 0; checked by the tests)

private:
    void process48 (const float* in, float* out, int n);
    void formFrame();
    void computeFeatures (const std::complex<float>* X);
    static std::vector<int> makeErbBands();

    DfnEngine engine;
    Fft fft;
    std::vector<float> window;
    std::vector<int> erbBands;

    // the last 5 spectra (frames k-2 .. k+2 around the frame being synthesized), newest last
    std::array<std::vector<std::complex<float>>, kOrder> specHist;
    std::array<float, kErb> erbState {};
    std::array<float, kDf> specState {};
    float alpha = 0.99f;
    std::array<float, kErb> fErb {}, mask {};
    std::array<float, kDf * 2> fSpec {};
    std::array<float, kDf * 10> coefs {};

    std::vector<float> inWin, frameBuf, ola, synthBuf;
    std::vector<std::complex<float>> outBins, cur;
    int hopFill = 0;
    int64_t frameCount = 0;
    std::vector<float> outFifo;
    int fifoMask = 0; int64_t fifoW = 0, fifoR = 0;

    // host-rate wrapper
    Resampler toModel, fromModel;
    std::vector<float> buf48in, buf48out, hostFifo, hostTmp;
    int hostMask = 0; int64_t hostW = 0, hostR = 0;
    int latency = 0, extra48 = 0, hostPrefill = 0;
    bool resampling = false;

    float amountT = 1.0f, amountCur = 1.0f;

    // Voice guard: the network was trained on speech and can mistake a long, steady sung note for
    // noise. A loud frame that is clearly a voice (far above the noise floor, or strongly tonal)
    // may lose only a few dB; quiet frames (gaps, noise) are cleaned fully.
    static constexpr int kFloorFrames = 500;     // noise floor = quietest frame of the last 5 s
    std::array<float, kFloorFrames> floorHist {};
    int floorPos = 0;
    float guardMix = 0.0f;

    // Silence (a stopped DAW): the state after the last frame that was all signal is kept, and the
    // audio that follows the silence continues from it, as if there had been no gap.
    struct Resume
    {
        DfnEngine::Snapshot net;
        std::array<float, kErb> erbState {};
        std::array<float, kDf> specState {};
        std::array<float, kFloorFrames> floorHist {};
        int floorPos = 0;
        float guardMix = 0.0f;
        bool valid = false;
    } resume;
    int64_t sinceZeros = 0;                      // 48 kHz samples since the last run of digital silence
    int zeroRun = 0;
    bool inGap = false;
};
} // namespace vs
