#include "hlsl/scanner.h"
#include <algorithm>
#include <cctype>
#include "common/util.h"
namespace sls {
namespace {
  enum class TK { Ident,
    Number,
    String,
    Punct };
  struct T {
    TK kind;
    Span span;
    std::string_view text;
  };
  std::string collapse(std::string_view text) {
    std::string result;
    bool space = false;
    for (char c : trim(text)) {
      if (std::isspace(static_cast<unsigned char>(c))) {
        space = true;
        continue;
      }
      if (space && !result.empty())
        result.push_back(' ');
      space = false;
      result.push_back(c);
    }
    return result;
  }
  bool isPunct(const T &token, char c) {
    return token.kind == TK::Punct && token.text[0] == c;
  }
  class Scanner {
  public:
    Scanner(std::string_view source, Span range)
      : src_(source), range_(range) {}
    HlslScan run() {
      tokenize();
      walk();
      return std::move(out_);
    }
  private:
    // ---- tokenizer + directives ----------------------------------------------
    void tokenize() {
      size_t i = range_.begin;
      size_t end = std::min(range_.end, src_.size());
      bool lineStart = true;
      while (i < end) {
        char c = src_[i];
        if (c == '\n') {
          lineStart = true;
          ++i;
          continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
          ++i;
          continue;
        }
        if (c == '/' && i + 1 < end && src_[i + 1] == '/') {
          while (i < end && src_[i] != '\n')
            ++i;
          continue;
        }
        if (c == '/' && i + 1 < end && src_[i + 1] == '*') {
          size_t close = src_.find("*/", i + 2);
          i = close == std::string_view::npos || close + 2 > end ? end : close + 2;
          continue;
        }
        if (c == '#' && lineStart) {
          i = directive(i, end);
          continue;
        }
        lineStart = false;
        size_t begin = i;
        if (c == '"' || c == '\'') {
          ++i;
          while (i < end && src_[i] != c && src_[i] != '\n') {
            if (src_[i] == '\\')
              ++i;
            ++i;
          }
          if (i < end && src_[i] == c)
            ++i;
          tokens_.push_back({TK::String, {begin, i}, src_.substr(begin, i - begin)});
        } else if (isIdentStart(c)) {
          while (i < end && isIdentChar(src_[i]))
            ++i;
          tokens_.push_back({TK::Ident, {begin, i}, src_.substr(begin, i - begin)});
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
          while (i < end && (isIdentChar(src_[i]) || src_[i] == '.'))
            ++i;
          tokens_.push_back({TK::Number, {begin, i}, src_.substr(begin, i - begin)});
        } else {
          ++i;
          tokens_.push_back({TK::Punct, {begin, i}, src_.substr(begin, 1)});
        }
      }
    }
    // Parses a preprocessor directive starting at '#'; returns the offset after it.
    size_t directive(size_t hash, size_t end) {
      size_t lineEnd = hash;
      while (lineEnd < end) {
        if (src_[lineEnd] == '\n') {
          size_t back = lineEnd;
          while (back > hash && (src_[back - 1] == '\r'))
            --back;
          if (back > hash && src_[back - 1] == '\\') {
            ++lineEnd;
            continue;
          }
          break;
        }
        ++lineEnd;
      }
      size_t i = hash + 1;
      auto skipSpaces = [&] {
        while (i < lineEnd && (src_[i] == ' ' || src_[i] == '\t'))
          ++i;
      };
      auto word = [&]() -> Span {
        size_t begin = i;
        while (i < lineEnd && isIdentChar(src_[i]))
          ++i;
        return {begin, i};
      };
      skipSpaces();
      Span nameSpan = word();
      std::string_view name = src_.substr(nameSpan.begin, nameSpan.end - nameSpan.begin);
      Span whole{hash, lineEnd};
      if (!whole.empty() && src_[whole.end - 1] == '\r')
        --whole.end;
      if (name == "pragma") {
        skipSpaces();
        HlslPragma pragma;
        pragma.nameSpan = word();
        pragma.name = std::string(src_.substr(pragma.nameSpan.begin, pragma.nameSpan.end - pragma.nameSpan.begin));
        pragma.span = whole;
        while (true) {
          while (i < whole.end && std::isspace(static_cast<unsigned char>(src_[i])))
            ++i;
          if (i >= whole.end || (src_[i] == '/' && i + 1 < whole.end && src_[i + 1] == '/'))
            break;
          size_t begin = i;
          while (i < whole.end && !std::isspace(static_cast<unsigned char>(src_[i])))
            ++i;
          pragma.args.push_back({std::string(src_.substr(begin, i - begin)), {begin, i}});
        }
        if (!pragma.name.empty())
          out_.pragmas.push_back(std::move(pragma));
      } else if (name == "include" || name == "include_with_pragmas") {
        skipSpaces();
        if (i < whole.end && (src_[i] == '"' || src_[i] == '<')) {
          char close = src_[i] == '"' ? '"' : '>';
          size_t begin = ++i;
          while (i < whole.end && src_[i] != close)
            ++i;
          HlslInclude include;
          include.path = std::string(src_.substr(begin, i - begin));
          include.pathSpan = {begin, i};
          include.span = whole;
          include.withPragmas = name == "include_with_pragmas";
          out_.includes.push_back(std::move(include));
        }
      } else if (name == "define") {
        skipSpaces();
        Span macro = word();
        if (!macro.empty()) {
          HlslDecl decl;
          decl.kind = DeclKind::Macro;
          decl.name = std::string(src_.substr(macro.begin, macro.end - macro.begin));
          decl.nameSpan = macro;
          if (i < whole.end && src_[i] == '(') {
            size_t k = i + 1;
            while (k < whole.end && (src_[k] == ' ' || src_[k] == '\t'))
              ++k;
            decl.zeroParams = k < whole.end && src_[k] == ')';
          }
          std::string text = collapse(src_.substr(whole.begin, whole.end - whole.begin));
          if (text.size() > 200)
            text = text.substr(0, 200) + " ...";
          decl.detail = std::move(text);
          out_.decls.push_back(std::move(decl));
        }
      }
      return lineEnd;
    }
    // ---- declarations --------------------------------------------------------
    enum class Frame { Other,
      Struct,
      CBuffer,
      Function,
      Initializer };
    void walk() {
      std::vector<const T *> statement;
      std::vector<const T *> member;
      std::vector<std::pair<Frame, std::string>> frames;
      std::string macroCBuffer;
      for (size_t k = 0; k < tokens_.size(); ++k) {
        const T &token = tokens_[k];
        int depth = static_cast<int>(frames.size());
        if (depth == 0 && token.kind == TK::Ident && token.text == "CBUFFER_START" && k + 3 < tokens_.size() &&
            isPunct(tokens_[k + 1], '(') && tokens_[k + 2].kind == TK::Ident && isPunct(tokens_[k + 3], ')')) {
          const T &name = tokens_[k + 2];
          addDecl(DeclKind::CBuffer, name, "cbuffer " + std::string(name.text), "");
          macroCBuffer = std::string(name.text);
          statement.clear();
          k += 3;
          continue;
        }
        if (depth == 0 && token.kind == TK::Ident && token.text == "CBUFFER_END") {
          macroCBuffer.clear();
          statement.clear();
          continue;
        }
        if (isPunct(token, '{')) {
          Frame frame = Frame::Other;
          std::string name;
          if (depth == 0) {
            frame = classifyBlock(statement, token, name);
            if (frame != Frame::Initializer)
              statement.clear();
          }
          frames.emplace_back(frame, name);
          member.clear();
          continue;
        }
        if (isPunct(token, '}')) {
          if (frames.empty())
            continue;
          Frame frame = frames.back().first;
          frames.pop_back();
          if (frames.empty() && frame != Frame::Initializer)
            statement.clear();
          member.clear();
          continue;
        }
        if (depth == 0) {
          if (isPunct(token, ';')) {
            finishVariable(statement, macroCBuffer);
            statement.clear();
          } else {
            statement.push_back(&token);
          }
          continue;
        }
        if (depth == 1 && (frames.back().first == Frame::Struct || frames.back().first == Frame::CBuffer)) {
          if (isPunct(token, ';')) {
            DeclKind kind = frames.back().first == Frame::Struct ? DeclKind::Field : DeclKind::Variable;
            addVariables(member, kind, frames.back().second);
            member.clear();
          } else {
            member.push_back(&token);
          }
        }
      }
    }
    Frame classifyBlock(const std::vector<const T *> &statement, const T &brace, std::string &name) {
      if (statement.empty())
        return Frame::Other;
      const T &first = *statement[0];
      if (first.kind == TK::Ident && (first.text == "struct" || first.text == "cbuffer" || first.text == "tbuffer") &&
          statement.size() >= 2 && statement[1]->kind == TK::Ident) {
        bool isStruct = first.text == "struct";
        name = std::string(statement[1]->text);
        addDecl(isStruct ? DeclKind::Struct : DeclKind::CBuffer, *statement[1], std::string(first.text) + " " + name, "");
        return isStruct ? Frame::Struct : Frame::CBuffer;
      }
      int bracket = 0;
      size_t start = statement.size();
      for (size_t i = 0; i < statement.size(); ++i) {
        const T &token = *statement[i];
        if (isPunct(token, '['))
          ++bracket;
        if (isPunct(token, ']'))
          --bracket;
        if (bracket == 0 && start == statement.size() && !isPunct(token, ']'))
          start = i;
        if (isPunct(token, '=') && bracket == 0)
          return Frame::Initializer;
        if (isPunct(token, '(') && bracket == 0) {
          if (i >= start + 2 && statement[i - 1]->kind == TK::Ident) {
            const T &nameToken = *statement[i - 1];
            std::string detail = collapse(src_.substr(statement[start]->span.begin, brace.span.begin - statement[start]->span.begin));
            addDecl(DeclKind::Function, nameToken, detail, "");
            name = std::string(nameToken.text);
            return Frame::Function;
          }
          return Frame::Other;
        }
      }
      return Frame::Other;
    }
    void finishVariable(const std::vector<const T *> &statement, const std::string &container) {
      if (statement.size() < 2)
        return;
      const T &first = *statement[0];
      if (first.kind == TK::Ident && (first.text == "struct" || first.text == "typedef" || first.text == "return"))
        return;
      // Resource macros such as TEXTURE2D(_BaseMap); SAMPLER(sampler_BaseMap);
      if (statement.size() == 4 && first.kind == TK::Ident && isPunct(*statement[1], '(') &&
          statement[2]->kind == TK::Ident && isPunct(*statement[3], ')')) {
        std::string_view macro = first.text;
        bool upper = std::all_of(macro.begin(), macro.end(), [](char c) { return !std::islower(static_cast<unsigned char>(c)); });
        if (upper) {
          addDecl(DeclKind::Variable, *statement[2], collapse(src_.substr(first.span.begin, statement[3]->span.end - first.span.begin)), container);
        }
        return;
      }
      for (const T *token : statement) {
        if (isPunct(*token, '='))
          break;
        if (isPunct(*token, '('))
          return; // prototype or macro invocation
      }
      addVariables(statement, DeclKind::Variable, container);
    }
    // Handles "type a, b[4] : register(t0) = x" style declarations.
    void addVariables(const std::vector<const T *> &statement, DeclKind kind, const std::string &container) {
      if (statement.size() < 2)
        return;
      size_t declEnd = statement.back()->span.end;
      std::string detail = collapse(src_.substr(statement.front()->span.begin, declEnd - statement.front()->span.begin));
      const T *candidate = nullptr;
      bool stopped = false;
      int nesting = 0;
      size_t identCount = 0;
      for (const T *token : statement) {
        if (isPunct(*token, '(') || isPunct(*token, '<'))
          ++nesting;
        if (isPunct(*token, ')') || isPunct(*token, '>'))
          --nesting;
        if (nesting > 0)
          continue;
        if (isPunct(*token, ',')) {
          if (candidate && identCount >= 1)
            addDecl(kind, *candidate, detail, container);
          candidate = nullptr;
          stopped = false;
          continue;
        }
        if (stopped)
          continue;
        if (isPunct(*token, '[') || isPunct(*token, ':') || isPunct(*token, '=')) {
          stopped = true;
          continue;
        }
        if (token->kind == TK::Ident) {
          candidate = token;
          ++identCount;
        }
      }
      if (candidate && identCount >= 2)
        addDecl(kind, *candidate, detail, container);
    }
    void addDecl(DeclKind kind, const T &name, std::string detail, std::string container) {
      HlslDecl decl;
      decl.kind = kind;
      decl.name = std::string(name.text);
      decl.nameSpan = name.span;
      decl.detail = std::move(detail);
      decl.container = std::move(container);
      out_.decls.push_back(std::move(decl));
    }
    std::string_view src_;
    Span range_;
    std::vector<T> tokens_;
    HlslScan out_;
  };
} // namespace
HlslScan scanHlsl(std::string_view source, Span range) {
  return Scanner(source, range).run();
}
} // namespace sls
