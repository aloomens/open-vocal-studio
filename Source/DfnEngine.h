#pragma once

// DeepFilterNet3 (H. Schroeter et al., MIT / Apache-2.0) as a streaming, frame-by-frame network:
// one 10 ms frame of features in, the ERB gains and deep-filter coefficients for the frame two
// frames back out (the model's 2-frame look-ahead). The weights are read from the published ONNX
// files (embedded in the plugin); the layers are written out here, with their state carried from
// frame to frame (causal convolution history, GRU hidden states). Every buffer is sized in
// load(), so processFrame() never allocates and always gives the same result for the same input.
//
// Layer structure, as exported in enc.onnx / erb_dec.onnx / df_dec.onnx (DeepFilterNet3 defaults:
// 32 ERB bands, 96 deep-filter bins, 64 conv channels, 256-unit GRUs):
//  encoder: erb_conv0 (3x3, causal) -> erb_conv1..3 (depthwise 1x3 + pointwise, freq stride 2,2,1)
//           df_conv0 (3x3 grouped, causal) -> df_conv1 (stride 2) -> df_fc_emb (grouped linear)
//           emb = linear_out(GRU(linear_in(e3 + df_emb)))
//  ERB decoder: 2-layer GRU on emb, then a U-Net of pointwise skips and transposed convs -> 32 gains
//  DF decoder: 2-layer GRU + grouped skip, grouped linear -> tanh, plus a causal 5-frame
//           convolution of c0 -> 96 x 5 complex coefficients

#include <vector>
#include <string>
#include <map>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <stdexcept>
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
 #include <immintrin.h>
 #define VS_DFN_SSE 1
#endif

namespace vs
{
class DfnEngine
{
public:
    static constexpr int kErb = 32, kDf = 96, kCh = 64, kHid = 256, kEmb = 512, kOrder = 5;

