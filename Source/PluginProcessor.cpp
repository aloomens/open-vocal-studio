#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>

using namespace juce;

namespace
{
const StringArray kModes { "Mix (best quality)", "Track (low latency)" };
const StringArray kKeys { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
const StringArray kRevTypes { "Off", "Plate", "Room" };
const StringArray kNotes { "1/64", "1/32T", "1/32", "1/16T", "1/32D", "1/16", "1/8T", "1/16D", "1/8" };
constexpr double kNoteBeats[] = { 0.0625, 1.0 / 12.0, 0.125, 1.0 / 6.0, 0.1875, 0.25, 1.0 / 3.0, 0.375, 0.5 };

StringArray scaleNames()
{
    StringArray s;
    for (int i = 0; i < vs::Scales::kCount; ++i) s.add (vs::Scales::name (i));
    return s;
}

struct Preset { const char* name; std::vector<std::pair<String, double>> values; };

const std::vector<Preset>& presets()
{
    static const std::vector<Preset> p {
        { "Natural Polish", {} },
        { "Pop Lead", { { IDs::comp, 55 }, { IDs::air, 3.0f }, { IDs::smooth, 40 }, { IDs::resonance, 50 }, { IDs::deess, 50 },
                        { IDs::revMix, 20 }, { IDs::revDecay, 1.8f }, { IDs::dlyOn, 1 }, { IDs::dlySync, 1 }, { IDs::dlyNote, 5 },
                        { IDs::dlyMix, 12 }, { IDs::dlyFeedback, 18 }, { IDs::pitchStrength, 80 }, { IDs::pitchSpeed, 45 } } },
        { "Intimate Ballad", { { IDs::comp, 30 }, { IDs::rider, 6 }, { IDs::revMix, 26 }, { IDs::revDecay, 2.6f }, { IDs::revPreDelay, 40 },
                               { IDs::deess, 50 }, { IDs::breath, 20 }, { IDs::pitchStrength, 55 }, { IDs::pitchSpeed, 20 }, { IDs::air, 2.0f } } },
        { "Rock Lead", { { IDs::comp, 70 }, { IDs::smooth, 45 }, { IDs::resonance, 55 }, { IDs::revType, 2 }, { IDs::revMix, 14 },
                         { IDs::revDecay, 0.9f }, { IDs::dlyOn, 1 }, { IDs::dlySync, 0 }, { IDs::dlyTime, 115 }, { IDs::dlyMix, 16 },
                         { IDs::dlyFeedback, 10 }, { IDs::doubler, 25 }, { IDs::pitchStrength, 60 } } },
        { "Spoken Word", { { IDs::pitchOn, 0 }, { IDs::revType, 0 }, { IDs::denoise, 80 }, { IDs::deroom, 40 }, { IDs::comp, 50 },
                           { IDs::breath, 35 }, { IDs::rider, 6 }, { IDs::air, 1.0f }, { IDs::deess, 45 } } },
        { "Tracking (Low Latency)", { { IDs::mode, 1 }, { IDs::pitchOn, 0 }, { IDs::revMix, 15 }, { IDs::revDecay, 1.2f } } },
    };
    return p;
}
} // namespace

//==============================================================================
AudioProcessorValueTreeState::ParameterLayout VocalStudioAudioProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout l;
    // values carry their unit in the text ("60 %", "+1.5 dB"), as in Clean Tone Studio and Open Mastering
    // Studio: the knobs and the DAW show the same thing (typing "60" or "60 %" both work)
    auto unitText = [] (const String& unit) -> std::function<String (float, int)>
    {
        if (unit == "%")  return [] (float v, int) { return String (roundToInt (v)) + " %"; };
        if (unit == "dB") return [] (float v, int) { return (v > 0.05f ? "+" : "") + String (std::abs (v) < 0.05f ? 0.0f : v, 1) + " dB"; };
        if (unit == "Hz") return [] (float v, int) { return v < 1000.0f ? String (roundToInt (v)) + " Hz" : String (v / 1000.0f, 1) + " kHz"; };
        if (unit == "ms") return [] (float v, int) { return String (roundToInt (v)) + " ms"; };          // all whole ms here
        if (unit == "s")  return [] (float v, int) { return String (v, 1) + " s"; };
        return [unit] (float v, int) { return String (v, 1) + " " + unit; };
    };
    auto pct = [unitText] (const char* id, const char* name, float def, int version = 1)
    {
        return std::make_unique<AudioParameterFloat> (ParameterID { id, version }, name, NormalisableRange<float> (0.0f, 100.0f, 0.1f), def,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction (unitText ("%")));
    };
    auto fl = [unitText] (const String& id, const String& name, float lo, float hi, float step, float def, const char* unit, float skewMid = 0.0f)
    {
        NormalisableRange<float> r (lo, hi, step);
        if (skewMid > 0.0f) r.setSkewForCentre (skewMid);
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction (unitText (unit)));
    };
    auto ch = [] (const char* id, const char* name, const StringArray& c, int def)
    { return std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, c, def); };
    auto bo = [] (const String& id, const String& name, bool def)
    { return std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def); };

    l.add (ch (IDs::mode, "Mode", kModes, 0));
    l.add (fl (IDs::inGain, "Input", -24, 24, 0.1f, 0, "dB"));
    l.add (fl (IDs::rumble, "Rumble Filter", 20, 300, 1, 80, "Hz", 80));
    l.add (pct (IDs::denoise, "Denoise", 60));
    l.add (pct (IDs::deroom, "De-room", 0));
    l.add (pct (IDs::deplosive, "De-plosive", 50));
    l.add (bo (IDs::pitchOn, "Pitch Correction", true));
    l.add (pct (IDs::pitchStrength, "Pitch Strength", 85));      // 1.3.1: the most natural (VocalTest --diag-defaults)
    l.add (pct (IDs::pitchSpeed, "Pitch Speed", 45));
    l.add (fl (IDs::pitchTolerance, "Pitch Tolerance", 0, 50, 0.5f, 5, "cents"));
    l.add (ch (IDs::key, "Key", kKeys, 0));
    l.add (ch (IDs::scale, "Scale", scaleNames(), 0));
    for (int pc = 0; pc < 12; ++pc) l.add (bo (IDs::note (pc), String ("Note ") + kKeys[pc], true));
    l.add (pct (IDs::resonance, "Resonance", 40));
    l.add (pct (IDs::smooth, "Smooth", 30));
    l.add (pct (IDs::deess, "De-ess", 40));
    l.add (pct (IDs::breath, "Breath", 0));
    l.add (pct (IDs::eqAmount, "EQ Amount", 100));
    for (int b = 0; b < vs::VocalEq::kBands; ++b)
    {
        const float f = vs::VocalEq::kFreqs[b];
        l.add (fl (IDs::eq (b), "EQ " + (f >= 1000 ? String (f / 1000.0f, 1) + " kHz" : String ((int) f) + " Hz"), -12, 12, 0.1f, 0, "dB"));
    }
    l.add (fl (IDs::air, "Air", 0, 6, 0.1f, 1.5f, "dB"));
    l.add (fl (IDs::rider, "Rider", 0, 12, 0.1f, 4, "dB"));
    l.add (pct (IDs::comp, "Compress", 40));
    l.add (ch (IDs::revType, "Reverb", kRevTypes, 1));
    l.add (pct (IDs::revMix, "Reverb Mix", 18));
    l.add (fl (IDs::revDecay, "Reverb Decay", 0.3f, 4.0f, 0.01f, 1.6f, "s", 1.4f));
    l.add (fl (IDs::revPreDelay, "Pre-delay", 0, 120, 1, 25, "ms"));
    l.add (pct (IDs::revDuck, "Reverb Duck", 40));
    l.add (bo (IDs::dlyOn, "Delay", false));
    l.add (bo (IDs::dlySync, "Delay Sync", true));
    l.add (fl (IDs::dlyTime, "Delay Time", 60, 150, 1, 110, "ms"));
    l.add (ch (IDs::dlyNote, "Delay Note", kNotes, 5));
    l.add (pct (IDs::dlyFeedback, "Delay Feedback", 15));
    l.add (pct (IDs::dlyMix, "Delay Mix", 12));
    l.add (pct (IDs::dlyDuck, "Delay Duck", 50));
    l.add (pct (IDs::dlyWidth, "Delay Width", 50));
    l.add (pct (IDs::doubler, "Doubler", 0));
    l.add (fl (IDs::outGain, "Output", -24, 12, 0.1f, 0, "dB"));
    l.add (bo (IDs::compare, "Compare (A/B)", false));
    // added in 1.2.4 at the end of the list (hosts key automation by ID; the order is cosmetic)
    l.add (pct (IDs::consonant, "Consonant", 0, 2));
    return l;
}

