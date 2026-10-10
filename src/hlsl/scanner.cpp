#include "hlsl/scanner.h"
#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
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
        if (dead_ > 0) {
          // In an #if 0: skip to the next line, where a directive may end it, without reading strings or comments.
          while (i < end && src_[i] != '\n')
            ++i;
          continue;
        }
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
      if (dead_ > 0) {
        // Inside an #if 0 only the nesting counts: an #else or #elif of the #if 0 itself starts code that may be
        // compiled, which then goes on as an #if of its own up to the #endif.
        if (name == "if" || name == "ifdef" || name == "ifndef") {
          ++dead_;
        } else if (name == "endif") {
          --dead_;
        } else if ((name == "else" || name == "elif") && dead_ == 1) {
          dead_ = 0;
          branches_.push_back({tokens_.size(), Branch::If});
        }
        return lineEnd;
      }
      if (name == "if" && isZero(src_.substr(nameSpan.end, lineEnd - nameSpan.end))) {
        dead_ = 1;
        return lineEnd;
      }
      if (name == "if" || name == "ifdef" || name == "ifndef")
        branches_.push_back({tokens_.size(), Branch::If});
      else if (name == "elif" || name == "else")
        branches_.push_back({tokens_.size(), Branch::Else});
      else if (name == "endif")
        branches_.push_back({tokens_.size(), Branch::Endif});
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
          decl.doc = docComment(hash, std::string_view::npos);
          out_.decls.push_back(std::move(decl));
        }
      }
      return lineEnd;
    }
    // An #if expression that is 0, as in #if 0 // old code.
    static bool isZero(std::string_view expression) {
      size_t comment = expression.find("//");
      if (comment != std::string_view::npos)
        expression = expression.substr(0, comment);
      return trim(expression) == "0";
    }
    // ---- declarations --------------------------------------------------------
    enum class Frame { Other,
      Struct,
      CBuffer,
      Function,
      Initializer };
    struct State {
      std::vector<const T *> statement;
      std::vector<const T *> member;
      std::vector<std::pair<Frame, std::string>> frames;
      std::string macroCBuffer;
      // A struct at file scope, from its opening brace until the ';' after its closing one: the names before that
      // ';' are variables of it, as in struct S { ... } gS;, or with typedef, names for it.
      struct TopStruct {
        std::string name; // empty for an anonymous one
        bool isTypedef = false;
        size_t firstDecl = 0; // where its fields start in the declarations
        bool closed = false;
      };
      std::optional<TopStruct> topStruct;
    };
    // Each branch of an #if starts where the #if left off, so that alternatives such as two headers of one function,
    // each opening its body, don't add up; after the #endif the code goes on from where the first branch ended.
    struct Conditional {
      State start;
      std::optional<State> firstEnd;
    };
    void enterBranches(size_t k, size_t &next, State &state, std::vector<Conditional> &open) {
      for (; next < branches_.size() && branches_[next].token <= k; ++next) {
        switch (branches_[next].kind) {
        case Branch::If:
          open.push_back({state, std::nullopt});
          break;
        case Branch::Else:
          if (!open.empty()) {
            if (!open.back().firstEnd)
              open.back().firstEnd = state;
            state = open.back().start;
          }
          break;
        case Branch::Endif:
          if (!open.empty()) {
            if (open.back().firstEnd)
              state = std::move(*open.back().firstEnd);
            open.pop_back();
          }
          break;
        }
      }
    }
    void walk() {
      State state;
      auto &[statement, member, frames, macroCBuffer, topStruct] = state;
      std::vector<Conditional> open;
      size_t next = 0;
      for (size_t k = 0; k < tokens_.size(); ++k) {
        enterBranches(k, next, state, open);
        const T &token = tokens_[k];
        int depth = static_cast<int>(frames.size());
        if (depth == 0 && token.kind == TK::Ident && token.text == "CBUFFER_START" && k + 3 < tokens_.size() &&
            isPunct(tokens_[k + 1], '(') && tokens_[k + 2].kind == TK::Ident && isPunct(tokens_[k + 3], ')')) {
          const T &name = tokens_[k + 2];
          addDecl(DeclKind::CBuffer, name, "cbuffer " + std::string(name.text), "", token.span.begin);
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
            bool isTypedef = !statement.empty() && statement[0]->kind == TK::Ident && statement[0]->text == "typedef";
            frame = classifyBlock(statement, token, name);
            if (frame == Frame::Struct)
              topStruct = State::TopStruct{name, isTypedef, out_.decls.size()};
            else
              topStruct.reset(); // a struct whose ';' never came
            if (frame != Frame::Initializer)
              statement.clear();
          } else if (depth == 1 && frames.back().first == Frame::Struct &&
                     addMethod(member, token.span.begin, frames.back().second)) {
            frame = Frame::Function;
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
          if (frames.empty() && frame == Frame::Struct && topStruct)
            topStruct->closed = true;
          member.clear();
          continue;
        }
        if (depth == 0) {
          if (isPunct(token, ';')) {
            if (topStruct && topStruct->closed)
              finishStruct(statement, *topStruct, macroCBuffer);
            else
              finishVariable(statement, macroCBuffer);
            topStruct.reset();
            statement.clear();
          } else {
            // A macro invocation that brings its own semicolon, or needs none, such as
            // UNITY_INSTANCING_BUFFER_END(Props): what follows it is a declaration of its own.
            if ((token.kind == TK::Ident || isPunct(token, '[')) && isMacroCall(statement)) {
              finishVariable(statement, macroCBuffer, true);
              statement.clear();
            }
            statement.push_back(&token);
          }
          continue;
        }
        if (depth == 1 && (frames.back().first == Frame::Struct || frames.back().first == Frame::CBuffer)) {
          if (isPunct(token, ';')) {
            DeclKind kind = frames.back().first == Frame::Struct ? DeclKind::Field : DeclKind::Variable;
            // A method declared here and defined outside, float Get();, is no field.
            if (kind == DeclKind::Variable || member.empty() || !addMethod(member, member.back()->span.end, frames.back().second))
              addVariables(member, kind, frames.back().second);
            member.clear();
          } else {
            // As at file scope: UNITY_FOG_COORDS(1) and the like need no semicolon.
            if ((token.kind == TK::Ident || isPunct(token, '[')) && isMacroCall(member))
              member.clear();
            member.push_back(&token);
          }
        }
      }
    }
    // The ';' after a struct's closing brace: what is between them names variables of the struct, or with typedef,
    // the struct itself, whose fields then belong to that name if the struct had none of its own.
    void finishStruct(const std::vector<const T *> &statement, const State::TopStruct &top, const std::string &container) {
      std::vector<const T *> names;
      bool stopped = false;
      for (const T *token : statement) {
        if (isPunct(*token, ','))
          stopped = false;
        else if (isPunct(*token, '[') || isPunct(*token, ':') || isPunct(*token, '='))
          stopped = true;
        else if (!stopped && token->kind == TK::Ident)
          names.push_back(token);
      }
      if (names.empty())
        return;
      if (!top.isTypedef) {
        for (const T *name : names)
          addDecl(DeclKind::Variable, *name, "struct " + top.name + " " + std::string(name->text), container, name->span.begin);
        return;
      }
      for (const T *name : names)
        addDecl(DeclKind::Struct, *name, "typedef struct " + std::string(name->text), "", name->span.begin);
      if (top.name.empty()) {
        for (size_t i = top.firstDecl; i < out_.decls.size(); ++i) {
          if (out_.decls[i].kind == DeclKind::Field && out_.decls[i].container.empty())
            out_.decls[i].container = std::string(names[0]->text);
        }
      }
    }
    // A method of `structName`, if `member` is the header of one: its name is the identifier before the first '(',
    // with a type before that, and no '=' or ':' before it, as a field's initializer or semantic has. `end` is where
    // the header ends. Attributes in brackets before it are skipped.
    bool addMethod(const std::vector<const T *> &member, size_t end, const std::string &structName) {
      int bracket = 0;
      size_t start = member.size();
      for (size_t i = 0; i < member.size(); ++i) {
        const T &token = *member[i];
        if (isPunct(token, '['))
          ++bracket;
        if (isPunct(token, ']'))
          --bracket;
        if (bracket != 0 || isPunct(token, ']'))
          continue;
        if (start == member.size())
          start = i;
        if (isPunct(token, '=') || isPunct(token, ':'))
          return false;
        if (isPunct(token, '(')) {
          if (i < start + 2 || member[i - 1]->kind != TK::Ident)
            return false;
          std::string detail = collapse(src_.substr(member[start]->span.begin, end - member[start]->span.begin));
          addDecl(DeclKind::Function, *member[i - 1], detail, structName, member[start]->span.begin);
          return true;
        }
      }
      return false;
    }
    // NAME(...) and nothing more: no declaration goes on after that with another identifier.
    static bool isMacroCall(const std::vector<const T *> &statement) {
      if (statement.size() < 3 || statement[0]->kind != TK::Ident || !isPunct(*statement[1], '('))
        return false;
      int nesting = 0;
      for (size_t i = 1; i < statement.size(); ++i) {
        if (isPunct(*statement[i], '('))
          ++nesting;
        if (isPunct(*statement[i], ')') && --nesting == 0)
          return i + 1 == statement.size();
      }
      return false;
    }
    Frame classifyBlock(const std::vector<const T *> &statement, const T &brace, std::string &name) {
      if (statement.empty())
        return Frame::Other;
      size_t head = statement[0]->kind == TK::Ident && statement[0]->text == "typedef" && statement.size() > 1 ? 1 : 0;
      const T &first = *statement[head];
      if (first.kind == TK::Ident && (first.text == "struct" || first.text == "cbuffer" || first.text == "tbuffer") &&
          statement.size() >= head + 2 && statement[head + 1]->kind == TK::Ident) {
        bool isStruct = first.text == "struct";
        name = std::string(statement[head + 1]->text);
        addDecl(isStruct ? DeclKind::Struct : DeclKind::CBuffer, *statement[head + 1], std::string(first.text) + " " + name, "", first.span.begin);
        return isStruct ? Frame::Struct : Frame::CBuffer;
      }
      if (first.kind == TK::Ident && first.text == "struct" && statement.size() == head + 1)
        return Frame::Struct; // anonymous: typedef struct { ... } Name;
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
            addDecl(DeclKind::Function, nameToken, detail, "", statement[0]->span.begin);
            name = std::string(nameToken.text);
            return Frame::Function;
          }
          return Frame::Other;
        }
      }
      return Frame::Other;
    }
    // `bare`: a macro invocation with no semicolon after it, such as UNITY_INSTANCING_BUFFER_START(Props).
    void finishVariable(const std::vector<const T *> &statement, const std::string &container, bool bare = false) {
      if (statement.size() < 2)
        return;
      const T &first = *statement[0];
      if (first.kind == TK::Ident && (first.text == "struct" || first.text == "return"))
        return;
      if (first.kind == TK::Ident && first.text == "typedef") {
        // typedef float3 Color; typedef float Weights[4];
        const T *name = nullptr;
        for (size_t i = 1; i < statement.size() && !isPunct(*statement[i], '['); ++i) {
          if (statement[i]->kind == TK::Ident)
            name = statement[i];
        }
        if (name && name != statement[1])
          addDecl(DeclKind::Struct, *name, collapse(src_.substr(first.span.begin, statement.back()->span.end - first.span.begin)), "", first.span.begin, statement.back()->span.end);
        return;
      }
      // Resource macros such as TEXTURE2D(_BaseMap); SAMPLER(sampler_BaseMap); and the ones that take the type
      // first, UNITY_DEFINE_INSTANCED_PROP(float4, _Color): an upper-case macro of plain names declares the last.
      if (isMacroCall(statement) && first.kind == TK::Ident) {
        std::string_view macro = first.text;
        bool upper = std::all_of(macro.begin(), macro.end(), [](char c) { return !std::islower(static_cast<unsigned char>(c)); });
        bool names = statement.size() >= 4;
        for (size_t i = 2; i + 1 < statement.size(); ++i)
          names = names && (i % 2 == 0 ? statement[i]->kind == TK::Ident : isPunct(*statement[i], ','));
        const T &name = *statement[statement.size() - 2];
        // A pair such as UNITY_INSTANCING_BUFFER_START(Props) ... _END(Props) declares the name once; with
        // semicolons, the same name in two #if branches is two declarations the variant picks between.
        if (upper && names && (!bare || bareDeclared_.insert(std::string(name.text)).second)) {
          addDecl(DeclKind::Variable, name, collapse(src_.substr(first.span.begin, statement.back()->span.end - first.span.begin)), container, first.span.begin, statement.back()->span.end);
        }
        return;
      }
      for (const T *token : statement) {
        if (isPunct(*token, '=') || isPunct(*token, ':'))
          break; // an initializer or a register(t0): what is before it decides
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
            addDecl(kind, *candidate, detail, container, statement.front()->span.begin, declEnd);
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
        addDecl(kind, *candidate, detail, container, statement.front()->span.begin, declEnd);
    }
    // `begin` is where the declaration starts, and `end` where it ends if a comment after it on the same line should
    // count too.
    void addDecl(DeclKind kind, const T &name, std::string detail, std::string container, size_t begin, size_t end = std::string_view::npos) {
      // A statement an #else picks up again can finish a second time.
      if (!declared_.insert(name.span.begin).second)
        return;
      HlslDecl decl;
      decl.kind = kind;
      decl.name = std::string(name.text);
      decl.nameSpan = name.span;
      decl.detail = std::move(detail);
      decl.container = std::move(container);
      decl.doc = docComment(begin, end);
      out_.decls.push_back(std::move(decl));
    }
    // ---- documentation comments ---------------------------------------------
    // The comment that documents a declaration: the // lines or the /* */ block directly above the line it starts on,
    // with nothing between them, or else a // comment after it on its last line. This is how Unity documents its
    // include files.
    std::string docComment(size_t begin, size_t end) const {
      size_t floor = range_.begin;
      size_t lineStart = begin;
      while (lineStart > floor && src_[lineStart - 1] != '\n')
        --lineStart;
      std::vector<std::string> lines; // bottom-up
      if (trim(src_.substr(lineStart, begin - lineStart)).empty()) {
        size_t pos = lineStart;
        while (pos > floor && lines.size() < kMaxDocLines) {
          size_t prevEnd = pos - 1; // the '\n' ending the line above
          size_t prevStart = prevEnd;
          while (prevStart > floor && src_[prevStart - 1] != '\n')
            --prevStart;
          std::string_view line = trim(src_.substr(prevStart, prevEnd - prevStart));
          if (line.starts_with("//")) {
            lines.emplace_back(commentText(line));
            pos = prevStart;
            continue;
          }
          if (line.ends_with("*/") && lines.empty()) {
            size_t close = prevStart + src_.substr(prevStart, prevEnd - prevStart).rfind("*/");
            size_t open = src_.rfind("/*", close);
            if (open != std::string_view::npos && open >= floor) {
              std::string_view block = src_.substr(open + 2, close - open - 2);
              std::vector<std::string> blockLines;
              size_t at = 0;
              while (at <= block.size()) {
                size_t next = block.find('\n', at);
                std::string_view part = block.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at);
                blockLines.emplace_back(commentText(trim(part)));
                if (next == std::string_view::npos)
                  break;
                at = next + 1;
              }
              lines.assign(blockLines.rbegin(), blockLines.rend());
            }
          }
          break;
        }
      }
      std::reverse(lines.begin(), lines.end());
      if (lines.empty() && end != std::string_view::npos) {
        size_t lineEnd = src_.find('\n', end);
        std::string_view rest = trim(src_.substr(end, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - end));
        while (!rest.empty() && (rest.front() == ';' || rest.front() == ','))
          rest = trim(rest.substr(1));
        if (rest.starts_with("//"))
          lines.emplace_back(commentText(rest));
      }
      // Separator lines such as //----- say nothing; blank lines only matter between paragraphs.
      std::erase_if(lines, [](const std::string &line) {
        return !line.empty() && std::none_of(line.begin(), line.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)); });
      });
      while (!lines.empty() && lines.front().empty())
        lines.erase(lines.begin());
      while (!lines.empty() && lines.back().empty())
        lines.pop_back();
      std::string text;
      for (const std::string &line : lines) {
        if (!text.empty())
          text += line.empty() ? "\n" : (text.back() == '\n' ? "\n" : "  \n");
        text += line;
      }
      return text;
    }
    // A comment line without its markers: "// text", "/// text", "//! text" or " * text" in a block.
    static std::string commentText(std::string_view line) {
      while (!line.empty() && (line.front() == '/' || line.front() == '!' || line.front() == '*'))
        line.remove_prefix(1);
      return std::string(trim(line));
    }
    static constexpr size_t kMaxDocLines = 40;
    std::string_view src_;
    Span range_;
    std::vector<T> tokens_;
    struct Branch {
      size_t token; // the token the directive comes before
      enum Kind { If,
        Else,
        Endif } kind;
    };
    std::vector<Branch> branches_;
    int dead_ = 0; // how deep in an #if 0 the tokenizer is, counting the #if blocks inside it
    std::set<size_t> declared_; // where the names declared so far begin
    std::set<std::string, std::less<>> bareDeclared_; // the names macros with no semicolon after them have declared
    HlslScan out_;
  };
} // namespace
HlslScan scanHlsl(std::string_view source, Span range) {
  return Scanner(source, range).run();
}
} // namespace sls