    // Parses the three ONNX files and builds the network. Throws std::runtime_error on a mismatch.
    void load (const void* enc, size_t encSize, const void* erb, size_t erbSize, const void* df, size_t dfSize)
    {
        Tensors te, tr, td;
        parseModel ((const uint8_t*) enc, encSize, te);
        parseModel ((const uint8_t*) erb, erbSize, tr);
        parseModel ((const uint8_t*) df, dfSize, td);

        // ---- encoder ----
        erbConv0W = get (te, "onnx::Conv_282", 64 * 9); erbConv0B = get (te, "onnx::Conv_283", 64);
        dw1 = get (te, "erb_conv1.0.weight", 64 * 3); pw1W = get (te, "onnx::Conv_285", 64 * 64); pw1B = get (te, "onnx::Conv_286", 64);
        dw2 = get (te, "erb_conv2.0.weight", 64 * 3); pw2W = get (te, "onnx::Conv_288", 64 * 64); pw2B = get (te, "onnx::Conv_289", 64);
        dw3 = get (te, "erb_conv3.0.weight", 64 * 3); pw3W = get (te, "onnx::Conv_291", 64 * 64); pw3B = get (te, "onnx::Conv_292", 64);
        dfConv0W = get (te, "df_conv0.1.weight", 64 * 9); dfPw0W = get (te, "onnx::Conv_294", 64 * 64); dfPw0B = get (te, "onnx::Conv_295", 64);
        dfDw1 = get (te, "df_conv1.0.weight", 64 * 3); dfPw1W = get (te, "onnx::Conv_297", 64 * 64); dfPw1B = get (te, "onnx::Conv_298", 64);
        dfFcEmb = get (te, "df_fc_emb.0.weight", 32 * 96 * 16);
        encLinIn = get (te, "emb_gru.linear_in.0.weight", 16 * 32 * 16);
        encLinOut = get (te, "emb_gru.linear_out.0.weight", 16 * 16 * 32);
        encGru.set (get (te, "/emb_gru/Constant_3_output_0", 768 * 256), get (te, "/emb_gru/Constant_4_output_0", 768 * 256),
                    get (te, "/emb_gru/Constant_5_output_0", 1536));
        // ---- ERB decoder ----
        erbLinIn = get (tr, "emb_gru.linear_in.0.weight", 16 * 32 * 16);
        erbLinOut = get (tr, "emb_gru.linear_out.0.weight", 16 * 16 * 32);
        erbGru1.set (get (tr, "/emb_gru/Constant_3_output_0", 768 * 256), get (tr, "/emb_gru/Constant_4_output_0", 768 * 256),
                     get (tr, "/emb_gru/Constant_5_output_0", 1536));
        erbGru2.set (get (tr, "/emb_gru/Constant_9_output_0", 768 * 256), get (tr, "/emb_gru/Constant_10_output_0", 768 * 256),
                     get (tr, "/emb_gru/Constant_11_output_0", 1536));
        conv3pW = get (tr, "onnx::Conv_288", 64); conv3pB = get (tr, "onnx::Conv_289", 64);
        convt3Dw = get (tr, "convt3.0.weight", 64 * 3); convt3PwW = get (tr, "onnx::Conv_291", 64 * 64); convt3PwB = get (tr, "onnx::Conv_292", 64);
        conv2pW = get (tr, "onnx::Conv_294", 64); conv2pB = get (tr, "onnx::Conv_295", 64);
        convt2Dw = get (tr, "convt2.0.weight", 64 * 3); convt2PwW = get (tr, "onnx::Conv_297", 64 * 64); convt2PwB = get (tr, "onnx::Conv_298", 64);
        conv1pW = get (tr, "onnx::Conv_300", 64); conv1pB = get (tr, "onnx::Conv_301", 64);
        convt1Dw = get (tr, "convt1.0.weight", 64 * 3); convt1PwW = get (tr, "onnx::Conv_303", 64 * 64); convt1PwB = get (tr, "onnx::Conv_304", 64);
        conv0pW = get (tr, "onnx::Conv_306", 64); conv0pB = get (tr, "onnx::Conv_307", 64);
        convOutW = get (tr, "onnx::Conv_309", 64 * 3); convOutB = get (tr, "onnx::Conv_310", 1);
        // ---- DF decoder ----
        dfLinIn = get (td, "df_gru.linear_in.0.weight", 8 * 64 * 32);
        dfGru1.set (get (td, "onnx::GRU_291", 768 * 256), get (td, "onnx::GRU_292", 768 * 256), get (td, "onnx::GRU_293", 1536));
        dfGru2.set (get (td, "onnx::GRU_311", 768 * 256), get (td, "onnx::GRU_312", 768 * 256), get (td, "onnx::GRU_313", 1536));
        dfSkip = get (td, "df_skip.weight", 16 * 32 * 16);
        dfOut = get (td, "df_out.0.weight", 16 * 16 * 60);
        dfConvpW = get (td, "df_convp.1.weight", 10 * 32 * 5);
        dfConvpPwW = get (td, "onnx::Conv_268", 100); dfConvpPwB = get (td, "onnx::Conv_269", 10);

        // ---- state and scratch ----
        erbHist.assign (3 * kErb, 0.0f);
        specHist.assign (3 * 2 * kDf, 0.0f);
        c0Hist.assign (5 * kCh * kDf, 0.0f);
        e0.assign (kCh * 32, 0); e1.assign (kCh * 16, 0); e2.assign (kCh * 8, 0); e3.assign (kCh * 8, 0);
        c0.assign (kCh * kDf, 0); tmp.assign (kCh * kDf, 0); c1.assign (kCh * 48, 0); c1t.assign (48 * kCh, 0);
        dfEmb.assign (kEmb, 0); comb.assign (kEmb, 0); emb.assign (kEmb, 0);
        g256.assign (kHid, 0); g256b.assign (kHid, 0); g512.assign (kEmb, 0);
        d8.assign (kCh * 8, 0); d16.assign (kCh * 16, 0); d32.assign (kCh * 32, 0); u.assign (kCh * 32, 0);
        dfo.assign (kDf * 10, 0); convp.assign (10 * kDf, 0);
        loaded = true;
        reset();
    }

