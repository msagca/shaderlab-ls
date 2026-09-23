#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "shaderlab/reference.h"
namespace sls::hlsl {
// Every table here has a signature and description paraphrased from the Microsoft HLSL reference.
std::span<const ref::Entry> keywords();
const std::vector<std::string> &types(); // scalar, vector, matrix, sampler and resource types
std::span<const ref::Entry> intrinsics(); // intrinsic functions
const ref::Entry *findKeyword(std::string_view name);
const ref::Entry *findIntrinsic(std::string_view name);
const ref::Entry *findSemantic(std::string_view name); // case-insensitive, with or without an index: TEXCOORD3
const ref::Entry *findAttribute(std::string_view name); // [unroll], [numthreads(...)], ...
const ref::Entry *findMethod(std::string_view name); // texture and buffer methods: Sample, Load, ...
// A type's description, generated for the scalar, vector and matrix types: float3 is 3 floats.
struct TypeDoc {
  std::string detail;
  std::string doc;
};
std::optional<TypeDoc> describeType(std::string_view name);
bool isKeywordOrType(std::string_view name);
} // namespace sls::hlsl
