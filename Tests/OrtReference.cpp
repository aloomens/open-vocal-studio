// Test-only reference: runs the original DeepFilterNet3 ONNX files through ONNX Runtime
// (loaded at run time as vs_onnxruntime.dll next to the test program), whole sequence at once.
// DfnCheck compares the plugin's own streaming engine (Source/DfnEngine.h) against it.
#include <juce_core/juce_core.h>
#include <BinaryData.h>
#include <complex>
#include <mutex>
#include <memory>
#include <vector>
#include <string>
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include "OrtReference.h"

namespace vs
{
static constexpr int kRefErb = 32, kRefDf = 96, kRefOrder = 5;
//==============================================================================
// ONNX Runtime, loaded once per process from "vs_onnxruntime.dll" next to this module.
// The private name keeps it apart from any other onnxruntime.dll the host or Windows has loaded.
namespace
{
struct OrtRuntime
{
    std::mutex m;
    bool tried = false;
    const OrtApi* api = nullptr;
    std::unique_ptr<Ort::Env> env;
    juce::String error;

    static OrtRuntime& get() { static OrtRuntime r; return r; }

    static juce::File moduleFolder()
    {
       #if JUCE_WINDOWS
        HMODULE h = nullptr;
        GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR> (&OrtRuntime::get), &h);
        wchar_t path[MAX_PATH * 4] {};
        GetModuleFileNameW (h, path, (DWORD) (sizeof (path) / sizeof (path[0])));
        return juce::File (juce::String (path)).getParentDirectory();
       #else
        return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
       #endif
    }

    bool ensure()
    {
        std::lock_guard<std::mutex> lock (m);
        if (tried) return api != nullptr;
        tried = true;
       #if JUCE_WINDOWS
        const auto dll = moduleFolder().getChildFile ("vs_onnxruntime.dll");
        HMODULE lib = LoadLibraryExW (dll.getFullPathName().toWideCharPointer(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (lib == nullptr) { error = "Neural engine not found (" + dll.getFullPathName() + ")"; return false; }
        using GetBase = const OrtApiBase* (ORT_API_CALL*) ();
        auto getBase = reinterpret_cast<GetBase> (GetProcAddress (lib, "OrtGetApiBase"));
        if (getBase == nullptr) { error = "Neural engine DLL is not ONNX Runtime"; return false; }
        api = getBase()->GetApi (ORT_API_VERSION);
        if (api == nullptr) { error = "Neural engine DLL is too old"; return false; }
        Ort::InitApi (api);
        try
        {
            OrtThreadingOptions* to = nullptr;   // no global pools: each session runs on its caller's thread
            juce::ignoreUnused (to);
            env = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_ERROR, "VocalStudio");
        }
        catch (const std::exception& e) { error = juce::String ("ONNX Runtime: ") + e.what(); api = nullptr; return false; }
        return true;
       #else
        error = "Neural engine is only available on Windows";
        return false;
       #endif
    }
};
} // namespace

//==============================================================================
class DfnModel
{
public:
    bool load (juce::String& err)
    {
        auto& rt = OrtRuntime::get();
        if (! rt.ensure()) { err = rt.error; return false; }
        try
        {
            Ort::SessionOptions so;
            so.SetIntraOpNumThreads (1);
            so.SetInterOpNumThreads (1);
            so.SetExecutionMode (ExecutionMode::ORT_SEQUENTIAL);
            so.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
            so.AddConfigEntry ("session.intra_op.allow_spinning", "0");
            so.DisableCpuMemArena();
            enc = std::make_unique<Ort::Session> (*rt.env, BinaryData::enc_onnx, (size_t) BinaryData::enc_onnxSize, so);
            erb = std::make_unique<Ort::Session> (*rt.env, BinaryData::erb_dec_onnx, (size_t) BinaryData::erb_dec_onnxSize, so);
            df  = std::make_unique<Ort::Session> (*rt.env, BinaryData::df_dec_onnx, (size_t) BinaryData::df_dec_onnxSize, so);
            names (*enc, encIn, encOut);
            names (*erb, erbIn, erbOut);
            names (*df, dfIn, dfOut);
        }
        catch (const std::exception& e) { err = juce::String ("Model load failed: ") + e.what(); return false; }
        return true;
    }

    juce::String describe()
    {
        juce::String s;
        auto one = [&] (const char* nm, Ort::Session& ses)
        {
            s << nm << ":\n";
            Ort::AllocatorWithDefaultOptions al;
            for (size_t i = 0; i < ses.GetInputCount(); ++i)
            {
                auto shape = ses.GetInputTypeInfo (i).GetTensorTypeAndShapeInfo().GetShape();
                s << "  in  " << ses.GetInputNameAllocated (i, al).get() << " [";
                for (auto d : shape) s << (int) d << ",";
                s << "]\n";
            }
            for (size_t i = 0; i < ses.GetOutputCount(); ++i)
            {
                auto shape = ses.GetOutputTypeInfo (i).GetTensorTypeAndShapeInfo().GetShape();
                s << "  out " << ses.GetOutputNameAllocated (i, al).get() << " [";
                for (auto d : shape) s << (int) d << ",";
                s << "]\n";
            }
        };
        one ("enc", *enc); one ("erb_dec", *erb); one ("df_dec", *df);
        return s;
    }