    bool isLoaded() const { return loaded; }

    // Everything the network carries from frame to frame, for save / restore (sized once).
    struct Snapshot { std::vector<float> erbHist, specHist, c0Hist, h; };
    void allocate (Snapshot& s) const
    {
        s.erbHist.assign (erbHist.size(), 0.0f); s.specHist.assign (specHist.size(), 0.0f);
        s.c0Hist.assign (c0Hist.size(), 0.0f); s.h.assign (5 * kHid, 0.0f);
    }
    void save (Snapshot& s) const
    {
        std::copy (erbHist.begin(), erbHist.end(), s.erbHist.begin());
        std::copy (specHist.begin(), specHist.end(), s.specHist.begin());
        std::copy (c0Hist.begin(), c0Hist.end(), s.c0Hist.begin());
        int i = 0;
        for (const Gru* g : { &encGru, &erbGru1, &erbGru2, &dfGru1, &dfGru2 }) { std::copy (g->h.begin(), g->h.end(), s.h.begin() + i * kHid); ++i; }
    }
    void restore (const Snapshot& s)
    {
        std::copy (s.erbHist.begin(), s.erbHist.end(), erbHist.begin());
        std::copy (s.specHist.begin(), s.specHist.end(), specHist.begin());
        std::copy (s.c0Hist.begin(), s.c0Hist.end(), c0Hist.begin());
        int i = 0;
        for (Gru* g : { &encGru, &erbGru1, &erbGru2, &dfGru1, &dfGru2 }) { std::copy (s.h.begin() + i * kHid, s.h.begin() + (i + 1) * kHid, g->h.begin()); ++i; }
    }

    void reset()
    {
        std::fill (erbHist.begin(), erbHist.end(), 0.0f);
        std::fill (specHist.begin(), specHist.end(), 0.0f);
        std::fill (c0Hist.begin(), c0Hist.end(), 0.0f);
        histPos = 0;
        for (auto* g : { &encGru, &erbGru1, &erbGru2, &dfGru1, &dfGru2 }) g->reset();
    }