VocalStudioAudioProcessor::VocalStudioAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VocalStudio", createLayout())
{
    apvts.addParameterListener (IDs::mode, this);
    apvts.addParameterListener (IDs::pitchOn, this);
    // looked up once: building their names in processBlock would allocate on the audio thread
    for (int b = 0; b < vs::VocalEq::kBands; ++b) eqRaw[(size_t) b] = apvts.getRawParameterValue (IDs::eq (b));
    for (int pc = 0; pc < 12; ++pc) noteRaw[(size_t) pc] = apvts.getRawParameterValue (IDs::note (pc));
}

VocalStudioAudioProcessor::~VocalStudioAudioProcessor()
{
    cancelPendingUpdate();
    apvts.removeParameterListener (IDs::mode, this);
    apvts.removeParameterListener (IDs::pitchOn, this);
}

bool VocalStudioAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    const bool inOk = in == AudioChannelSet::mono() || in == AudioChannelSet::stereo();
    const bool outOk = out == AudioChannelSet::mono() || out == AudioChannelSet::stereo();
    return inOk && outOk;
}

//==============================================================================
void VocalStudioAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    fs = sampleRate;
    maxBlock = kInternalBlock;
    juce::ignoreUnused (samplesPerBlock);
    mono.assign ((size_t) maxBlock, 0.0f);
    wetL.assign ((size_t) maxBlock, 0.0f); wetR = wetL; dryL = wetL; dryR = wetL;
    for (auto* v : { &dryLTmp, &dryRTmp, &roomL, &roomR }) v->reserve ((size_t) maxBlock);   // assign() below never allocates
    paramCoef = vs::onePoleCoef (12.0, fs);
    kIn.setHighShelf (fs, 1500.0, 4.0); kOut = kIn;
    msCoef = vs::onePoleCoef (400.0, fs);
    longCoef = vs::onePoleCoef (3000.0, fs);
    configure();
    player.prepare (sampleRate);
    prepared = true;
}

