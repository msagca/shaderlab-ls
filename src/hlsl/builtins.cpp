#include "hlsl/builtins.h"

#include <algorithm>
#include <unordered_set>

namespace sls::hlsl {

namespace {

// Microsoft HLSL reference: keywords, data types and intrinsic functions (shader model 5).
const char* const kKeywords[] = {
    "break", "case", "cbuffer", "centroid", "class", "column_major", "const", "continue", "default", "discard",
    "do", "else", "export", "extern", "false", "for", "if", "in", "inline", "inout", "interface", "linear",
    "line", "lineadj", "namespace", "nointerpolation", "noperspective", "out", "packoffset", "point",
    "precise", "register", "return", "row_major", "sample", "shared", "snorm", "static", "struct", "switch",
    "tbuffer", "triangle", "triangleadj", "true", "typedef", "uniform", "unorm", "unsigned", "volatile",
    "while",
};

const char* const kResourceTypes[] = {
    "void", "string", "sampler", "SamplerState", "SamplerComparisonState", "sampler1D", "sampler2D", "sampler3D",
    "samplerCUBE", "texture", "Texture1D", "Texture1DArray", "Texture2D", "Texture2DArray", "Texture2DMS",
    "Texture2DMSArray", "Texture3D", "TextureCube", "TextureCubeArray", "Buffer", "StructuredBuffer",
    "RWStructuredBuffer", "AppendStructuredBuffer", "ConsumeStructuredBuffer", "RWBuffer", "RWTexture1D",
    "RWTexture1DArray", "RWTexture2D", "RWTexture2DArray", "RWTexture3D", "InputPatch", "OutputPatch",
    "PointStream", "LineStream", "TriangleStream", "vector", "matrix",
};

const char* const kScalars[] = {"bool", "int", "uint", "dword", "half", "float", "double", "min16float",
                                "min10float", "min16int", "min12int", "min16uint"};

const char* const kIntrinsics[] = {
    "abort", "abs", "acos", "all", "any", "asin", "atan", "atan2", "ceil", "clamp", "clip", "cos", "cosh",
    "countbits", "cross", "D3DCOLORtoUBYTE4", "ddx", "ddx_coarse", "ddx_fine", "ddy", "ddy_coarse", "ddy_fine",
    "degrees", "determinant", "distance", "dot", "dst", "errorf", "EvaluateAttributeAtCentroid",
    "EvaluateAttributeAtSample", "EvaluateAttributeSnapped", "exp", "exp2", "f16tof32", "f32tof16",
    "faceforward", "firstbithigh", "firstbitlow", "floor", "fmod", "frac", "frexp", "fwidth",
    "GetRenderTargetSampleCount", "GetRenderTargetSamplePosition", "isfinite", "isinf", "isnan", "ldexp",
    "length", "lerp", "lit", "log", "log10", "log2", "mad", "max", "min", "modf", "msad4", "mul", "noise",
    "normalize", "pow", "printf", "radians", "reflect", "refract", "reversebits", "round",
    "rsqrt", "saturate", "sign", "sin", "sinh", "smoothstep", "sqrt", "step", "tan", "tanh", "tex1D",
    "tex1Dbias", "tex1Dgrad", "tex1Dlod", "tex1Dproj", "tex2D", "tex2Dbias", "tex2Dgrad", "tex2Dlod",
    "tex2Dproj", "tex3D", "tex3Dbias", "tex3Dgrad", "tex3Dlod", "tex3Dproj", "texCUBE", "texCUBEbias",
    "texCUBEgrad", "texCUBElod", "texCUBEproj", "transpose", "trunc",
};

}  // namespace

const std::vector<std::string>& keywords() {
  static const std::vector<std::string> result(std::begin(kKeywords), std::end(kKeywords));
  return result;
}

const std::vector<std::string>& types() {
  static const std::vector<std::string> result = [] {
    std::vector<std::string> list(std::begin(kResourceTypes), std::end(kResourceTypes));
    for (const char* scalar : kScalars) {
      std::string base = scalar;
      list.push_back(base);
      if (base == "dword") continue;
      for (int rows = 1; rows <= 4; ++rows) {
        list.push_back(base + std::to_string(rows));
        for (int cols = 1; cols <= 4; ++cols) list.push_back(base + std::to_string(rows) + "x" + std::to_string(cols));
      }
    }
    return list;
  }();
  return result;
}

const std::vector<std::string>& intrinsics() {
  static const std::vector<std::string> result(std::begin(kIntrinsics), std::end(kIntrinsics));
  return result;
}

bool isIntrinsic(std::string_view name) {
  const auto& list = intrinsics();
  return std::find(list.begin(), list.end(), name) != list.end();
}

bool isKeywordOrType(std::string_view name) {
  static const std::unordered_set<std::string_view> set = [] {
    std::unordered_set<std::string_view> s;
    for (const auto& k : keywords()) s.insert(k);
    for (const auto& t : types()) s.insert(t);
    return s;
  }();
  return set.contains(name);
}

}  // namespace sls::hlsl