    // featErb: 32 values; featSpec: 96 complex as re,im pairs. Outputs (for the frame 2 back):
    // mask 32 gains; coefs 96 x 5 x (re, im).
    void processFrame (const float* featErb, const float* featSpec, float* mask, float* coefs)
    {
        // ---------------- encoder ----------------
        // causal time history (oldest first): shift and append
        std::memmove (erbHist.data(), erbHist.data() + kErb, sizeof (float) * 2 * kErb);
        std::memcpy (erbHist.data() + 2 * kErb, featErb, sizeof (float) * kErb);
        std::memmove (specHist.data(), specHist.data() + 2 * kDf, sizeof (float) * 4 * kDf);
        for (int f = 0; f < kDf; ++f) { specHist[(size_t) (4 * kDf + f)] = featSpec[2 * f]; specHist[(size_t) (5 * kDf + f)] = featSpec[2 * f + 1]; }

        // erb_conv0: 1 -> 64, kernel 3x3 (time x freq), freq padding 1, + bias, ReLU
        for (int c = 0; c < kCh; ++c)
        {
            const float* w = &erbConv0W[(size_t) c * 9];
            for (int f = 0; f < 32; ++f)
            {
                float acc = erbConv0B[(size_t) c];
                for (int kt = 0; kt < 3; ++kt)
                    for (int kf = 0; kf < 3; ++kf)
                    {
                        const int ff = f + kf - 1;
                        if (ff >= 0 && ff < 32) acc += w[kt * 3 + kf] * erbHist[(size_t) (kt * kErb + ff)];
                    }
                e0[(size_t) (c * 32 + f)] = std::max (acc, 0.0f);
            }
        }
        sepConv (e0.data(), 32, dw1.data(), 2, pw1W.data(), pw1B.data(), e1.data());
        sepConv (e1.data(), 16, dw2.data(), 2, pw2W.data(), pw2B.data(), e2.data());
        sepConv (e2.data(), 8, dw3.data(), 1, pw3W.data(), pw3B.data(), e3.data());

        // df_conv0: (re, im) -> 64, grouped (ch 0-31 from re, 32-63 from im), 3x3, no bias; then pointwise + ReLU
        for (int c = 0; c < kCh; ++c)
        {
            const float* w = &dfConv0W[(size_t) c * 9];
            const int g = c / 32;
            for (int f = 0; f < kDf; ++f)
            {
                float acc = 0.0f;
                for (int kt = 0; kt < 3; ++kt)
                {
                    const float* x = &specHist[(size_t) ((kt * 2 + g) * kDf)];
                    for (int kf = 0; kf < 3; ++kf)
                    {
                        const int ff = f + kf - 1;
                        if (ff >= 0 && ff < kDf) acc += w[kt * 3 + kf] * x[ff];
                    }
                }
                tmp[(size_t) (c * kDf + f)] = acc;
            }
        }
        pointwise (tmp.data(), kDf, dfPw0W.data(), dfPw0B.data(), c0.data(), true);
        // keep c0 for the DF decoder's 5-frame convolution
        std::memmove (c0Hist.data(), c0Hist.data() + kCh * kDf, sizeof (float) * 4 * kCh * kDf);
        std::memcpy (c0Hist.data() + 4 * kCh * kDf, c0.data(), sizeof (float) * kCh * kDf);

        sepConv (c0.data(), kDf, dfDw1.data(), 2, dfPw1W.data(), dfPw1B.data(), c1.data());
        // [64][48] -> [48][64] -> grouped linear (32 groups, 96 -> 16) -> ReLU
        for (int c = 0; c < kCh; ++c) for (int f = 0; f < 48; ++f) c1t[(size_t) (f * kCh + c)] = c1[(size_t) (c * 48 + f)];
        groupedLinear (c1t.data(), dfFcEmb.data(), 32, 96, 16, dfEmb.data());
        for (auto& v : dfEmb) v = std::max (v, 0.0f);
        // e3 [64][8] -> [8][64], + df embedding
        for (int c = 0; c < kCh; ++c) for (int f = 0; f < 8; ++f) comb[(size_t) (f * kCh + c)] = e3[(size_t) (c * 8 + f)];
        for (int i = 0; i < kEmb; ++i) comb[(size_t) i] += dfEmb[(size_t) i];
        // emb_gru: linear_in (16 groups, 32 -> 16) ReLU, GRU, linear_out (16 groups, 16 -> 32) ReLU
        groupedLinear (comb.data(), encLinIn.data(), 16, 32, 16, g256.data());
        for (auto& v : g256) v = std::max (v, 0.0f);
        encGru.step (g256.data());
        groupedLinear (encGru.h.data(), encLinOut.data(), 16, 16, 32, emb.data());
        for (auto& v : emb) v = std::max (v, 0.0f);

        // ---------------- ERB decoder ----------------
        groupedLinear (emb.data(), erbLinIn.data(), 16, 32, 16, g256.data());
        for (auto& v : g256) v = std::max (v, 0.0f);
        erbGru1.step (g256.data());
        erbGru2.step (erbGru1.h.data());
        groupedLinear (erbGru2.h.data(), erbLinOut.data(), 16, 16, 32, g512.data());
        // [8][64] -> [64][8], ReLU
        for (int f = 0; f < 8; ++f) for (int c = 0; c < kCh; ++c) d8[(size_t) (c * 8 + f)] = std::max (g512[(size_t) (f * kCh + c)], 0.0f);
        // skip from e3 (depthwise 1x1 + bias, ReLU), then convt3 (depthwise 1x3, pointwise, ReLU)
        addSkip (e3.data(), 8, conv3pW.data(), conv3pB.data(), d8.data());
        sepConv (d8.data(), 8, convt3Dw.data(), 1, convt3PwW.data(), convt3PwB.data(), u.data());
        addSkip (e2.data(), 8, conv2pW.data(), conv2pB.data(), u.data());
        upConv (u.data(), 8, convt2Dw.data(), convt2PwW.data(), convt2PwB.data(), d16.data());
        addSkip (e1.data(), 16, conv1pW.data(), conv1pB.data(), d16.data());
        upConv (d16.data(), 16, convt1Dw.data(), convt1PwW.data(), convt1PwB.data(), d32.data());
        addSkip (e0.data(), 32, conv0pW.data(), conv0pB.data(), d32.data());
        // conv0_out: 64 -> 1, 1x3, + bias, sigmoid
        for (int f = 0; f < 32; ++f)
        {
            float acc = convOutB[0];
            for (int c = 0; c < kCh; ++c)
                for (int kf = 0; kf < 3; ++kf)
                {
                    const int ff = f + kf - 1;
                    if (ff >= 0 && ff < 32) acc += convOutW[(size_t) (c * 3 + kf)] * d32[(size_t) (c * 32 + ff)];
                }
            mask[f] = 1.0f / (1.0f + std::exp (-acc));
        }

        // ---------------- DF decoder ----------------
        groupedLinear (emb.data(), dfLinIn.data(), 8, 64, 32, g256.data());
        for (auto& v : g256) v = std::max (v, 0.0f);
        dfGru1.step (g256.data());
        dfGru2.step (dfGru1.h.data());
        groupedLinear (emb.data(), dfSkip.data(), 16, 32, 16, g256b.data());
        for (int i = 0; i < kHid; ++i) g256b[(size_t) i] += dfGru2.h[(size_t) i];
        groupedLinear (g256b.data(), dfOut.data(), 16, 16, 60, dfo.data());     // 960 = [96][10]
        // df_convp: c0 over the last 5 frames, 2 groups (32 in -> 5 out each), then pointwise 10 -> 10 + bias, ReLU
        for (int o = 0; o < 10; ++o)
        {
            const int g = o / 5;
            float* out = &tmp[(size_t) (o * kDf)];
            std::fill (out, out + kDf, 0.0f);
            for (int ci = 0; ci < 32; ++ci)
                for (int kt = 0; kt < 5; ++kt)
                    axpy (dfConvpW[(size_t) ((o * 32 + ci) * 5 + kt)], &c0Hist[(size_t) ((kt * kCh + g * 32 + ci) * kDf)], out, kDf);
        }
        for (int o = 0; o < 10; ++o)
        {
            float* out = &convp[(size_t) (o * kDf)];
            for (int f = 0; f < kDf; ++f) out[f] = dfConvpPwB[(size_t) o];
            for (int i = 0; i < 10; ++i) axpy (dfConvpPwW[(size_t) (o * 10 + i)], &tmp[(size_t) (i * kDf)], out, kDf);
            for (int f = 0; f < kDf; ++f) out[f] = std::max (out[f], 0.0f);
        }
        for (int f = 0; f < kDf; ++f)
            for (int o = 0; o < 10; ++o)
                coefs[f * 10 + o] = std::tanh (dfo[(size_t) (f * 10 + o)]) + convp[(size_t) (o * kDf + f)];
    }

private:
    //------------------------------------------------------------------ math helpers
    static inline void axpy (float a, const float* x, float* y, int n)
    {
        int i = 0;
       #if VS_DFN_SSE
        const __m128 va = _mm_set1_ps (a);
        for (; i + 4 <= n; i += 4) _mm_storeu_ps (y + i, _mm_add_ps (_mm_loadu_ps (y + i), _mm_mul_ps (va, _mm_loadu_ps (x + i))));
       #endif
        for (; i < n; ++i) y[i] += a * x[i];
    }
    static inline float dot (const float* a, const float* b, int n)
    {
        int i = 0; float s = 0.0f;
       #if VS_DFN_SSE
        __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps(), s2 = _mm_setzero_ps(), s3 = _mm_setzero_ps();
        for (; i + 16 <= n; i += 16)
        {
            s0 = _mm_add_ps (s0, _mm_mul_ps (_mm_loadu_ps (a + i), _mm_loadu_ps (b + i)));
            s1 = _mm_add_ps (s1, _mm_mul_ps (_mm_loadu_ps (a + i + 4), _mm_loadu_ps (b + i + 4)));
            s2 = _mm_add_ps (s2, _mm_mul_ps (_mm_loadu_ps (a + i + 8), _mm_loadu_ps (b + i + 8)));
            s3 = _mm_add_ps (s3, _mm_mul_ps (_mm_loadu_ps (a + i + 12), _mm_loadu_ps (b + i + 12)));
        }
        __m128 t = _mm_add_ps (_mm_add_ps (s0, s1), _mm_add_ps (s2, s3));
        alignas (16) float r[4]; _mm_store_ps (r, t);
        s = (r[0] + r[1]) + (r[2] + r[3]);
       #endif
        for (; i < n; ++i) s += a[i] * b[i];
        return s;
    }

