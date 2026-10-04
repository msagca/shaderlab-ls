#pragma once
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "common/text.h"
namespace sls {
// What the preprocessor is known to have defined at some point in the code. Everything here is three-valued: a
// name is defined, not defined, or not known to be either, and a condition that depends on what is not known is
// neither true nor false. Code is only ever called inactive when its condition is false for certain.
struct MacroState {
  std::map<std::string, std::optional<long long>, std::less<>> defined; // with its value, when it is a number
  std::set<std::string, std::less<>> undefined;
  std::set<std::string, std::less<>> unknown; // defined or #undef'd in code that may or may not be compiled
  // Whether a name in none of the three is not defined (true) or not known (false): a program sees all its code
  // and knows what it defines, an include file is compiled with what its includer defined first.
  bool closed = true;
  // Names that may be defined where this state can't see: in an include file, or by Unity itself.
  std::function<bool(std::string_view)> elsewhere;
};
// Walks the conditional directives of text[range] as the preprocessor would, keeping `state` up to date with the
// #define and #undef of the code it compiles, and returns the code a false condition leaves out: from the line after
// the directive that turns it off to the start of the one that turns it back on.
std::vector<Span> inactiveRegions(std::string_view text, Span range, MacroState &state);
} // namespace sls
