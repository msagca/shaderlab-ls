#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "common/text.h"

namespace sls {

struct PragmaArg {
  std::string text;
  Span span;
};

struct HlslPragma {
  std::string name;
  Span nameSpan;
  std::vector<PragmaArg> args;
  Span span;  // whole directive line
};

struct HlslInclude {
  std::string path;
  Span pathSpan;  // inside the quotes
  Span span;
  bool withPragmas = false;
};

enum class DeclKind { Function, Struct, CBuffer, Variable, Field, Macro };

struct HlslDecl {
  DeclKind kind = DeclKind::Variable;
  std::string name;
  Span nameSpan;
  std::string detail;  // declaration text, whitespace collapsed
  std::string container;  // struct or cbuffer name for fields/members
  bool zeroParams = false;  // macro declared as NAME()
};

struct HlslScan {
  std::vector<HlslPragma> pragmas;
  std::vector<HlslInclude> includes;
  std::vector<HlslDecl> decls;
};

// A light, error-tolerant scan of HLSL in source[range]. It does not evaluate the preprocessor:
// declarations in every #if branch are reported. Spans are offsets into `source`.
HlslScan scanHlsl(std::string_view source, Span range);

}  // namespace sls
