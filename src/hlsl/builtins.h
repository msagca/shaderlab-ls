#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sls::hlsl {

const std::vector<std::string>& keywords();
const std::vector<std::string>& types();  // scalar, vector, matrix, sampler and resource types
const std::vector<std::string>& intrinsics();  // intrinsic functions

bool isIntrinsic(std::string_view name);
bool isKeywordOrType(std::string_view name);

}  // namespace sls::hlsl
