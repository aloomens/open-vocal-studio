#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Base.h"
#include "DeepFilter.h"
#include "Pitch.h"
#include "Spectral.h"
#include "Dynamics.h"
#include "FilePlayer.h"
#include "Effects.h"

namespace IDs
{
    #define VS_ID(name) inline constexpr const char* name = #name;
    VS_ID (mode) VS_ID (inGain) VS_ID (rumble)
    VS_ID (denoise) VS_ID (deroom) VS_ID (deplosive)
    VS_ID (pitchOn) VS_ID (pitchStrength) VS_ID (pitchSpeed) VS_ID (pitchTolerance) VS_ID (key) VS_ID (scale)
    VS_ID (resonance) VS_ID (smooth) VS_ID (deess) VS_ID (breath)
    VS_ID (eqAmount) VS_ID (air)
    VS_ID (rider) VS_ID (comp)
    VS_ID (revType) VS_ID (revMix) VS_ID (revDecay) VS_ID (revPreDelay) VS_ID (revDuck)
    VS_ID (dlyOn) VS_ID (dlySync) VS_ID (dlyTime) VS_ID (dlyNote) VS_ID (dlyFeedback) VS_ID (dlyMix) VS_ID (dlyDuck) VS_ID (dlyWidth)
    VS_ID (doubler) VS_ID (outGain) VS_ID (compare)
    VS_ID (consonant)                                  // added in 1.2.4 (default 0: older projects sound the same)
    #undef VS_ID
    inline juce::String eq (int b) { return "eq" + juce::String (b); }
    inline juce::String note (int pc) { return "note" + juce::String (pc); }
}

class VocalStudioAudioProcessor : public juce::AudioProcessor,
                                  private juce::AudioProcessorValueTreeState::Listener,
                                  private juce::AsyncUpdater
{
public:
    VocalStudioAudioProcessor();
    ~VocalStudioAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return VS_PRODUCT_NAME; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 5.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    // ---- for the editor / tests ----
    bool isStandaloneApp() const { return wrapperType == wrapperType_Standalone || forceStandaloneUi; }
    static inline bool forceStandaloneUi = false;  // tests: show the standalone-only parts of the window
    void reconfigureNow();                       // apply Mode / pitch-on changes (message thread)
    int chainLatency() const { return latencyNow; }
    bool neuralActive() const { return dfnActive; }
    bool isMixMode() const { return mixMode; }

    void startAnalyze();
    juce::String stopAnalyzeAndApply();          // returns a short report for the status line
    bool isAnalyzing() const { return spectral.isAnalyzing(); }
    double analyzeSeconds() const { return spectral.analyzedSeconds(); }
    void detectedKey (int& key, bool& minor, float& confidence) const { keyChroma.detect (key, minor, confidence); }
    double keyHeardSeconds() const { return keyChroma.activeSeconds(); }   // music the key detector has heard (recent)
    bool detectedKeyUsable() const;              // a key detected with enough confidence to use
    bool applyDetectedKey();                     // sets Key and Scale; false (nothing changed) if none yet
    float currentMidi() const { return pitch.curMidi.load(); }
    float currentCorrection() const { return pitch.curCorrCents.load(); }
    int currentNote() const { return pitch.curNote.load(); }

    struct Meters
    {
        std::atomic<float> in { -100 }, out { -100 }, rider { 0 }, comp { 0 }, deess { 0 }, plosive { 0 }, consonant { 0 }, limiter { 0 }, matchDb { 0 };
    } meters;
    vs::SpectralStage spectral;                  // display curve read by the editor
    int editorWidth = 0, editorHeight = 0;

    // standalone file player: plays a loaded file into the chain instead of the live input
    vs::FilePlayer player;
    juce::String loadFileTake (const juce::File& f);   // message thread; returns an error, or empty

    static constexpr int kInternalBlock = 512;

private:
    void parameterChanged (const juce::String& id, float) override;
    void handleAsyncUpdate() override { reconfigureNow(); }
    void configure();
    void processChunk (float* const* io, int numIn, int numOut, int n);
    float p (const char* id) const { return apvts.getRawParameterValue (id)->load(); }
    std::array<std::atomic<float>*, vs::VocalEq::kBands> eqRaw {};
    std::array<std::atomic<float>*, 12> noteRaw {};

    double fs = 48000.0;
    int maxBlock = kInternalBlock;
    bool prepared = false, mixMode = true, pitchInChain = true, dfnActive = false;
    int latencyNow = 0;
    int currentProgram = 0;
    std::atomic<bool> configDirty { false };

    vs::DeepFilter dfn;
    vs::RumbleFilter rumble;
    vs::Deplosive deplosive;
    vs::PitchCorrector pitch;
    vs::ChromaAnalyzer keyChroma;            // key detection from the input spectrum (before the denoiser)
    double keyFs = 0.0;
    vs::VocalEq eq;
    vs::Rider rider;
    vs::VocalCompressor comp;
    vs::Doubler doubler;
    vs::SmoothDelay delay;
    vs::FdnReverb plate, room;
    vs::TruePeakLimiter limiter;
    std::array<vs::DelayLine, 2> dryDelay;       // A/B compare: the input, delayed by the latency

    std::vector<float> mono, wetL, wetR, dryL, dryR, dryLTmp, dryRTmp, roomL, roomR;
    vs::Glide inGainG, outGainG, compareG;
    float paramCoef = 0.01f;
    // loudness of input and output for the level-matched compare
    vs::Biquad kIn, kOut;
    float msIn = 0, msOut = 0, longIn = -100, longOut = -100, matchDb = 0;
    float msCoef = 0.001f, longCoef = 0.0001f;
    float meterIn = 0, meterOut = 0;
    double lastBpm = 120.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalStudioAudioProcessor)
};
