#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "shaderlab/model.h"

namespace sls {

enum class Tok {
  End,
  Ident,
  Number,
  String,
  LBrace,
  RBrace,
  LParen,
  RParen,
  LBracket,
  RBracket,
  Equals,
  Comma,
  Colon,
  Plus,
  CodeBlock,
  Comment,  // only with keepComments
  Unknown,
};

struct Token {
  Tok kind = Tok::End;
  Span span;
  std::string_view text;
  // CodeBlock only.
  BlockKind block = BlockKind::HlslProgram;
  Span content;
  Span endKeyword;
  bool terminated = true;  // also set for Comment tokens: false for an unclosed /* comment
  // String only.
  bool stringTerminated = true;
};

// Tokenizes ShaderLab. HLSLPROGRAM...ENDHLSL style blocks become one token. Comments are skipped unless
// keepComments is set.
std::vector<Token> lexShaderLab(std::string_view source, bool keepComments = false);

// Unquotes a String token.
std::string stringValue(const Token& token);

}  // namespace sls
