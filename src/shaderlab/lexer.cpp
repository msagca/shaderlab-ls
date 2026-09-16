#include "shaderlab/lexer.h"

#include <cctype>

#include "common/util.h"

namespace sls {

const char* blockKeyword(BlockKind kind) {
  switch (kind) {
    case BlockKind::HlslProgram: return "HLSLPROGRAM";
    case BlockKind::HlslInclude: return "HLSLINCLUDE";
    case BlockKind::CgProgram: return "CGPROGRAM";
    case BlockKind::CgInclude: return "CGINCLUDE";
    case BlockKind::GlslProgram: return "GLSLPROGRAM";
  }
  return "";
}

namespace {

struct BlockSpec {
  std::string_view begin;
  std::string_view end;
  BlockKind kind;
};

constexpr BlockSpec kBlocks[] = {
    {"HLSLPROGRAM", "ENDHLSL", BlockKind::HlslProgram},
    {"HLSLINCLUDE", "ENDHLSL", BlockKind::HlslInclude},
    {"CGPROGRAM", "ENDCG", BlockKind::CgProgram},
    {"CGINCLUDE", "ENDCG", BlockKind::CgInclude},
    {"GLSLPROGRAM", "ENDGLSL", BlockKind::GlslProgram},
};

// Finds `word` as a whole identifier at or after `from`.
size_t findWord(std::string_view source, std::string_view word, size_t from) {
  while (true) {
    size_t at = source.find(word, from);
    if (at == std::string_view::npos) return at;
    bool startOk = at == 0 || !isIdentChar(source[at - 1]);
    size_t after = at + word.size();
    bool endOk = after >= source.size() || !isIdentChar(source[after]);
    if (startOk && endOk) return at;
    from = at + 1;
  }
}

}  // namespace

std::vector<Token> lexShaderLab(std::string_view src, bool keepComments) {
  std::vector<Token> tokens;
  size_t i = src.substr(0, 3) == "\xEF\xBB\xBF" ? 3 : 0;
  auto push = [&](Tok kind, size_t begin, size_t end) {
    Token token;
    token.kind = kind;
    token.span = {begin, end};
    token.text = src.substr(begin, end - begin);
    tokens.push_back(token);
    return &tokens.back();
  };

  while (i < src.size()) {
    char c = src[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      size_t begin = i;
      while (i < src.size() && src[i] != '\n') ++i;
      size_t end = i;
      if (end > begin && src[end - 1] == '\r') --end;
      if (keepComments) push(Tok::Comment, begin, end);
      continue;
    }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      size_t begin = i;
      size_t close = src.find("*/", i + 2);
      i = close == std::string_view::npos ? src.size() : close + 2;
      if (keepComments) push(Tok::Comment, begin, i)->terminated = close != std::string_view::npos;
      continue;
    }
    size_t begin = i;
    if (c == '"') {
      ++i;
      bool terminated = false;
      while (i < src.size() && src[i] != '\n') {
        if (src[i] == '\\' && i + 1 < src.size()) {
          i += 2;
          continue;
        }
        if (src[i] == '"') {
          ++i;
          terminated = true;
          break;
        }
        ++i;
      }
      push(Tok::String, begin, i)->stringTerminated = terminated;
      continue;
    }
    bool negativeNumber = c == '-' && i + 1 < src.size() &&
                          (std::isdigit(static_cast<unsigned char>(src[i + 1])) || src[i + 1] == '.');
    if (std::isdigit(static_cast<unsigned char>(c)) || negativeNumber ||
        (c == '.' && i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
      if (c == '-') ++i;
      while (i < src.size() && (std::isdigit(static_cast<unsigned char>(src[i])) || src[i] == '.')) ++i;
      if (i < src.size() && (src[i] == 'e' || src[i] == 'E') && i + 1 < src.size() &&
          (std::isdigit(static_cast<unsigned char>(src[i + 1])) || src[i + 1] == '-' || src[i + 1] == '+')) {
        i += 2;
        while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i]))) ++i;
      }
      // "2D", "2DArray", "3D" are identifiers that start with a digit.
      if (i < src.size() && isIdentStart(src[i]) && !negativeNumber) {
        while (i < src.size() && isIdentChar(src[i])) ++i;
        push(Tok::Ident, begin, i);
      } else {
        push(Tok::Number, begin, i);
      }
      continue;
    }
    if (isIdentStart(c)) {
      while (i < src.size() && isIdentChar(src[i])) ++i;
      std::string_view word = src.substr(begin, i - begin);
      const BlockSpec* spec = nullptr;
      for (const BlockSpec& candidate : kBlocks) {
        if (word == candidate.begin) spec = &candidate;
      }
      if (!spec) {
        push(Tok::Ident, begin, i);
        continue;
      }
      size_t endAt = findWord(src, spec->end, i);
      Token* token = push(Tok::CodeBlock, begin, endAt == std::string_view::npos ? src.size() : endAt + spec->end.size());
      token->block = spec->kind;
      if (endAt == std::string_view::npos) {
        token->terminated = false;
        token->content = {i, src.size()};
        token->endKeyword = {src.size(), src.size()};
        i = src.size();
      } else {
        token->content = {i, endAt};
        token->endKeyword = {endAt, endAt + spec->end.size()};
        i = endAt + spec->end.size();
      }
      continue;
    }
    Tok kind = Tok::Unknown;
    switch (c) {
      case '{': kind = Tok::LBrace; break;
      case '}': kind = Tok::RBrace; break;
      case '(': kind = Tok::LParen; break;
      case ')': kind = Tok::RParen; break;
      case '[': kind = Tok::LBracket; break;
      case ']': kind = Tok::RBracket; break;
      case '=': kind = Tok::Equals; break;
      case ',': kind = Tok::Comma; break;
      case ':': kind = Tok::Colon; break;
      case '+': kind = Tok::Plus; break;
      default: break;
    }
    // Consume a whole UTF-8 sequence for unknown characters.
    ++i;
    if (kind == Tok::Unknown) {
      while (i < src.size() && (static_cast<unsigned char>(src[i]) & 0xC0) == 0x80) ++i;
    }
    push(kind, begin, i);
  }
  Token end;
  end.kind = Tok::End;
  end.span = {src.size(), src.size()};
  tokens.push_back(end);
  return tokens;
}

std::string stringValue(const Token& token) {
  std::string_view text = token.text;
  if (!text.empty() && text.front() == '"') text.remove_prefix(1);
  if (token.stringTerminated && !text.empty() && text.back() == '"') text.remove_suffix(1);
  std::string value;
  value.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == '"' || text[i + 1] == '\\')) ++i;
    value.push_back(text[i]);
  }
  return value;
}

}  // namespace sls