void VocalStudioAudioProcessor::configure()
{
    mixMode = p (IDs::mode) < 0.5f;
    const bool pitchOn = p (IDs::pitchOn) > 0.5f;
    if (mixMode)
    {
        dfn.prepare (fs, maxBlock);
        dfnActive = dfn.isLoaded();
    }
    else dfnActive = false;
    rumble.prepare (fs);
    deplosive.prepare (fs, mixMode ? (int) std::lround (0.003 * fs) : 0);
    pitch.prepare (fs, maxBlock, mixMode ? 60.0 : 100.0);
    pitchInChain = mixMode || pitchOn;
    if (fs != keyFs) { keyChroma.prepare (fs); keyFs = fs; }      // (a mode switch keeps what it has heard)
    spectral.prepare (fs, ! mixMode);
    eq.prepare (fs);
    rider.prepare (fs);
    comp.prepare (fs);
    doubler.prepare (fs);
    delay.prepare (fs);
    plate.prepare (fs); room.prepare (fs);
    plate.setVoicing (0.0f, 0.5);
    room.setVoicing (0.45f, 0.3);
    limiter.prepare (fs, (int) std::lround ((mixMode ? 0.0015 : 0.0005) * fs));
    limiter.setCeilingDb (-1.0f);

    latencyNow = (dfnActive ? dfn.getLatency() : 0) + deplosive.getLatency()
                 + (pitchInChain ? pitch.getLatency() : 0) + spectral.getLatency() + limiter.getLatency();
    for (auto& d : dryDelay) { d.prepare (latencyNow + 8); d.setDelay (latencyNow); d.clear(); }
    inGainG.snap (vs::dbToGain (p (IDs::inGain)));
    outGainG.snap (vs::dbToGain (p (IDs::outGain)));
    compareG.snap (p (IDs::compare) > 0.5f ? 1.0f : 0.0f);
    msIn = msOut = 0; longIn = longOut = -100; matchDb = 0;
    setLatencySamples (latencyNow);
    configDirty = false;
}

