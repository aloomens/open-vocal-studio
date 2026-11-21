#include "DeepFilter.h"
#include <BinaryData.h>
#include <mutex>

namespace vs
{
static DfnStatus gStatus;
static std::mutex gStatusMutex;

DfnStatus DeepFilter::status() { std::lock_guard<std::mutex> l (gStatusMutex); return gStatus; }

std::vector<int> DeepFilter::makeErbBands()
{
    // libDF erb_fb(): equal steps on the ERB scale, at least 2 bins per band
    auto freq2erb = [] (double f) { return 9.265 * std::log (1.0 + f / (24.7 * 9.265)); };
    auto erb2freq = [] (double e) { return 24.7 * 9.265 * (std::exp (e / 9.265) - 1.0); };
    const double freqWidth = (double) kSr / kFft;
    const double lo = freq2erb (0.0), hi = freq2erb (kSr / 2.0);
    const double step = (hi - lo) / kErb;
    std::vector<int> erb ((size_t) kErb, 0);
    int prev = 0, over = 0;
    for (int i = 1; i <= kErb; ++i)
    {
        const double f = erb2freq (lo + i * step);
        const int fb = (int) std::lround (f / freqWidth);
        int nb = fb - prev - over;
        if (nb < 2) { over = 2 - nb; nb = 2; }
        else over = 0;
        erb[(size_t) i - 1] = nb;
        prev = fb;
    }
    erb[kErb - 1] += 1;
    int sum = 0; for (int v : erb) sum += v;
    const int tooLarge = sum - kBins;
    if (tooLarge > 0) erb[kErb - 1] -= tooLarge;
    return erb;
}

void DeepFilter::prepare (double fs, int maxBlock)
{
    if (! engine.isLoaded())
    {
        juce::String msg;
        try
        {
            engine.load (BinaryData::enc_onnx, (size_t) BinaryData::enc_onnxSize,
                         BinaryData::erb_dec_onnx, (size_t) BinaryData::erb_dec_onnxSize,
                         BinaryData::df_dec_onnx, (size_t) BinaryData::df_dec_onnxSize);
            msg = "DeepFilterNet3 loaded";
            engine.allocate (resume.net);
        }
        catch (const std::exception& e) { msg = e.what(); }
        std::lock_guard<std::mutex> l (gStatusMutex);
        gStatus.loaded = engine.isLoaded();
        gStatus.message = msg;
    }

    fft.prepare (kFft);
    window.resize (kFft);
    for (int i = 0; i < kFft; ++i)
    {
        const double s = std::sin (0.5 * kPiD * (i + 0.5) / (kFft / 2));
        window[(size_t) i] = (float) std::sin (0.5 * kPiD * s * s);     // Vorbis window: w^2 sums to 1 at 50 %
    }
    erbBands = makeErbBands();
    {   // libDF calc_norm_alpha(): exp(-hop/sr/tau), rounded to the precision where it's below 1
        const double a = std::exp (-(double) kHop / kSr / 1.0);
        double r = 1.0; int prec = 3;
        while (r >= 1.0) { const double p = std::pow (10.0, prec); r = std::round (a * p) / p; ++prec; }
        alpha = (float) r;
    }
    for (auto& s : specHist) s.assign (kBins, {});
    inWin.assign (kFft, 0.0f); frameBuf.assign (kFft, 0.0f); ola.assign (kFft, 0.0f); synthBuf.assign (kFft, 0.0f);
    outBins.assign (kBins, {}); cur.assign (kBins, {});

    // 48 kHz delay: frame f is analyzed when its last sample arrives; the network answers for frame
    // f - 2, which is synthesized at once. Output sample s leaves (kLook + 2) * hop - 1 samples later.
    const int d48 = (kLook + 2) * kHop - 1;
    resampling = std::llround (fs) != kSr;
    toModel.prepare (fs, kSr, maxBlock);
    fromModel.prepare (kSr, fs, (int) std::ceil (maxBlock * (double) kSr / fs) + 16);
    extra48 = 0;
    if (resampling)
    {
        const int L = toModel.ratioL();     // pad the 48 kHz delay to a whole number of host samples
        extra48 = (L - (d48 % L)) % L;
    }
    {
        int s = 1; while (s < d48 + extra48 + 4 * kHop + 4096) s <<= 1;
        outFifo.assign ((size_t) s, 0.0f); fifoMask = s - 1;
    }
    const int total48 = d48 + extra48;
    if (resampling)
    {
        const int M = toModel.ratioM(), L = toModel.ratioL();
        const int z = toModel.lookahead() + (int) std::ceil (fromModel.lookahead() * (double) M / L) + 3;
        latency = (int) ((int64_t) total48 * M / L) + z;
        int s = 1; while (s < z + 2 * maxBlock + 64) s <<= 1;
        hostFifo.assign ((size_t) s, 0.0f); hostMask = s - 1;
        hostPrefill = z;
        buf48in.assign ((size_t) std::ceil (maxBlock * (double) kSr / fs) + 16, 0.0f);
        buf48out.assign (buf48in.size(), 0.0f);
        hostTmp.assign ((size_t) maxBlock * 2 + 64, 0.0f);
    }
    else
    {
        latency = total48;
        hostPrefill = 0;
    }
    reset();
}

void DeepFilter::reset()
{
    for (int b = 0; b < kErb; ++b) erbState[(size_t) b] = -60.0f + (-90.0f + 60.0f) * b / (kErb - 1);
    for (int b = 0; b < kDf; ++b) specState[(size_t) b] = 0.001f + (0.0001f - 0.001f) * b / (kDf - 1);
    for (auto& s : specHist) std::fill (s.begin(), s.end(), std::complex<float> {});
    std::fill (inWin.begin(), inWin.end(), 0.0f);
    std::fill (ola.begin(), ola.end(), 0.0f);
    std::fill (outFifo.begin(), outFifo.end(), 0.0f);
    hopFill = 0; frameCount = 0;
    fifoR = 0;
    fifoW = (kLook + 1) * kHop - 1 + extra48;   // pre-filled with silence
    if (engine.isLoaded()) engine.reset();
    mask.fill (1.0f); coefs.fill (0.0f);
    amountCur = amountT;
    toModel.reset(); fromModel.reset();
    if (resampling) { std::fill (hostFifo.begin(), hostFifo.end(), 0.0f); hostR = 0; hostW = hostPrefill; }
    underruns = 0;
    floorHist.fill (1000.0f);                   // no floor yet
    floorPos = 0; guardMix = 0.0f;
    resume.valid = false; sinceZeros = 0; zeroRun = 0; inGap = false;
}

void DeepFilter::computeFeatures (const std::complex<float>* X)
{
    // ERB features: band power in dB, exponential mean normalization, / 40
    int bin = 0;
    for (int b = 0; b < kErb; ++b)
    {
        const int nb = erbBands[(size_t) b];
        float e = 0.0f;
        for (int j = 0; j < nb; ++j) e += std::norm (X[bin + j]);
        e /= (float) nb;
        bin += nb;
        const float db = 10.0f * std::log10 (e + 1e-10f);
        float& s = erbState[(size_t) b];
        s = db * (1.0f - alpha) + s * alpha;
        fErb[(size_t) b] = (db - s) / 40.0f;
    }
    // complex features: unit normalization of the lowest 96 bins
    for (int k = 0; k < kDf; ++k)
    {
        float& s = specState[(size_t) k];
        s = std::abs (X[k]) * (1.0f - alpha) + s * alpha;
        const auto v = X[k] / std::sqrt (s);
        fSpec[(size_t) (2 * k)] = v.real(); fSpec[(size_t) (2 * k + 1)] = v.imag();
    }
}

void DeepFilter::formFrame()
{
    const int64_t f = frameCount++;
    // analysis: window, FFT, normalize by 1/N (libDF), so the features match the training
    std::rotate (specHist.begin(), specHist.begin() + 1, specHist.end());
    auto& X = specHist[kOrder - 1];
    for (int i = 0; i < kFft; ++i) frameBuf[(size_t) i] = inWin[(size_t) i] * window[(size_t) i];
    fft.forwardReal (frameBuf.data(), X.data());
    for (auto& v : X) v *= 1.0f / (float) kFft;
    // Digital silence (a stopped DAW, a muted stretch): the features' running means and the
    // network's state are left as they were, so the voice that follows is treated exactly as if
    // there had been no gap. The frame itself comes out silent either way.
    double frameEnergy = 0.0;
    for (const auto& v : X) frameEnergy += std::norm (v);
    const bool silent = frameEnergy < 1.0e-14;
    const bool clean = ! silent && sinceZeros >= kFft;   // the whole window is signal
    if (silent) inGap = true;
    else
    {
        if (inGap && resume.valid)
        {
            // back from silence: continue from the state just before it
            engine.restore (resume.net);
            erbState = resume.erbState; specState = resume.specState;
            floorHist = resume.floorHist; floorPos = resume.floorPos; guardMix = resume.guardMix;
        }
        inGap = false;
        computeFeatures (X.data());
    }
    auto saveResume = [&]
    {
        if (! clean || ! engine.isLoaded()) return;
        engine.save (resume.net);
        resume.erbState = erbState; resume.specState = specState;
        resume.floorHist = floorHist; resume.floorPos = floorPos; resume.guardMix = guardMix;
        resume.valid = true;
    };

    // amount: smoothed per frame; the network only runs while it's wanted
    const bool wasOff = amountCur <= 0.0f;
    amountCur += 0.3f * (amountT - amountCur);
    if (std::abs (amountT - amountCur) < 1e-4f) amountCur = amountT;
    if (amountCur > 0.0f && engine.isLoaded() && ! silent)
    {
        if (wasOff) engine.reset();             // coming back on: start the network from rest
        engine.processFrame (fErb.data(), fSpec.data(), mask.data(), coefs.data());
    }

    // synthesize frame k = f - 2 (the network's answer), using spectra k-2 .. k+2
    const int64_t k = f - kLook;
    if (k < 0) { saveResume(); return; }        // the pre-filled silence plays until then
    const auto& Xk = specHist[(size_t) (kOrder - 1 - kLook)];
    const float a = engine.isLoaded() && f >= kLook ? amountCur : 0.0f;
    if (a <= 0.0f)
        std::copy (Xk.begin(), Xk.end(), outBins.begin());
    else
    {
        int bin = 0;
        for (int b = 0; b < kErb; ++b)
            for (int j = 0; j < erbBands[(size_t) b]; ++j, ++bin) outBins[(size_t) bin] = Xk[(size_t) bin] * mask[(size_t) b];
        for (int b = 0; b < kDf; ++b)
        {
            std::complex<float> acc {};
            for (int i = 0; i < kOrder; ++i)
            {
                const std::complex<float> c (coefs[(size_t) (b * 10 + 2 * i)], coefs[(size_t) (b * 10 + 2 * i + 1)]);
                acc += specHist[(size_t) i][(size_t) b] * c;
            }
            outBins[(size_t) b] = acc;
        }
        // amount below 100 %: the original is mixed back in (0..40 dB of removal)
        const float lim = a >= 0.999f ? 0.0f : std::pow (10.0f, -(a * 40.0f) / 20.0f);
        if (lim > 0.0f)
            for (int b = 0; b < kBins; ++b) outBins[(size_t) b] = outBins[(size_t) b] * (1.0f - lim) + Xk[(size_t) b] * lim;

        // voice guard
        double ein = 0, eout = 0, logSum = 0, linSum = 0; int nFlat = 0;
        for (int b = 2; b < kBins; ++b)
        {
            const double p = std::norm (Xk[(size_t) b]);
            ein += p; eout += std::norm (outBins[(size_t) b]);
            if (b >= 4 && b <= 100) { logSum += std::log (p + 1e-20); linSum += p; ++nFlat; }   // 200 Hz - 5 kHz
        }
        const double flat = std::exp (logSum / nFlat) / (linSum / nFlat + 1e-20);
        const bool tonal = flat < 0.12;                 // harmonic (a sung note), not noise
        const float inDb = 10.0f * std::log10 ((float) ein + 1e-20f);
        // every frame teaches the floor (a constant hum or buzz is tonal too, and must count as noise);
        // a sung note stands well above the quieter frames between phrases
        if (inDb > -120.0f)
        {
            floorHist[(size_t) floorPos] = inDb;
            floorPos = (floorPos + 1) % kFloorFrames;
        }
        float floorDb = 1e9f;
        for (float v : floorHist) floorDb = std::min (floorDb, v);
        const float snr = floorDb > 500.0f ? 100.0f : inDb - floorDb;   // no noise seen yet: treat as clean
        float allowed = snr >= 35.0f ? 3.0f : (snr <= 18.0f ? 80.0f : 3.0f + (35.0f - snr) / 17.0f * 77.0f);
        if (tonal && snr >= 10.0f) allowed = 2.0f;
        const float lossDb = 10.0f * std::log10 ((float) (ein / (eout + 1e-20)));
        float want = 0.0f;
        if (lossDb > allowed && ein > 0)
        {
            const double a1 = std::sqrt (eout), a2 = std::sqrt (ein), t = a2 * std::pow (10.0, -allowed / 20.0);
            want = (float) std::clamp ((t - a1) / (a2 - a1 + 1e-20), 0.0, 1.0);
        }
        guardMix += (want > guardMix ? 0.7f : 0.15f) * (want - guardMix);
        if (guardMix > 1e-4f)
            for (int b = 0; b < kBins; ++b) outBins[(size_t) b] += guardMix * (Xk[(size_t) b] - outBins[(size_t) b]);
    }

    // synthesis: inverse FFT (unnormalized; the 1/N is in the analysis), window, overlap-add
    fft.inverseReal (outBins.data(), synthBuf.data());
    for (int i = 0; i < kFft; ++i) ola[(size_t) i] += synthBuf[(size_t) i] * window[(size_t) i];
    for (int i = 0; i < kHop; ++i) outFifo[(size_t) ((fifoW + i) & fifoMask)] = ola[(size_t) i];
    fifoW += kHop;
    std::copy (ola.begin() + kHop, ola.end(), ola.begin());
    std::fill (ola.begin() + kHop, ola.end(), 0.0f);
    saveResume();
}

void DeepFilter::process48 (const float* in, float* out, int n)
{
    for (int i = 0; i < n; ++i)
    {
        // a run of 64+ (near-)zero samples is digital silence
        if (std::abs (in[i]) < 1.0e-7f) { if (++zeroRun >= 64) sinceZeros = 0; }
        else zeroRun = 0;
        ++sinceZeros;
        inWin[(size_t) (kHop + hopFill)] = in[i];
        if (++hopFill == kHop)
        {
            formFrame();
            std::copy (inWin.begin() + kHop, inWin.end(), inWin.begin());
            hopFill = 0;
        }
        out[i] = outFifo[(size_t) (fifoR & fifoMask)];
        ++fifoR;
    }
}

void DeepFilter::process (float* x, int n)
{
    if (! resampling) { process48 (x, x, n); return; }
    const int n48 = toModel.process (x, n, buf48in.data(), (int) buf48in.size());
    process48 (buf48in.data(), buf48out.data(), n48);
    const int nh = fromModel.process (buf48out.data(), n48, hostTmp.data(), (int) hostTmp.size());
    for (int i = 0; i < nh; ++i) hostFifo[(size_t) ((hostW + i) & hostMask)] = hostTmp[(size_t) i];
    hostW += nh;
    for (int i = 0; i < n; ++i)
    {
        if (hostR < hostW) { x[i] = hostFifo[(size_t) (hostR & hostMask)]; ++hostR; }
        else { x[i] = 0.0f; ++underruns; }
    }
}

std::vector<float> DeepFilter::processWholeForTest (const std::vector<float>& in48)
{
    // the streaming path itself, run at 48 kHz, with the delay removed
    reset();
    const int d48 = (kLook + 2) * kHop - 1 + extra48;
    std::vector<float> x (in48);
    x.resize (in48.size() + (size_t) d48, 0.0f);
    std::vector<float> y (x.size());
    for (size_t p = 0; p < x.size(); p += 512)
    {
        const int n = (int) std::min<size_t> (512, x.size() - p);
        process48 (&x[p], &y[p], n);
    }
    std::vector<float> res (y.begin() + d48, y.begin() + d48 + (long) in48.size());
    reset();
    return res;
}

void DeepFilter::featuresForTest (const std::vector<float>& in48, std::vector<float>& erb, std::vector<float>& spec)
{
    reset();
    erb.clear(); spec.clear();
    std::vector<float> win (kFft, 0.0f), fr (kFft);
    std::vector<std::complex<float>> X (kBins);
    for (size_t p = 0; p + kHop <= in48.size(); p += kHop)
    {
        std::copy (win.begin() + kHop, win.end(), win.begin());
        std::copy_n (&in48[p], kHop, win.begin() + kHop);
        for (int i = 0; i < kFft; ++i) fr[(size_t) i] = win[(size_t) i] * window[(size_t) i];
        fft.forwardReal (fr.data(), X.data());
        for (auto& v : X) v *= 1.0f / (float) kFft;
        computeFeatures (X.data());
        erb.insert (erb.end(), fErb.begin(), fErb.end());
        spec.insert (spec.end(), fSpec.begin(), fSpec.end());
    }
    reset();
}

void DeepFilter::runNetworkForTest (const std::vector<float>& erb, const std::vector<float>& spec, std::vector<float>& m, std::vector<float>& c)
{
    engine.reset();
    const size_t T = erb.size() / kErb;
    m.assign (T * kErb, 0.0f); c.assign (T * kDf * 10, 0.0f);
    for (size_t t = 0; t < T; ++t) engine.processFrame (&erb[t * kErb], &spec[t * kDf * 2], &m[t * kErb], &c[t * kDf * 10]);
    engine.reset();
}
} // namespace vs
