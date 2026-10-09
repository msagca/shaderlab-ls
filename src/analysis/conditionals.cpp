#include "analysis/conditionals.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include "common/util.h"
#include "hlsl/lexer.h"
namespace sls {
namespace {
  using Value = std::optional<long long>; // nullopt: not known
  enum class Tri { No,
    Yes,
    Maybe };
  Tri truth(Value value) {
    return !value ? Tri::Maybe : *value != 0 ? Tri::Yes
                                             : Tri::No;
  }
  // Whether `name` is defined, and its value when it is a known number.
  struct Lookup {
    Tri defined;
    Value value;
  };
  Lookup lookup(const MacroState &state, std::string_view name) {
    if (state.unknown.contains(name))
      return {Tri::Maybe, std::nullopt};
    if (auto it = state.defined.find(name); it != state.defined.end())
      return {Tri::Yes, it->second};
    if (state.undefined.contains(name))
      return {Tri::No, 0};
    if (!state.closed || (state.elsewhere && state.elsewhere(name)))
      return {Tri::Maybe, std::nullopt};
    return {Tri::No, 0};
  }
  std::optional<long long> number(std::string_view text) {
    while (!text.empty() && (text.back() == 'u' || text.back() == 'U' || text.back() == 'l' || text.back() == 'L'))
      text.remove_suffix(1);
    if (text.empty())
      return std::nullopt;
    std::string copy(text);
    char *end = nullptr;
    long long value = std::strtoll(copy.c_str(), &end, 0);
    if (end != copy.c_str() + copy.size())
      return std::nullopt;
    return value;
  }
  // A #if expression, evaluated with what is known. Anything it can't parse makes the result unknown.
  class Expression {
  public:
    Expression(std::string_view text, const MacroState &state)
      : text_(text), state_(state) {
      tokenize();
    }
    Value evaluate() {
      Value value = conditional();
      return failed_ || at_ != tokens_.size() ? std::nullopt : value;
    }
  private:
    void tokenize() {
      static const std::string_view kTwo[] = {"&&", "||", "==", "!=", "<=", ">=", "<<", ">>"};
      size_t i = 0;
      while (i < text_.size()) {
        char c = text_[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
          ++i;
        } else if (isIdentStart(c) || std::isdigit(static_cast<unsigned char>(c))) {
          size_t begin = i;
          while (i < text_.size() && isIdentChar(text_[i]))
            ++i;
          tokens_.push_back(text_.substr(begin, i - begin));
        } else {
          size_t length = 1;
          for (std::string_view two : kTwo) {
            if (text_.substr(i, 2) == two)
              length = 2;
          }
          tokens_.push_back(text_.substr(i, length));
          i += length;
        }
      }
    }
    std::string_view peek() const {
      return at_ < tokens_.size() ? tokens_[at_] : std::string_view();
    }
    bool accept(std::string_view token) {
      if (peek() != token)
        return false;
      ++at_;
      return true;
    }
    Value conditional() {
      Value condition = binary(1);
      if (!accept("?"))
        return condition;
      Value yes = conditional();
      if (!accept(":"))
        failed_ = true;
      Value no = conditional();
      Tri which = truth(condition);
      if (which == Tri::Yes)
        return yes;
      if (which == Tri::No)
        return no;
      return yes && no && *yes == *no ? yes : std::nullopt;
    }
    static int precedence(std::string_view op) {
      if (op == "||")
        return 1;
      if (op == "&&")
        return 2;
      if (op == "|")
        return 3;
      if (op == "^")
        return 4;
      if (op == "&")
        return 5;
      if (op == "==" || op == "!=")
        return 6;
      if (op == "<" || op == ">" || op == "<=" || op == ">=")
        return 7;
      if (op == "<<" || op == ">>")
        return 8;
      if (op == "+" || op == "-")
        return 9;
      if (op == "*" || op == "/" || op == "%")
        return 10;
      return 0;
    }
    Value binary(int minimum) {
      Value left = unary();
      while (true) {
        std::string_view op = peek();
        int level = precedence(op);
        if (level == 0 || level < minimum)
          return left;
        ++at_;
        Value right = binary(level + 1);
        left = apply(op, left, right);
      }
    }
    static Value apply(std::string_view op, Value a, Value b) {
      // && and || are known when one side settles them, whatever the other is.
      if (op == "&&") {
        if (truth(a) == Tri::No || truth(b) == Tri::No)
          return 0;
        return a && b ? Value(1) : std::nullopt;
      }
      if (op == "||") {
        if (truth(a) == Tri::Yes || truth(b) == Tri::Yes)
          return 1;
        return a && b ? Value(0) : std::nullopt;
      }
      if (!a || !b)
        return std::nullopt;
      long long x = *a, y = *b;
      if (op == "|")
        return x | y;
      if (op == "^")
        return x ^ y;
      if (op == "&")
        return x & y;
      if (op == "==")
        return x == y;
      if (op == "!=")
        return x != y;
      if (op == "<")
        return x < y;
      if (op == ">")
        return x > y;
      if (op == "<=")
        return x <= y;
      if (op == ">=")
        return x >= y;
      if (op == "<<")
        return y >= 0 && y < 63 ? Value(x << y) : std::nullopt;
      if (op == ">>")
        return y >= 0 && y < 63 ? Value(x >> y) : std::nullopt;
      if (op == "+")
        return x + y;
      if (op == "-")
        return x - y;
      if (op == "*")
        return x * y;
      if (op == "/")
        return y != 0 ? Value(x / y) : std::nullopt;
      if (op == "%")
        return y != 0 ? Value(x % y) : std::nullopt;
      return std::nullopt;
    }
    Value unary() {
      if (accept("!")) {
        Value value = unary();
        return value ? Value(*value == 0) : std::nullopt;
      }
      if (accept("~")) {
        Value value = unary();
        return value ? Value(~*value) : std::nullopt;
      }
      if (accept("-")) {
        Value value = unary();
        return value ? Value(-*value) : std::nullopt;
      }
      if (accept("+"))
        return unary();
      return primary();
    }
    Value primary() {
      std::string_view token = peek();
      if (token.empty()) {
        failed_ = true;
        return std::nullopt;
      }
      ++at_;
      if (token == "(") {
        Value value = conditional();
        if (!accept(")"))
          failed_ = true;
        return value;
      }
      if (std::isdigit(static_cast<unsigned char>(token[0])))
        return number(token);
      if (!isIdentStart(token[0])) {
        failed_ = true;
        return std::nullopt;
      }
      if (token == "defined") {
        bool parenthesized = accept("(");
        std::string_view name = peek();
        if (name.empty() || !isIdentStart(name[0])) {
          failed_ = true;
          return std::nullopt;
        }
        ++at_;
        if (parenthesized && !accept(")"))
          failed_ = true;
        Tri defined = lookup(state_, name).defined;
        return defined == Tri::Maybe ? std::nullopt : Value(defined == Tri::Yes);
      }
      if (peek() == "(") {
        // A function-like macro's call: what it expands to is not worked out.
        int depth = 0;
        do {
          if (peek() == "(")
            ++depth;
          if (peek() == ")")
            --depth;
          ++at_;
        } while (at_ < tokens_.size() && depth > 0);
        return std::nullopt;
      }
      return lookup(state_, token).value;
    }
    std::string_view text_;
    const MacroState &state_;
    std::vector<std::string_view> tokens_;
    size_t at_ = 0;
    bool failed_ = false;
  };
  // A directive's text after its name, on one line, without comments.
  std::string directiveRest(std::string_view text) {
    std::string rest;
    size_t i = 0;
    while (i < text.size()) {
      if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r')) {
        i += 1;
        while (i < text.size() && (text[i] == '\r' || text[i] == '\n'))
          ++i;
        rest.push_back(' ');
        continue;
      }
      if (text.substr(i, 2) == "//")
        break;
      if (text.substr(i, 2) == "/*") {
        size_t close = text.find("*/", i + 2);
        i = close == std::string_view::npos ? text.size() : close + 2;
        rest.push_back(' ');
        continue;
      }
      rest.push_back(text[i]);
      ++i;
    }
    return std::string(trim(rest));
  }
  // A directive's name and the rest of it: "ifdef" and "_FANCY".
  std::pair<std::string_view, std::string> splitDirective(std::string_view text, const HlslToken &token) {
    std::string_view directive = text.substr(token.span.begin + 1, token.span.end - token.span.begin - 1);
    size_t i = 0;
    while (i < directive.size() && (directive[i] == ' ' || directive[i] == '\t'))
      ++i;
    size_t nameBegin = i;
    while (i < directive.size() && isIdentChar(directive[i]))
      ++i;
    return {directive.substr(nameBegin, i - nameBegin), directiveRest(directive.substr(i))};
  }
  std::string leadingName(std::string_view text) {
    size_t end = 0;
    while (end < text.size() && isIdentChar(text[end]))
      ++end;
    return std::string(text.substr(0, end));
  }
  std::string withoutSpaces(std::string_view text) {
    std::string result;
    for (char c : text) {
      if (!std::isspace(static_cast<unsigned char>(c)))
        result.push_back(c);
    }
    return result;
  }
  // Whether text[begin, end) is wrapped in one pair of parentheses: "(a && b)" is, "(a) && (b)" is not.
  bool parenthesized(std::string_view text) {
    if (text.size() < 2 || text.front() != '(' || text.back() != ')')
      return false;
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      depth += text[i] == '(' ? 1 : text[i] == ')' ? -1
                                                   : 0;
      if (depth == 0 && i + 1 < text.size())
        return false;
    }
    return true;
  }
  // What a guard tells of the names it tests, for the ones it settles alone: "defined(_A) && !defined(_B)" holding
  // has _A defined and _B not, and "defined(_A) || X" failing has _A not defined. A name it needs to be non-zero is
  // known to be defined, though not its value.
  void learn(const Guard &guard, MacroState &state) {
    std::string text = withoutSpaces(guard.condition);
    while (parenthesized(text))
      text = text.substr(1, text.size() - 2);
    // Holding, it is the parts of an && that each hold; failing, the parts of an || that each fail.
    std::string_view joiner = guard.holds ? "&&" : "||";
    std::string_view other = guard.holds ? "||" : "&&";
    std::vector<std::string> parts;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      depth += text[i] == '(' ? 1 : text[i] == ')' ? -1
                                                   : 0;
      if (depth != 0)
        continue;
      if (text.compare(i, 2, other) == 0)
        return; // not settled by any one name
      if (text.compare(i, 2, joiner) == 0) {
        parts.push_back(text.substr(start, i - start));
        start = i + 2;
        ++i;
      }
    }
    parts.push_back(text.substr(start));
    for (std::string part : parts) {
      bool holds = guard.holds;
      while (true) {
        if (parenthesized(part))
          part = part.substr(1, part.size() - 2);
        else if (!part.empty() && part[0] == '!' && part.compare(0, 2, "!=") != 0) {
          part.erase(0, 1);
          holds = !holds;
        } else
          break;
      }
      bool tested = part.compare(0, 7, "defined") == 0;
      std::string name = leadingName(tested ? std::string_view(part).substr(7) : std::string_view(part));
      if (tested && name.empty() && parenthesized(part.substr(7)))
        name = leadingName(std::string_view(part).substr(8));
      if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])) || name == "defined")
        continue;
      size_t length = tested ? (part.size() > 7 && part[7] == '(' ? name.size() + 9 : name.size() + 7) : name.size();
      if (part.size() != length)
        continue; // compares or computes with it
      if (holds) {
        state.undefined.erase(name);
        state.defined.try_emplace(name, std::nullopt);
      } else if (tested) {
        state.defined.erase(name);
        state.undefined.insert(name);
      }
    }
  }
  struct Frame {
    bool parentLive;
    bool parentCertain;
    Tri taken; // whether a branch before this one, or this one, was taken
    bool live; // this branch may be compiled
    bool certain; // this branch is compiled for certain
  };
} // namespace
std::vector<Span> inactiveRegions(std::string_view text, Span range, MacroState &state) {
  std::vector<Span> regions;
  std::vector<Frame> frames;
  auto live = [&] { return frames.empty() || frames.back().live; };
  auto certain = [&] { return frames.empty() || frames.back().certain; };
  size_t inactiveSince = 0;
  auto lineStart = [&](size_t offset) {
    while (offset > range.begin && text[offset - 1] != '\n')
      --offset;
    return offset;
  };
  auto nextLine = [&](size_t offset) {
    size_t newline = text.find('\n', offset);
    return newline == std::string_view::npos || newline >= range.end ? range.end : newline + 1;
  };
  for (const HlslToken &token : lexHlsl(text, range)) {
    if (token.kind != HlslTok::Directive)
      continue;
    auto [name, rest] = splitDirective(text, token);
    bool wasLive = live();
    if (name == "if" || name == "ifdef" || name == "ifndef") {
      Tri condition = Tri::Maybe;
      if (wasLive) {
        if (name == "if") {
          condition = truth(Expression(rest, state).evaluate());
        } else {
          size_t end = 0;
          while (end < rest.size() && isIdentChar(rest[end]))
            ++end;
          condition = lookup(state, std::string_view(rest).substr(0, end)).defined;
          if (name == "ifndef" && condition != Tri::Maybe)
            condition = condition == Tri::Yes ? Tri::No : Tri::Yes;
        }
      }
      frames.push_back({wasLive, certain(), condition, wasLive && condition != Tri::No, certain() && condition == Tri::Yes});
    } else if ((name == "elif" || name == "else") && !frames.empty()) {
      Frame &frame = frames.back();
      Tri condition = Tri::Yes;
      if (name == "elif" && frame.parentLive && frame.taken != Tri::Yes)
        condition = truth(Expression(rest, state).evaluate());
      frame.live = frame.parentLive && frame.taken != Tri::Yes && condition != Tri::No;
      frame.certain = frame.parentCertain && frame.taken == Tri::No && condition == Tri::Yes;
      if (frame.taken != Tri::Yes)
        frame.taken = condition == Tri::Yes ? Tri::Yes : (frame.taken == Tri::No && condition == Tri::No ? Tri::No : Tri::Maybe);
    } else if (name == "endif" && !frames.empty()) {
      frames.pop_back();
    } else if ((name == "define" || name == "undef") && wasLive) {
      size_t end = 0;
      while (end < rest.size() && isIdentChar(rest[end]))
        ++end;
      std::string macro = rest.substr(0, end);
      if (!macro.empty()) {
        state.defined.erase(macro);
        state.undefined.erase(macro);
        state.unknown.erase(macro);
        if (!certain()) {
          state.unknown.insert(macro); // in code that may not be compiled
        } else if (name == "undef") {
          state.undefined.insert(macro);
        } else {
          bool functionLike = end < rest.size() && rest[end] == '(';
          state.defined[macro] = functionLike ? std::nullopt : number(trim(std::string_view(rest).substr(end)));
        }
      }
    }
    bool nowLive = live();
    if (wasLive && !nowLive)
      inactiveSince = nextLine(token.span.end);
    if (!wasLive && nowLive) {
      size_t end = lineStart(token.span.begin);
      if (end > inactiveSince)
        regions.push_back({inactiveSince, end});
    }
  }
  if (!live() && range.end > inactiveSince)
    regions.push_back({inactiveSince, range.end});
  return regions;
}
std::vector<Guard> guardsAt(std::string_view text, Span range, size_t offset) {
  struct Block {
    std::vector<Guard> earlier; // a guard for each branch so far, holding where that branch is compiled
    std::vector<Guard> branch; // the guards of the branch `offset` would be in
    size_t opened; // the directive that opened it, counted from the start
    bool includeGuard = false; // #ifndef X or #if !defined(X), followed by #define X
  };
  std::vector<Block> blocks;
  size_t count = 0;
  for (const HlslToken &token : lexHlsl(text, range)) {
    if (token.span.begin >= offset)
      break;
    if (token.kind != HlslTok::Directive)
      continue;
    auto [name, rest] = splitDirective(text, token);
    ++count;
    if (name == "if" || name == "ifdef" || name == "ifndef") {
      Guard guard = name == "if" ? Guard{rest, true} : Guard{"defined(" + leadingName(rest) + ")", name == "ifdef"};
      blocks.push_back({{guard}, {guard}, count});
    } else if ((name == "elif" || name == "else") && !blocks.empty()) {
      Block &block = blocks.back();
      block.branch.clear();
      for (const Guard &earlier : block.earlier)
        block.branch.push_back({earlier.condition, !earlier.holds});
      if (name == "elif") {
        block.branch.push_back({rest, true});
        block.earlier.push_back({rest, true});
      }
    } else if (name == "endif" && !blocks.empty()) {
      blocks.pop_back();
    } else if (name == "define" && !blocks.empty()) {
      Block &block = blocks.back();
      if (block.opened + 1 != count || block.earlier.size() != 1)
        continue;
      // The opener needs the macro undefined and nothing else: #ifndef X, or #if !defined(X).
      MacroState opener;
      learn(block.earlier.front(), opener);
      if (opener.defined.empty() && opener.undefined.size() == 1 && opener.undefined.count(leadingName(rest)))
        block.includeGuard = true;
    }
  }
  std::vector<Guard> guards;
  for (const Block &block : blocks) {
    if (!block.includeGuard)
      guards.insert(guards.end(), block.branch.begin(), block.branch.end());
  }
  return guards;
}
GuardFit guardFit(const std::vector<Guard> &guards, const std::vector<Guard> &assumed) {
  MacroState state;
  state.closed = false; // what the assumed guards don't tell is not known
  for (const Guard &guard : assumed)
    learn(guard, state);
  GuardFit fit;
  for (const Guard &guard : guards) {
    bool same = std::any_of(assumed.begin(), assumed.end(), [&](const Guard &other) {
      return other.holds == guard.holds && withoutSpaces(other.condition) == withoutSpaces(guard.condition);
    });
    if (same)
      continue;
    Tri value = truth(Expression(guard.condition, state).evaluate());
    if (value == Tri::Maybe)
      ++fit.unsettled;
    else if ((value == Tri::Yes) != guard.holds)
      fit.excluded = true;
  }
  return fit;
}
} // namespace sls