void VocalStudioAudioProcessor::releaseResources() {}

void VocalStudioAudioProcessor::parameterChanged (const String&, float)
{
    configDirty = true;
    triggerAsyncUpdate();
}

void VocalStudioAudioProcessor::reconfigureNow()
{
    if (! prepared) return;
    const bool wantMix = p (IDs::mode) < 0.5f;
    const bool wantPitch = wantMix || p (IDs::pitchOn) > 0.5f;
    if (wantMix == mixMode && wantPitch == pitchInChain) { configDirty = false; return; }
    suspendProcessing (true);
    {
        const ScopedLock sl (getCallbackLock());
        configure();
    }
    suspendProcessing (false);
}

//==============================================================================
void VocalStudioAudioProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int numIn = getTotalNumInputChannels(), numOut = getTotalNumOutputChannels();
    const int n = buffer.getNumSamples();
    for (int c = numIn; c < numOut; ++c) buffer.clear (c, 0, n);
    if (! prepared) return;
    // standalone app, FILE source: the loaded file replaces the sound card input
    if (player.source.load (std::memory_order_relaxed)) player.render (buffer.getArrayOfWritePointers(), numIn, n);

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm()) lastBpm = jlimit (20.0, 400.0, *bpm);

    // module settings (once per host block)
    const float denoise = p (IDs::denoise) * 0.01f;
    dfn.setAmount (denoise);
    vs::SpectralStage::Params sp;
    sp.noise = dfnActive ? 0.0f : denoise;
    sp.deroom = p (IDs::deroom) * 0.01f;
    sp.resonance = p (IDs::resonance) * 0.01f;
    sp.smooth = p (IDs::smooth) * 0.01f;
    sp.deess = p (IDs::deess) * 0.01f;
    sp.breath = p (IDs::breath) * 0.01f;
    spectral.setParams (sp);
    rumble.setCutoff (p (IDs::rumble));
    deplosive.setAmount (p (IDs::deplosive) * 0.01f);
    deplosive.setConsonant (p (IDs::consonant) * 0.01f);

    vs::PitchCorrector::Params pp;
    const bool pitchOn = p (IDs::pitchOn) > 0.5f;
    pp.strength = pitchOn ? p (IDs::pitchStrength) * 0.01f : 0.0f;
    pp.speed = p (IDs::pitchSpeed) * 0.01f;
    pp.toleranceCents = p (IDs::pitchTolerance);
    pp.key = (int) p (IDs::key);
    pp.scale = (int) p (IDs::scale);
    int mask = 0;
    for (int pc = 0; pc < 12; ++pc) if (noteRaw[(size_t) pc]->load() > 0.5f) mask |= 1 << pc;
    pp.noteMask = mask;
    pitch.setParams (pp);
    pitch.setEnabled (pitchOn);

    std::array<float, vs::VocalEq::kBands> g {};
    for (int b = 0; b < vs::VocalEq::kBands; ++b) g[(size_t) b] = eqRaw[(size_t) b]->load();
    eq.setGains (g, p (IDs::eqAmount) * 0.01f, p (IDs::air));
    rider.setRange (p (IDs::rider));
    comp.setAmount (p (IDs::comp) * 0.01f);

    const int rt = (int) p (IDs::revType);
    plate.setParams (0.25f, p (IDs::revDecay), p (IDs::revPreDelay), 7000.0f, 220.0f, p (IDs::revMix) * 0.01f, rt == 1, p (IDs::revDuck) * 0.01f);
    room.setParams (0.12f, p (IDs::revDecay) * 0.6f, p (IDs::revPreDelay) * 0.5f, 4500.0f, 180.0f, p (IDs::revMix) * 0.01f, rt == 2, p (IDs::revDuck) * 0.01f);
    const double beats = kNoteBeats[jlimit (0, 8, (int) p (IDs::dlyNote))];
    const float dlyMs = p (IDs::dlySync) > 0.5f ? (float) (60000.0 / lastBpm * beats) : p (IDs::dlyTime);
    delay.setParams (dlyMs, p (IDs::dlyFeedback) * 0.01f, 5500.0f, 0.08f, p (IDs::dlyDuck) * 0.01f,
                     p (IDs::dlyWidth) * 0.01f, p (IDs::dlyMix) * 0.01f, p (IDs::dlyOn) > 0.5f, false);
    doubler.setLevel (p (IDs::doubler) * 0.01f);

    for (int pos = 0; pos < n; pos += maxBlock)
    {
        const int m = jmin (maxBlock, n - pos);
        float* io[2] = { buffer.getWritePointer (0, pos), numOut > 1 ? buffer.getWritePointer (1, pos) : nullptr };
        processChunk (io, numIn, numOut, m);
    }
}