    // out[g*H + h] = sum_i in[g*I + i] * w[(g*I + i)*H + h]   (einsum btgi,gih->btgh)
    static void groupedLinear (const float* in, const float* w, int G, int I, int H, float* out)
    {
        for (int g = 0; g < G; ++g)
        {
            float* o = out + g * H;
            std::fill (o, o + H, 0.0f);
            for (int i = 0; i < I; ++i) axpy (in[g * I + i], w + (size_t) (g * I + i) * H, o, H);
        }
    }

    // pointwise 64 -> 64 over F bins: out[c][f] = b[c] + sum_ci W[c][ci] in[ci][f]
    static void pointwise (const float* in, int F, const float* W, const float* b, float* out, bool relu)
    {
        for (int c = 0; c < kCh; ++c)
        {
            float* o = out + c * F;
            for (int f = 0; f < F; ++f) o[f] = b[c];
            for (int ci = 0; ci < kCh; ++ci) axpy (W[c * kCh + ci], in + ci * F, o, F);
            if (relu) for (int f = 0; f < F; ++f) o[f] = std::max (o[f], 0.0f);
        }
    }

    // depthwise 1x3 (freq padding 1, stride s, no bias) then pointwise + bias + ReLU
    void sepConv (const float* in, int F, const float* dw, int stride, const float* pwW, const float* pwB, float* out)
    {
        const int Fo = (F + 2 - 3) / stride + 1;
        for (int c = 0; c < kCh; ++c)
        {
            const float* x = in + c * F;
            const float* w = dw + c * 3;
            float* y = &tmp[(size_t) (c * Fo)];
            for (int fo = 0; fo < Fo; ++fo)
            {
                float acc = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const int fi = fo * stride + k - 1;
                    if (fi >= 0 && fi < F) acc += w[k] * x[fi];
                }
                y[fo] = acc;
            }
        }
        pointwise (tmp.data(), Fo, pwW, pwB, out, true);
    }

