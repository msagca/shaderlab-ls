#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "analysis/analysis.h"
#include "fxc/fxc.h"

namespace sls {

struct CheckOptions {
  std::filesystem::path editorOverride;
  std::vector<std::string> keywords;  // shader keywords to treat as enabled
  std::vector<FxcDefine> defines;  // extra macros for every compile
};

// Compiles the document's HLSL programs with FXC the way Unity's D3D11 backend would and returns the
// resulting diagnostics mapped back onto the document.
std::vector<Diagnostic> checkHlsl(const Analysis& analysis, const CheckOptions& options,
                                  const std::function<bool()>& cancelled);

}  // namespace sls
