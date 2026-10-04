#pragma once
#include <string_view>
#include <unordered_map>
#include "common/text.h"
namespace sls {
// The parameters and locals of the functions in text[range], resolved by block scope as the compiler would: for
// each identifier that names one, the offset of the name declaring it, a declaring name mapping to itself. A `for`
// or `if` header's declarations belong to its statement. Identifiers not in the map name nothing local: a global,
// a type, a macro or a member - or something a lexical reading can't place, such as a name a macro declares.
std::unordered_map<size_t, size_t> localBindings(std::string_view text, Span range);
} // namespace sls