    // transposed depthwise 1x3 (stride 2, padding 1, output padding 1: F -> 2F) then pointwise + ReLU
    void upConv (const float* in, int F, const float* dw, const float* pwW, const float* pwB, float* out)
    {
        const int Fo = 2 * F;
        for (int c = 0; c < kCh; ++c)
        {
            const float* x = in + c * F;
            const float* w = dw + c * 3;
            float* y = &tmp[(size_t) (c * Fo)];
            std::fill (y, y + Fo, 0.0f);
            for (int i = 0; i < F; ++i)
                for (int k = 0; k < 3; ++k)
                {
                    const int o = 2 * i - 1 + k;
                    if (o >= 0 && o < Fo) y[o] += x[i] * w[k];
                }
        }
        pointwise (tmp.data(), Fo, pwW, pwB, out, true);
    }

    // y += ReLU(w[c] * skip[c][f] + b[c])   (depthwise 1x1 pathway conv)
    static void addSkip (const float* skip, int F, const float* w, const float* b, float* y)
    {
        for (int c = 0; c < kCh; ++c)
            for (int f = 0; f < F; ++f)
                y[c * F + f] += std::max (w[c] * skip[c * F + f] + b[c], 0.0f);
    }

    // ONNX GRU, gates z, r, h, linear_before_reset = 1, input and hidden size 256
    struct Gru
    {
        std::vector<float> W, R, Wb, Rb, h, gx, gh;
        void set (std::vector<float> w, std::vector<float> r, const std::vector<float>& b)
        {
            W = std::move (w); R = std::move (r);
            Wb.assign (b.begin(), b.begin() + 768); Rb.assign (b.begin() + 768, b.end());
            h.assign (kHid, 0.0f); gx.assign (768, 0.0f); gh.assign (768, 0.0f);
        }
        void reset() { std::fill (h.begin(), h.end(), 0.0f); }
        void step (const float* x)
        {
            for (int j = 0; j < 768; ++j)
            {
                gx[(size_t) j] = Wb[(size_t) j] + dot (&W[(size_t) j * kHid], x, kHid);
                gh[(size_t) j] = Rb[(size_t) j] + dot (&R[(size_t) j * kHid], h.data(), kHid);
            }
            for (int j = 0; j < kHid; ++j)
            {
                const float z = 1.0f / (1.0f + std::exp (-(gx[(size_t) j] + gh[(size_t) j])));
                const float r = 1.0f / (1.0f + std::exp (-(gx[(size_t) (256 + j)] + gh[(size_t) (256 + j)])));
                const float n = std::tanh (gx[(size_t) (512 + j)] + r * gh[(size_t) (512 + j)]);
                h[(size_t) j] = (1.0f - z) * n + z * h[(size_t) j];
            }
        }
    };

