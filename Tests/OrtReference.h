#pragma once
#include <juce_core/juce_core.h>
#include <vector>
namespace vs
{
// DeepFilterNet3 through ONNX Runtime, whole sequence: features [T][32], [T][96 re,im] ->
// gains [T][32], coefficients [T][96][5][re,im]. False (with err) if the runtime can't load.
bool ortReference (const std::vector<float>& erb, const std::vector<float>& spec, std::vector<float>& mask, std::vector<float>& coefs, juce::String& err);
}
