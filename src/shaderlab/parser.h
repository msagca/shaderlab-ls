#pragma once

#include <string_view>

#include "shaderlab/model.h"

namespace sls {

// Parses a .shader file into its ShaderLab structure and reports syntax and value errors.
// HLSL inside code blocks is not inspected here.
ShaderFile parseShaderLab(std::string_view source);

}  // namespace sls