    // featErb: T*32, featSpec: T*96 complex. Outputs: mask T*32, coefs T*96*5 complex.
    bool run (int T, const float* fErb, const std::complex<float>* fSpec, float* outMask, std::complex<float>* outCoefs)
    {
        try
        {
            auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
            specIn.resize ((size_t) T * 2 * kRefDf);
            for (int t = 0; t < T; ++t)
                for (int f = 0; f < kRefDf; ++f)
                {
                    const auto c = fSpec[(size_t) t * kRefDf + (size_t) f];
                    specIn[(size_t) t * kRefDf + (size_t) f] = c.real();
                    specIn[(size_t) (T + t) * kRefDf + (size_t) f] = c.imag();
                }
            const int64_t shErb[4] = { 1, 1, T, kRefErb };
            const int64_t shSpec[4] = { 1, 2, T, kRefDf };
            std::vector<Ort::Value> inputs;
            std::vector<const char*> inNames;
            for (auto& n : encIn)
            {
                if (n == "feat_erb") inputs.push_back (Ort::Value::CreateTensor<float> (mem, const_cast<float*> (fErb), (size_t) T * kRefErb, shErb, 4));
                else if (n == "feat_spec") inputs.push_back (Ort::Value::CreateTensor<float> (mem, specIn.data(), specIn.size(), shSpec, 4));
                else throw std::runtime_error ("unexpected encoder input " + n);
                inNames.push_back (n.c_str());
            }
            std::vector<const char*> outNames;
            for (auto& n : encOut) outNames.push_back (n.c_str());
            auto encRes = enc->Run (Ort::RunOptions { nullptr }, inNames.data(), inputs.data(), inputs.size(), outNames.data(), outNames.size());

            auto feed = [&] (Ort::Session& ses, const std::vector<std::string>& in, const std::vector<std::string>& out)
            {
                std::vector<const char*> ni, no;
                std::vector<Ort::Value> vals;
                for (auto& n : in)
                {
                    size_t j = 0;
                    while (j < encOut.size() && encOut[j] != n) ++j;
                    if (j == encOut.size()) throw std::runtime_error ("decoder input not produced by encoder: " + n);
                    // borrow the encoder's tensor without copying
                    auto info = encRes[j].GetTensorTypeAndShapeInfo();
                    auto shape = info.GetShape();
                    vals.push_back (Ort::Value::CreateTensor<float> (mem, encRes[j].GetTensorMutableData<float>(), info.GetElementCount(), shape.data(), shape.size()));
                    ni.push_back (n.c_str());
                }
                for (auto& n : out) no.push_back (n.c_str());
                return ses.Run (Ort::RunOptions { nullptr }, ni.data(), vals.data(), vals.size(), no.data(), no.size());
            };

            auto erbRes = feed (*erb, erbIn, erbOut);
            auto dfRes = feed (*df, dfIn, dfOut);

            // gains: [1,1,T,32]
            {
                const float* m = erbRes[0].GetTensorData<float>();
                std::copy (m, m + (size_t) T * kRefErb, outMask);
            }
            // coefficients: [1,T,96,10] = (order, re/im) interleaved
            {
                const float* c = dfRes[0].GetTensorData<float>();
                const size_t n = (size_t) T * kRefDf * kRefOrder;
                for (size_t i = 0; i < n; ++i) outCoefs[i] = { c[2 * i], c[2 * i + 1] };
            }
        }
        catch (const std::exception& e)
        {
            lastError = e.what();
            return false;
        }
        return true;
    }

    juce::String lastError;

private:
    static void names (Ort::Session& s, std::vector<std::string>& in, std::vector<std::string>& out)
    {
        Ort::AllocatorWithDefaultOptions al;
        in.clear(); out.clear();
        for (size_t i = 0; i < s.GetInputCount(); ++i) in.emplace_back (s.GetInputNameAllocated (i, al).get());
        for (size_t i = 0; i < s.GetOutputCount(); ++i) out.emplace_back (s.GetOutputNameAllocated (i, al).get());
    }

    std::unique_ptr<Ort::Session> enc, erb, df;
    std::vector<std::string> encIn, encOut, erbIn, erbOut, dfIn, dfOut;
    std::vector<float> specIn;
};


bool ortReference (const std::vector<float>& erb, const std::vector<float>& spec, std::vector<float>& mask, std::vector<float>& coefs, juce::String& err)
{
    static DfnModel model;
    static bool ok = model.load (err);
    if (! ok) return false;
    const int T = (int) (erb.size() / kRefErb);
    std::vector<std::complex<float>> fs ((size_t) T * kRefDf), co ((size_t) T * kRefDf * kRefOrder);
    for (size_t i = 0; i < fs.size(); ++i) fs[i] = { spec[2 * i], spec[2 * i + 1] };
    mask.assign ((size_t) T * kRefErb, 0.0f);
    if (! model.run (T, erb.data(), fs.data(), mask.data(), co.data())) { err = model.lastError; return false; }
    coefs.resize (co.size() * 2);
    for (size_t i = 0; i < co.size(); ++i) { coefs[2 * i] = co[i].real(); coefs[2 * i + 1] = co[i].imag(); }
    return true;
}
} // namespace vs
