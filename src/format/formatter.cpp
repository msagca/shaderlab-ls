#include "format/formatter.h"
#include <algorithm>
#include <optional>
#include <vector>
#include "common/text.h"
#include "common/util.h"
#include "shaderlab/lexer.h"
#include "shaderlab/reference.h"
namespace sls {
namespace {
  enum class Context { File,
    Shader,
    Properties,
    SubShader,
    Pass,
    Stencil,
    Other };
  Context contextFor(std::string_view keyword) {
    if (iequals(keyword, "Shader"))
      return Context::Shader;
    if (iequals(keyword, "Properties"))
      return Context::Properties;
    if (iequals(keyword, "SubShader"))
      return Context::SubShader;
    if (iequals(keyword, "Pass"))
      return Context::Pass;
    if (iequals(keyword, "Stencil"))
      return Context::Stencil;
    return Context::Other;
  }
  std::string_view rtrim(std::string_view text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
      text.remove_suffix(1);
    return text;
  }
  class Formatter {
  public:
    Formatter(std::string_view source, const FormatOptions &options)
      : src_(source), options_(options), lines_(source), tokens_(lexShaderLab(source, true)) {
      tokens_.pop_back(); // End
      if (options_.indentSize < 1)
        options_.indentSize = 1;
      if (options_.tabWidth < 1)
        options_.tabWidth = 1;
    }
    FormatResult run() {
      FormatResult result;
      if (!validate(result.error))
        return result;
      matchBraces();
      for (size_t i = 0; i < tokens_.size(); ++i)
        i = emit(i);
      flush();
      collapseEmptyLines();
      std::string newline = src_.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
      if (src_.substr(0, 3) == "\xEF\xBB\xBF")
        result.text = "\xEF\xBB\xBF"; // the lexer skips it; keep it
      for (const std::string &line : out_) {
        result.text += line;
        result.text += newline;
      }
      result.ok = true;
      return result;
    }
  private:
    bool validate(std::string &error) const {
      int depth = 0;
      for (const Token &token : tokens_) {
        size_t line = lines_.lineOf(token.span.begin) + 1;
        if (token.kind == Tok::LBrace)
          ++depth;
        if (token.kind == Tok::RBrace && --depth < 0) {
          error = "unmatched '}' on line " + std::to_string(line);
          return false;
        }
        if (token.kind == Tok::String && !token.stringTerminated) {
          error = "unterminated string on line " + std::to_string(line);
          return false;
        }
        if ((token.kind == Tok::CodeBlock || token.kind == Tok::Comment) && !token.terminated) {
          error = std::string(token.kind == Tok::Comment ? "unterminated comment" : "unterminated code block") +
                  " starting on line " + std::to_string(line);
          return false;
        }
      }
      if (depth != 0) {
        error = "missing '}'";
        return false;
      }
      return true;
    }
    // A block stays on one line when it was written on one line and holds no code block or comment.
    void matchBraces() {
      inline_.assign(tokens_.size(), false);
      std::vector<size_t> open;
      for (size_t i = 0; i < tokens_.size(); ++i) {
        if (tokens_[i].kind == Tok::LBrace)
          open.push_back(i);
        if (tokens_[i].kind != Tok::RBrace)
          continue;
        size_t begin = open.back();
        open.pop_back();
        bool oneLine = lines_.lineOf(tokens_[begin].span.begin) == lines_.lineOf(tokens_[i].span.begin);
        for (size_t k = begin + 1; k < i && oneLine; ++k) {
          oneLine = tokens_[k].kind != Tok::CodeBlock && tokens_[k].kind != Tok::Comment;
        }
        inline_[begin] = inline_[i] = oneLine;
      }
    }
    Context context() const {
      return scopes_.empty() ? Context::File : scopes_.back().context;
    }
    int depth() const {
      return static_cast<int>(std::count_if(scopes_.begin(), scopes_.end(), [](const Scope &s) { return !s.inlineBlock; }));
    }
    bool startsStatement(const Token &token) const {
      if (token.kind != Tok::Ident)
        return false;
      switch (context()) {
      case Context::File:
        return iequals(token.text, "Shader");
      case Context::Shader:
      case Context::SubShader:
      case Context::Pass:
        return ref::findCommand(token.text) || ref::isScopeKeyword(token.text) || ref::isLegacyCommand(token.text);
      case Context::Stencil:
        return ref::find(ref::stencilFields(), token.text) != nullptr;
      default:
        return false;
      }
    }
    size_t newlinesBetween(size_t from, size_t to) const {
      return static_cast<size_t>(std::count(src_.begin() + from, src_.begin() + to, '\n'));
    }
    bool spaceBetween(const Token &previous, const Token &token) const {
      auto is = [](const Token &t, Tok kind) { return t.kind == kind; };
      if (is(previous, Tok::LParen) || is(previous, Tok::LBracket))
        return false;
      if (is(token, Tok::Comma) || is(token, Tok::RParen) || is(token, Tok::RBracket) || is(token, Tok::Colon))
        return false;
      if (is(previous, Tok::LBrace) && is(token, Tok::RBrace))
        return false;
      if (is(token, Tok::LParen) && is(previous, Tok::Ident)) {
        // Type: `Range(0, 1)`. Property name: `_Color ("Color", Color)`. Anything else keeps its original spacing.
        if (iequals(previous.text, "Range"))
          return false;
        if (context() == Context::Properties)
          return true;
        return token.span.begin > previous.span.end;
      }
      return true;
    }
    void flush() {
      if (!current_.empty())
        out_.emplace_back(rtrim(current_));
      current_.clear();
    }
    // At most MaxEmptyLinesToKeep empty lines in a row, and none at the end of the file. Kept whatever the setting:
    // lines inside a block comment, and a line whose '\' would otherwise make the next line part of a macro.
    void collapseEmptyLines() {
      std::vector<std::string> kept;
      bool comment = false;
      int empty = 0;
      for (std::string &line : out_) {
        bool continuation = !kept.empty() && !kept.back().empty() && kept.back().back() == '\\';
        empty = line.empty() ? empty + 1 : 0;
        if (empty > options_.maxEmptyLines && !comment && !continuation)
          continue;
        comment = inBlockComment(line, comment);
        kept.push_back(std::move(line));
      }
      while (!kept.empty() && kept.back().empty())
        kept.pop_back();
      out_ = std::move(kept);
    }
    void indent(std::string &line, int level, int extraColumns = 0) const {
      if (options_.useTabs) {
        line.append(static_cast<size_t>(level), '\t');
        line.append(static_cast<size_t>(extraColumns / options_.tabWidth), '\t');
        line.append(static_cast<size_t>(extraColumns % options_.tabWidth), ' ');
      } else {
        line.append(static_cast<size_t>(level * options_.indentSize + extraColumns), ' ');
      }
    }
    // Emits tokens_[i] and returns the index of the last token it consumed.
    size_t emit(size_t i) {
      const Token &token = tokens_[i];
      bool closesInline = token.kind == Tok::RBrace && inline_[i];
      bool closesBlock = token.kind == Tok::RBrace && !inline_[i];
      bool opensBlock = token.kind == Tok::LBrace && !inline_[i];
      if (token.kind == Tok::RBrace)
        scopes_.pop_back();
      bool lineStart = current_.empty();
      if (previous_ && inlineLevel_ == 0) {
        bool sourceNewline = lines_.lineOf(previous_->span.end) != lines_.lineOf(token.span.begin);
        bool newLine = forceBreak_ || closesBlock || token.kind == Tok::CodeBlock ||
                       (token.kind == Tok::Comment ? sourceNewline : startsStatement(token) || sourceNewline);
        // The '{' of a block goes where the style puts it, whatever the file did.
        if (opensBlock)
          newLine = forceBreak_ || options_.bracesOnOwnLine;
        if (newLine && !current_.empty()) {
          flush();
          lineStart = true;
        }
        // The empty lines the file had between two statements, as many of them as the style keeps. None right after
        // a '{' or before a '}'.
        if (lineStart && !closesBlock && !afterOpen_ && !out_.empty() && !out_.back().empty()) {
          int blanks = std::min(static_cast<int>(newlinesBetween(previous_->span.end, token.span.begin)) - 1,
            options_.maxEmptyLines);
          for (int blank = 0; blank < blanks; ++blank)
            out_.emplace_back();
        }
      }
      forceBreak_ = false;
      afterOpen_ = false;
      if (lineStart) {
        indent(current_, depth());
        // A '{' moved to its own line still belongs to the statement above it.
        if (token.kind != Tok::LBrace)
          statementKeyword_ = token.kind == Tok::Ident ? std::string(token.text) : std::string();
      } else if (previous_ && spaceBetween(*previous_, token)) {
        current_ += ' ';
      }
      size_t last = i;
      if (token.kind == Tok::CodeBlock) {
        emitCodeBlock(token);
      } else if (token.kind == Tok::LBracket && context() == Context::Properties && (lineStart || lastKind_ == Tok::RBracket)) {
        last = emitAttribute(i);
      } else if (token.kind == Tok::Comment && lineStart && token.text.find('\n') != std::string_view::npos) {
        emitBlockComment(token);
      } else {
        current_ += token.text;
      }
      if (token.kind == Tok::LBrace) {
        scopes_.push_back({contextFor(statementKeyword_), inline_[i]});
        if (inline_[i]) {
          ++inlineLevel_;
        } else {
          forceBreak_ = true;
          afterOpen_ = true;
        }
      }
      if (closesInline)
        --inlineLevel_;
      if (closesBlock || (token.kind == Tok::Comment && token.text.substr(0, 2) == "//"))
        forceBreak_ = true;
      previous_ = &tokens_[last];
      lastKind_ = tokens_[last].kind;
      return last;
    }
    int columns(std::string_view whitespace) const {
      int result = 0;
      for (char c : whitespace) {
        if (c == ' ') {
          ++result;
        } else if (c == '\t') {
          result += options_.tabWidth - result % options_.tabWidth;
        } else {
          break;
        }
      }
      return result;
    }
    // Moves the continuation lines of a multi-line /* */ comment by as much as its first line moved.
    void emitBlockComment(const Token &token) {
      size_t lineBegin = lines_.lineStart(lines_.lineOf(token.span.begin));
      std::string_view before = src_.substr(lineBegin, token.span.begin - lineBegin);
      if (!trim(before).empty()) {
        current_ += token.text;
        return;
      }
      int delta = columns(current_) - columns(before);
      std::string_view text = token.text;
      size_t newline = text.find('\n');
      current_ += text.substr(0, newline + 1);
      for (size_t start = newline + 1; start <= text.size();) {
        size_t end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start + 1);
        if (!trim(line).empty()) {
          if (delta >= 0) {
            current_.append(static_cast<size_t>(delta), ' ');
          } else {
            size_t skip = 0;
            while (skip < line.size() && (line[skip] == ' ' || line[skip] == '\t') &&
                   columns(line.substr(0, skip + 1)) <= -delta) {
              ++skip;
            }
            line.remove_prefix(skip);
          }
        }
        current_ += line;
        if (end == std::string_view::npos)
          break;
        start = end + 1;
      }
    }
    // Property attributes such as [Header(Some text, here)] are copied verbatim: their text is shown in the Inspector.
    size_t emitAttribute(size_t i) {
      const Token &open = tokens_[i];
      int parens = 0;
      size_t close = std::string_view::npos;
      for (size_t k = open.span.end; k < src_.size() && src_[k] != '\n'; ++k) {
        if (src_[k] == '(')
          ++parens;
        if (src_[k] == ')')
          --parens;
        if (src_[k] == ']' && parens <= 0) {
          close = k;
          break;
        }
      }
      if (close == std::string_view::npos) {
        current_ += open.text;
        return i;
      }
      current_ += src_.substr(open.span.begin, close + 1 - open.span.begin);
      size_t last = i;
      while (last + 1 < tokens_.size() && tokens_[last + 1].span.begin <= close)
        ++last;
      return last;
    }
    void emitCodeBlock(const Token &token) {
      std::string_view keyword = src_.substr(token.span.begin, token.content.begin - token.span.begin);
      std::string_view content = src_.substr(token.content.begin, token.content.end - token.content.begin);
      if (content.find('\n') == std::string_view::npos) {
        current_ += src_.substr(token.span.begin, token.span.end - token.span.begin); // single-line block: untouched
        forceBreak_ = true;
        return;
      }
      current_ += keyword;
      flush();
      if (!emitFormattedCode(content))
        emitCodeAsWritten(content);
      indent(current_, depth());
      current_ += src_.substr(token.endKeyword.begin, token.endKeyword.end - token.endKeyword.begin);
      forceBreak_ = true;
    }
    // Lays the code out with clang-format, indented to the block. False when clang-format is not installed or
    // refuses the code, in which case the block keeps the layout it was written with.
    bool emitFormattedCode(std::string_view content) {
      std::optional<std::string> formatted = options_.clangFormat.format(content);
      if (!formatted)
        return false;
      std::string_view text = *formatted;
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.remove_suffix(1);
      for (size_t start = 0; start <= text.size();) {
        size_t end = text.find('\n', start);
        std::string_view line = rtrim(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (line.empty()) {
          out_.emplace_back();
        } else {
          std::string out;
          indent(out, depth());
          out += line;
          out_.push_back(std::move(out));
        }
        if (end == std::string_view::npos)
          break;
        start = end + 1;
      }
      return true;
    }
    // Keeps the code as it is, shifted so that its least indented line lines up with the keyword.
    void emitCodeAsWritten(std::string_view content) {
      std::vector<std::string_view> codeLines;
      for (size_t start = 0;;) {
        size_t end = content.find('\n', start);
        codeLines.push_back(content.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos)
          break;
        start = end + 1;
      }
      // Code after the opening keyword goes on its own line; code before the closing keyword keeps its indentation.
      std::string_view head = trim(codeLines.front());
      std::string_view tail = rtrim(codeLines.back());
      codeLines.erase(codeLines.begin());
      codeLines.pop_back();
      if (!trim(tail).empty())
        codeLines.push_back(tail);
      if (!head.empty()) {
        std::string text;
        indent(text, depth());
        text += head;
        out_.push_back(std::move(text));
      }
      int minimum = -1;
      for (std::string_view line : codeLines) {
        if (trim(line).empty())
          continue;
        int w = columns(line);
        minimum = minimum < 0 ? w : std::min(minimum, w);
      }
      for (std::string_view line : codeLines) {
        if (!line.empty() && line.back() == '\r')
          line.remove_suffix(1);
        std::string_view body = trim(line);
        if (body.empty()) {
          out_.emplace_back();
          continue;
        }
        std::string text;
        indent(text, depth(), columns(line) - minimum);
        // Keep trailing whitespace after a backslash: trimming it would turn the line into a continuation.
        std::string_view rest = line.substr(line.find_first_not_of(" \t"));
        std::string_view trimmed = rtrim(rest);
        text += !trimmed.empty() && trimmed.back() == '\\' && trimmed.size() != rest.size() ? rest : trimmed;
        out_.push_back(std::move(text));
      }
    }
    struct Scope {
      Context context;
      bool inlineBlock;
    };
    std::string_view src_;
    FormatOptions options_;
    LineIndex lines_;
    std::vector<Token> tokens_;
    std::vector<bool> inline_;
    std::vector<Scope> scopes_;
    std::vector<std::string> out_;
    std::string current_;
    std::string statementKeyword_;
    const Token *previous_ = nullptr;
    Tok lastKind_ = Tok::End;
    int inlineLevel_ = 0;
    bool forceBreak_ = false;
    bool afterOpen_ = false;
  };
} // namespace
FormatResult formatShaderLab(std::string_view source, const FormatOptions &options) {
  return Formatter(source, options).run();
}
} // namespace sls
