#pragma once
#include <string>
#include <string_view>
#include "format/style.h"
namespace sls {
struct FormatResult {
  bool ok = false;
  std::string text;
  std::string error; // why the file was left alone, when !ok
};
// Formats a .shader file:
// - one statement per line in Shader, SubShader, Pass and Stencil blocks, indented by nesting;
// - braces of multi-line blocks on their own lines, blocks written on one line kept on one line;
// - consistent spacing between tokens, at most one blank line in a row;
// - HLSL/CG/GLSL code keeps its layout and is only shifted so its least indented line lines up with the keyword.
// Comments, strings, property attribute text and keyword casing are preserved. Files with unbalanced braces or
// unterminated blocks, strings or comments are not formatted.
FormatResult formatShaderLab(std::string_view source, const FormatOptions &options);
} // namespace sls
