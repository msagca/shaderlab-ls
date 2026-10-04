#include "hlsl/scopes.h"
#include <map>
#include <string>
#include <vector>
#include "hlsl/builtins.h"
#include "hlsl/lexer.h"
namespace sls {
namespace {
  struct Scope {
    enum Kind { Block,
      Header, // a for/if/while/switch's parentheses
      Statement // the statement after a header, when it is not a block
    } kind;
    int depth; // braces open around it; a Block's own brace counts
    int parens; // parentheses open around it
    std::map<std::string, size_t, std::less<>> names;
  };
} // namespace
std::unordered_map<size_t, size_t> localBindings(std::string_view text, Span range) {
  std::unordered_map<size_t, size_t> bindings;
  std::vector<HlslToken> tokens;
  for (const HlslToken &token : lexHlsl(text, range)) {
    if (token.kind != HlslTok::Comment && token.kind != HlslTok::Directive)
      tokens.push_back(token);
  }
  auto str = [&](size_t k) { return text.substr(tokens[k].span.begin, tokens[k].span.end - tokens[k].span.begin); };
  auto punct = [&](size_t k, char c) { return k < tokens.size() && tokens[k].kind == HlslTok::Punct && text[tokens[k].span.begin] == c; };
  auto ident = [&](size_t k) { return k < tokens.size() && tokens[k].kind == HlslTok::Ident; };
  int depth = 0;
  int parens = 0;
  bool signature = false; // in a function's parameter list at file scope
  bool signed_ = false; // a parameter list closed at file scope; its body not opened yet
  std::map<std::string, size_t, std::less<>> parameters;
  std::vector<Scope> scopes; // empty outside a function body
  bool awaitingBody = false; // a header just closed: a '{' now opens its block
  int declaring = -1; // the parenthesis level of the declaration statement being read, or -1
  auto declare = [&](size_t k) {
    size_t at = tokens[k].span.begin;
    bindings[at] = at;
    (scopes.empty() ? parameters : scopes.back().names)[std::string(str(k))] = at;
  };
  auto typedBefore = [&](size_t k) {
    return k > 0 && ((ident(k - 1) && !hlsl::findKeyword(str(k - 1))) || punct(k - 1, '>'));
  };
  auto declaratorAfter = [&](size_t k) {
    return punct(k + 1, ',') || punct(k + 1, ';') || punct(k + 1, '=') || punct(k + 1, ')') || punct(k + 1, ':') || punct(k + 1, '[');
  };
  for (size_t k = 0; k < tokens.size(); ++k) {
    if (awaitingBody && !punct(k, '{')) {
      scopes.back().kind = Scope::Statement;
      awaitingBody = false;
    }
    if (tokens[k].kind == HlslTok::Punct) {
      char c = text[tokens[k].span.begin];
      if (c == '(') {
        // A function's parameter list: a name with a return type before it, which `register(t0)` and
        // `[numthreads(8, 8, 1)]` don't have.
        if (depth == 0 && parens == 0 && k > 0 && ident(k - 1) && typedBefore(k - 1)) {
          signature = true;
          parameters.clear();
        } else if (!scopes.empty() && k > 0 && ident(k - 1)) {
          std::string_view keyword = str(k - 1);
          if (keyword == "for" || keyword == "if" || keyword == "while" || keyword == "switch")
            scopes.push_back({Scope::Header, depth, parens, {}});
        }
        ++parens;
      } else if (c == ')') {
        parens = std::max(parens - 1, 0);
        if (signature && parens == 0) {
          signature = false;
          signed_ = true;
        }
        if (!scopes.empty() && scopes.back().kind == Scope::Header && scopes.back().parens == parens && scopes.back().depth == depth)
          awaitingBody = true;
      } else if (c == '{') {
        if (awaitingBody) {
          scopes.back().kind = Scope::Block;
          scopes.back().depth = depth + 1;
          awaitingBody = false;
        } else if (depth == 0 && signed_) {
          scopes.push_back({Scope::Block, 1, 0, std::move(parameters)});
          parameters.clear();
        } else if (!scopes.empty()) {
          scopes.push_back({Scope::Block, depth + 1, parens, {}});
        }
        ++depth;
        signed_ = false;
        declaring = -1;
      } else if (c == '}') {
        depth = std::max(depth - 1, 0);
        while (!scopes.empty() && scopes.back().depth > depth)
          scopes.pop_back();
        // A statement that was a block's last ends with it: `if (x) { ... }` leaves nothing open.
        while (!scopes.empty() && scopes.back().kind == Scope::Statement && scopes.back().depth == depth)
          scopes.pop_back();
        declaring = -1;
      } else if (c == ';') {
        if (depth == 0 && parens == 0)
          signed_ = signature = false;
        while (!scopes.empty() && scopes.back().kind == Scope::Statement && scopes.back().depth == depth && scopes.back().parens == parens)
          scopes.pop_back();
        if (declaring == parens)
          declaring = -1;
      }
      continue;
    }
    if (!ident(k) || (k > 0 && punct(k - 1, '.')))
      continue;
    if (signature) {
      // A parameter: at the list's own level, not inside a default value.
      if (parens == 1 && typedBefore(k) && declaratorAfter(k))
        declare(k);
      continue;
    }
    if (scopes.empty())
      continue;
    bool declarator = declaratorAfter(k) && !punct(k + 1, ')') && (typedBefore(k) || (declaring == parens && punct(k - 1, ',')));
    // In a header, `for (int i = 0; ...)`: a ')' may follow the name too.
    if (!declarator && scopes.back().kind == Scope::Header && parens == scopes.back().parens + 1 && typedBefore(k) && declaratorAfter(k))
      declarator = true;
    if (declarator) {
      declare(k);
      declaring = parens;
      continue;
    }
    if (punct(k + 1, '('))
      continue; // a call names a function
    std::string_view name = str(k);
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      if (auto it = scope->names.find(name); it != scope->names.end()) {
        bindings[tokens[k].span.begin] = it->second;
        break;
      }
    }
  }
  return bindings;
}
} // namespace sls