void VocalStudioAudioProcessor::processChunk (float* const* io, int numIn, int numOut, int n)
{
    float* L = io[0];
    float* R = numOut > 1 ? io[1] : nullptr;
    const float inGainT = vs::dbToGain (p (IDs::inGain)), outGainT = vs::dbToGain (p (IDs::outGain));
    const float compareT = p (IDs::compare) > 0.5f ? 1.0f : 0.0f;

    // input: mono sum (a vocal is a mono source), plus the delayed original for Compare
    float peakIn = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float l = L[i], r = (numIn > 1 && R != nullptr) ? R[i] : l;
        dryL[(size_t) i] = dryDelay[0].process (l);
        dryR[(size_t) i] = dryDelay[1].process (r);
        const float gIn = inGainG.next (inGainT, paramCoef);
        const float m = 0.5f * (l + r) * gIn;
        mono[(size_t) i] = m;
        peakIn = std::max (peakIn, std::abs (m));
        const float k = kIn.process (0.5f * (l + r), 0);
        msIn += msCoef * (k * k - msIn);
    }
    meters.in.store (vs::gainToDb (peakIn));

    float* x = mono.data();
    // key detection hears the input before any cleanup (the speech-trained denoiser would take most of
    // a full mix away). Outside ANALYZE it forgets with a ~60 s time constant, so it follows the song.
    keyChroma.push (x, n);
    if (! spectral.isAnalyzing()) keyChroma.decay (std::exp (-(double) n / (60.0 * fs)));
    if (dfnActive) dfn.process (x, n);
    deplosive.process (x, n);
    if (pitchInChain) pitch.process (x, n);
    rumble.process (x, n);          // after pitch correction: also clears any DC its grains leave
    spectral.process (x, n);
    eq.process (x, n);
    rider.process (x, n);
    comp.process (x, n);

    // stereo: dry voice in the center, then doubler, delay and reverb around it
    for (int i = 0; i < n; ++i) { wetL[(size_t) i] = x[i]; wetR[(size_t) i] = x[i]; }
    float* oL = wetL.data();
    float* oR = wetR.data();
    doubler.process (x, oL, oR, n);
    delay.process (oL, oR, n);
    // plate and room each work on the same input; their tails are added
    std::vector<float>& tL = dryLTmp, & tR = dryRTmp;
    tL.assign (oL, oL + n); tR.assign (oR, oR + n);
    plate.process (oL, oR, n);
    {
        std::vector<float>& rL = roomL, & rR = roomR;
        rL.assign (tL.begin(), tL.end()); rR.assign (tR.begin(), tR.end());
        room.process (rL.data(), rR.data(), n);
        for (int i = 0; i < n; ++i) { oL[i] += rL[(size_t) i] - tL[(size_t) i]; oR[i] += rR[(size_t) i] - tR[(size_t) i]; }
    }
    for (int i = 0; i < n; ++i)
    {
        const float go = outGainG.next (outGainT, paramCoef);
        oL[i] *= go; oR[i] *= go;
    }
    limiter.process (oL, oR, n);

    // output loudness, level-matched Compare, meters
    float peakOut = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float k = kOut.process (0.5f * (oL[i] + oR[i]), 0);
        msOut += msCoef * (k * k - msOut);
    }
    const float stIn = 10.0f * std::log10 (msIn + 1e-12f), stOut = 10.0f * std::log10 (msOut + 1e-12f);
    const float blockCoef = 1.0f - std::pow (1.0f - longCoef, (float) n);
    if (stIn > -55.0f) longIn += (longIn < -90.0f ? 1.0f : blockCoef) * (stIn - longIn);
    if (stOut > -55.0f) longOut += (longOut < -90.0f ? 1.0f : blockCoef) * (stOut - longOut);
    if (longIn > -90.0f && longOut > -90.0f) matchDb = jlimit (-24.0f, 24.0f, longOut - longIn);
    meters.matchDb.store (matchDb);
    const float matchG = vs::dbToGain (matchDb);
    for (int i = 0; i < n; ++i)
    {
        const float c = compareG.next (compareT, paramCoef);
        float l = oL[i], r = oR[i];
        if (c > 0.0f)
        {
            l += c * (dryL[(size_t) i] * matchG - l);
            r += c * (dryR[(size_t) i] * matchG - r);
        }
        if (R != nullptr) { L[i] = l; R[i] = r; }
        else L[i] = 0.5f * (l + r);
        peakOut = std::max (peakOut, std::max (std::abs (l), std::abs (r)));
    }
    meters.out.store (vs::gainToDb (peakOut));
    meters.rider.store (rider.gainDb.load());
    meters.comp.store (comp.grDb.load());
    meters.deess.store (spectral.deessDb.load());
    meters.plosive.store (deplosive.grDb.load());
    meters.consonant.store (deplosive.consGrDb.load());
    meters.limiter.store (limiter.grDb.load());
}