    //------------------------------------------------------------------ ONNX reading
    using Tensors = std::map<std::string, std::vector<float>>;

    static std::vector<float> get (const Tensors& t, const std::string& name, size_t expect)
    {
        auto it = t.find (name);
        if (it == t.end()) throw std::runtime_error ("DeepFilterNet3 weight missing: " + name);
        if (it->second.size() != expect) throw std::runtime_error ("DeepFilterNet3 weight has the wrong size: " + name);
        return it->second;
    }

    struct Pb
    {
        const uint8_t* p; const uint8_t* e;
        bool more() const { return p < e; }
        uint64_t varint() { uint64_t r = 0; int s = 0; while (p < e) { const uint8_t b = *p++; r |= (uint64_t) (b & 0x7f) << s; if (! (b & 0x80)) break; s += 7; } return r; }
        // calls f(field, wiretype, value-or-length, data-pointer)
        template <typename F> void each (F&& f)
        {
            while (more())
            {
                const uint64_t key = varint();
                const int field = (int) (key >> 3), wt = (int) (key & 7);
                if (wt == 0) { const uint64_t v = varint(); f (field, wt, v, nullptr); }
                else if (wt == 2) { const uint64_t n = varint(); if (p + n > e) throw std::runtime_error ("bad ONNX"); f (field, wt, n, p); p += n; }
                else if (wt == 5) { f (field, wt, 4, p); p += 4; }
                else if (wt == 1) { f (field, wt, 8, p); p += 8; }
                else throw std::runtime_error ("bad ONNX wire type");
            }
        }
    };

