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
// The methods of a texture, buffer or stream type: Sample, Load, ... for Texture2D; none for any other type.
std::vector<const ref::Entry *> methodsOf(std::string_view type);
// A scalar, vector or matrix type taken apart: float3 is {"float", 3, 0}, float4x4 {"float", 4, 4} and float
// {"float", 1, 0}. Unity's real and fixed count as scalars.
struct Shape {
  std::string scalar;
  int rows = 1;
  int columns = 0; // 0 for a scalar or a vector
};
std::optional<Shape> numericShape(std::string_view name);
// A type's description, generated for the scalar, vector and matrix types: float3 is 3 floats.
struct TypeDoc {
  std::string detail;
  std::string doc;
};
std::optional<TypeDoc> describeType(std::string_view name);
bool isKeywordOrType(std::string_view name);
} // namespace sls::hlsl