//==============================================================================
void VocalStudioAudioProcessor::startAnalyze()
{
    keyChroma.requestClear();
    spectral.setAnalyzing (true);
}

String VocalStudioAudioProcessor::stopAnalyzeAndApply()
{
    const double secs = spectral.analyzedSeconds();
    const auto ltas = spectral.getLtas();
    spectral.setAnalyzing (false);
    if (secs < 4.0)
        return "Analyze needs at least 4 seconds of singing (got " + String (secs, 1) + " s). Nothing changed.";
    const auto gains = vs::VocalEq::matchFromLtas (fs, ltas);
    for (int b = 0; b < vs::VocalEq::kBands; ++b)
        if (auto* prm = apvts.getParameter (IDs::eq (b))) prm->setValueNotifyingHost (prm->convertTo0to1 (gains[(size_t) b]));
    String msg = "EQ matched from " + String (secs, 0) + " s of voice";
    int key; bool minor; float conf;
    keyChroma.detect (key, minor, conf);
    if (detectedKeyUsable())
    {
        applyDetectedKey();
        msg << "; key " << kKeys[key] << (minor ? " minor" : " major");
    }
    else msg << "; key unclear, scale left as it was";
    return msg + ".";
}

bool VocalStudioAudioProcessor::detectedKeyUsable() const
{
    // at least 4 s of music, and not a near-tie: the key ahead of the runner-up by 0.015 in correlation
    // (confidence 0.15). VocalTest --diag-key: when right the lead is typically 0.2, a sparse melody such
    // as "Mary Had a Little Lamb" about 0.02; songs it gets wrong lead by at most about 0.06.
    int key; bool minor; float conf;
    keyChroma.detect (key, minor, conf);
    return keyChroma.activeSeconds() > 4.0 && conf >= 0.15f;
}