    static void readTensor (const uint8_t* d, size_t n, std::string& name, std::vector<float>& out, bool& isFloat)
    {
        Pb pb { d, d + n };
        const uint8_t* raw = nullptr; size_t rawN = 0; int dtype = 0;
        std::vector<float> fdata;
        pb.each ([&] (int field, int wt, uint64_t v, const uint8_t* p)
        {
            if (field == 2 && wt == 0) dtype = (int) v;
            else if (field == 8 && wt == 2) name.assign ((const char*) p, (size_t) v);
            else if (field == 9 && wt == 2) { raw = p; rawN = (size_t) v; }
            else if (field == 4 && wt == 2) { const size_t k = (size_t) v / 4; fdata.resize (k); std::memcpy (fdata.data(), p, k * 4); }
            else if (field == 4 && wt == 5) { float f; std::memcpy (&f, p, 4); fdata.push_back (f); }
        });
        isFloat = dtype == 1;
        if (! isFloat) return;
        if (raw != nullptr) { out.resize (rawN / 4); std::memcpy (out.data(), raw, (rawN / 4) * 4); }
        else out = std::move (fdata);
    }

    static void parseModel (const uint8_t* d, size_t n, Tensors& t)
    {
        Pb model { d, d + n };
        model.each ([&] (int field, int wt, uint64_t v, const uint8_t* p)
        {
            if (field != 7 || wt != 2) return;            // graph
            Pb graph { p, p + v };
            graph.each ([&] (int gf, int gwt, uint64_t gv, const uint8_t* gp)
            {
                if (gwt != 2) return;
                if (gf == 5)                               // initializer
                {
                    std::string name; std::vector<float> data; bool fl = false;
                    readTensor (gp, (size_t) gv, name, data, fl);
                    if (fl) t[name] = std::move (data);
                }
                else if (gf == 1)                          // node: keep Constant float tensors
                {
                    Pb node { gp, gp + gv };
                    std::string op, out; const uint8_t* tp = nullptr; size_t tn = 0;
                    node.each ([&] (int nf, int nwt, uint64_t nv, const uint8_t* np)
                    {
                        if (nwt != 2) return;
                        if (nf == 4) op.assign ((const char*) np, (size_t) nv);
                        else if (nf == 2) out.assign ((const char*) np, (size_t) nv);
                        else if (nf == 5)
                        {
                            Pb at { np, np + nv };
                            at.each ([&] (int af, int awt, uint64_t av, const uint8_t* ap)
                            { if (af == 5 && awt == 2) { tp = ap; tn = (size_t) av; } });
                        }
                    });
                    if (op == "Constant" && tp != nullptr)
                    {
                        std::string name; std::vector<float> data; bool fl = false;
                        readTensor (tp, tn, name, data, fl);
                        if (fl && data.size() > 1) t[out] = std::move (data);
                    }
                }
            });
        });
    }

    bool loaded = false;
    std::vector<float> erbConv0W, erbConv0B, dw1, pw1W, pw1B, dw2, pw2W, pw2B, dw3, pw3W, pw3B;
    std::vector<float> dfConv0W, dfPw0W, dfPw0B, dfDw1, dfPw1W, dfPw1B, dfFcEmb, encLinIn, encLinOut;
    std::vector<float> erbLinIn, erbLinOut, conv3pW, conv3pB, convt3Dw, convt3PwW, convt3PwB, conv2pW, conv2pB;
    std::vector<float> convt2Dw, convt2PwW, convt2PwB, conv1pW, conv1pB, convt1Dw, convt1PwW, convt1PwB, conv0pW, conv0pB, convOutW, convOutB;
    std::vector<float> dfLinIn, dfSkip, dfOut, dfConvpW, dfConvpPwW, dfConvpPwB;
    Gru encGru, erbGru1, erbGru2, dfGru1, dfGru2;
    std::vector<float> erbHist, specHist, c0Hist;
    int histPos = 0;
    std::vector<float> e0, e1, e2, e3, c0, tmp, c1, c1t, dfEmb, comb, emb, g256, g256b, g512, d8, d16, d32, u, dfo, convp;
};
} // namespace vs