bool VocalStudioAudioProcessor::applyDetectedKey()
{
    // nothing detected yet (or too unsure): leave Key and Scale alone - an empty detector would
    // otherwise report C major
    if (! detectedKeyUsable()) return false;
    int key; bool minor; float conf;
    keyChroma.detect (key, minor, conf);
    if (auto* k = apvts.getParameter (IDs::key)) k->setValueNotifyingHost (k->convertTo0to1 ((float) key));
    if (auto* s = apvts.getParameter (IDs::scale)) s->setValueNotifyingHost (s->convertTo0to1 (minor ? 2.0f : 1.0f));
    return true;
}

String VocalStudioAudioProcessor::loadFileTake (const File& f)
{
    AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) return "Can't read " + f.getFileName() + " (WAV, AIFF, FLAC, MP3 or Ogg).";
    if (r->lengthInSamples <= 0 || r->sampleRate <= 0) return f.getFileName() + " has no audio.";
    if (r->lengthInSamples > (int64) (r->sampleRate * 60.0 * 30.0)) return "Files up to 30 minutes long, please.";
    auto t = std::make_unique<vs::FilePlayer::Take>();
    const int ch = jlimit (1, 2, (int) r->numChannels);
    t->audio.setSize (ch, (int) r->lengthInSamples);
    r->read (&t->audio, 0, (int) r->lengthInSamples, 0, true, ch > 1);
    t->rate = r->sampleRate;
    t->name = f.getFileName();
    std::unique_ptr<vs::FilePlayer::Take> old;
    {
        const ScopedLock sl (getCallbackLock());        // the audio callback holds this while it runs
        old = player.swapTake (std::move (t));
    }
    return {};                                          // 'old' is freed here, outside the lock
}

//==============================================================================
int VocalStudioAudioProcessor::getNumPrograms() { return (int) presets().size(); }
const String VocalStudioAudioProcessor::getProgramName (int index)
{
    return isPositiveAndBelow (index, (int) presets().size()) ? String (presets()[(size_t) index].name) : String();
}

void VocalStudioAudioProcessor::setCurrentProgram (int index)
{
    if (! isPositiveAndBelow (index, (int) presets().size())) return;
    currentProgram = index;
    // every parameter back to its default (EQ gains and notes included), then the preset's values
    for (auto* prm : getParameters())
        if (auto* r = dynamic_cast<RangedAudioParameter*> (prm))
            if (r->getParameterID() != IDs::compare)
                r->setValueNotifyingHost (r->getDefaultValue());
    for (auto& [id, v] : presets()[(size_t) index].values)
        if (auto* prm = apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 ((float) v));
    // tell the host which preset is now loaded (a preset picked in the plugin window would
    // otherwise leave the host showing the previous one)
    updateHostDisplay (AudioProcessorListener::ChangeDetails().withProgramChanged (true));
}

void VocalStudioAudioProcessor::getStateInformation (MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("version", VS_VERSION, nullptr);
    state.setProperty ("program", currentProgram, nullptr);
    state.setProperty ("editorW", editorWidth, nullptr);
    state.setProperty ("editorH", editorHeight, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void VocalStudioAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto t = ValueTree::fromXml (*xml);
            currentProgram = t.getProperty ("program", 0);
            editorWidth = t.getProperty ("editorW", 0);
            editorHeight = t.getProperty ("editorH", 0);
            apvts.replaceState (t);
            configDirty = true;
            triggerAsyncUpdate();
        }
}

AudioProcessorEditor* VocalStudioAudioProcessor::createEditor() { return new VocalStudioAudioProcessorEditor (*this); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VocalStudioAudioProcessor(); }
