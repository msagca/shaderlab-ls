#include "hlsl/builtins.h"
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include "common/util.h"
namespace sls::hlsl {
namespace {
  // Microsoft HLSL reference: keywords, data types, semantics, attributes, object methods and intrinsic functions.
  constexpr ref::Entry kKeywords[] = {
    {"break", "break;", "Exits the innermost loop or `switch`."},
    {"case", "case value:", "Labels a branch of a `switch` statement."},
    {"cbuffer", "cbuffer Name : register(bN) { ... }",
      "Declares a constant buffer: a group of shader constants the application uploads together. In Unity, material "
      "properties belong in `cbuffer UnityPerMaterial` for the SRP Batcher."},
    {"centroid", "centroid",
      "Interpolation modifier: interpolates the input at a point inside the covered part of the pixel instead of its "
      "center, so MSAA never reads values from outside the triangle."},
    {"class", "class Name { ... };", "Declares a class. HLSL classes exist mainly for interfaces and dynamic linkage."},
    {"column_major", "column_major floatRxC", "Matrix packing modifier: stores each column of the matrix in consecutive registers. This is the default."},
    {"const", "const T name",
      "Marks a variable as read-only in the shader. At global scope, use `static const` for a compile-time constant: a "
      "plain global `const` is still a shader constant the application can set."},
    {"continue", "continue;", "Skips to the next iteration of the innermost loop."},
    {"default", "default:", "Labels the branch of a `switch` statement taken when no `case` matches."},
    {"discard", "discard;", "Throws the current pixel away: nothing is written to the render targets or the depth buffer.\n\nPixel shaders only."},
    {"do", "do { ... } while (condition);", "A loop that runs its body once before testing the condition."},
    {"else", "if (condition) { ... } else { ... }", "The branch of an `if` statement taken when the condition is false."},
    {"enum", "enum Name { A, B };", "Declares an enumeration. Needs DXC."},
    {"export", "export", "Makes a function visible outside a shader library (`lib_6_x` targets)."},
    {"extern", "extern T name;", "Marks a global variable as set by the application. Global variables are `extern` unless they are `static`."},
    {"false", "false", "The Boolean value false."},
    {"for", "for (init; condition; step) { ... }", "A loop with an initializer, a condition and a step."},
    {"globallycoherent", "globallycoherent RWTexture2D<T> name;", "Makes a UAV's memory accesses coherent across all thread groups on the GPU, not only within one group."},
    {"groupshared", "groupshared T name[N];",
      "Declares memory shared by all threads of a compute thread group, up to 32 KB per group. Synchronize access with "
      "`GroupMemoryBarrierWithGroupSync`.\n\nCompute shaders only."},
    {"if", "if (condition) { ... }", "Runs the block only when the condition is true. `[branch]` and `[flatten]` choose how it compiles."},
    {"in", "in T parameter", "Parameter modifier: the argument is copied into the function. This is the default."},
    {"inline", "inline", "Accepted for compatibility: HLSL always inlines functions."},
    {"inout", "inout T parameter", "Parameter modifier: the argument is copied into the function, and copied back out when it returns."},
    {"interface", "interface Name { ... };", "Declares an interface, for dynamic linkage."},
    {"line", "line T input[2]", "Geometry shader input primitive: a line, 2 vertices."},
    {"lineadj", "lineadj T input[4]", "Geometry shader input primitive: a line with its adjacent vertices, 4 in all."},
    {"linear", "linear", "Interpolation modifier: interpolates with perspective correction. This is the default."},
    {"namespace", "namespace Name { ... }", "Groups declarations under a name, reached as `Name::member`."},
    {"nointerpolation", "nointerpolation",
      "Interpolation modifier: passes the value of one of the triangle's vertices unchanged instead of interpolating. "
      "Integer values need it."},
    {"noperspective", "noperspective", "Interpolation modifier: interpolates linearly in screen space, without perspective correction."},
    {"out", "out T parameter", "Parameter modifier: the function writes the argument, which is copied out when it returns."},
    {"packoffset", "T name : packoffset(c0);", "Places a constant at a given register offset inside a `cbuffer`."},
    {"point", "point T input[1]", "Geometry shader input primitive: a single point."},
    {"precise", "precise T name",
      "Stops the compiler from optimizations that could change the result of calculations on this value, such as "
      "fusing a multiply and an add."},
    {"register", "T name : register(t0);",
      "Binds a resource or constant to a register slot: `b` for constant buffers, `t` for textures and buffers, `s` for "
      "samplers and `u` for UAVs."},
    {"return", "return value;", "Leaves the function, returning `value`."},
    {"row_major", "row_major floatRxC", "Matrix packing modifier: stores each row of the matrix in consecutive registers."},
    {"sample", "sample", "Interpolation modifier: interpolates the input at each MSAA sample location, which runs the pixel shader once per sample."},
    {"shared", "shared T name;", "Effect framework modifier that shares a variable between effects. It has no effect in Unity shaders."},
    {"snorm", "snorm float", "Limits a float to the range [-1, 1]. Used in typed UAVs and buffers, such as `RWTexture2D<snorm float4>`."},
    {"static", "static T name",
      "At global scope, the variable belongs to the shader and the application cannot set it; `static const` declares a "
      "compile-time constant. Inside a function, the variable keeps its value between calls."},
    {"struct", "struct Name { ... };", "Declares a structure type."},
    {"switch", "switch (value) { case ...: }", "Jumps to the `case` that matches the value. `[branch]`, `[flatten]`, `[forcecase]` and `[call]` choose how it compiles."},
    {"tbuffer", "tbuffer Name { ... }", "Declares a texture buffer: constants read through the texture path, which suits indexed access better than a `cbuffer`."},
    {"triangle", "triangle T input[3]", "Geometry shader input primitive: a triangle, 3 vertices."},
    {"triangleadj", "triangleadj T input[6]", "Geometry shader input primitive: a triangle with its adjacent vertices, 6 in all."},
    {"true", "true", "The Boolean value true."},
    {"typedef", "typedef T Name;", "Declares another name for a type."},
    {"uniform", "uniform T name",
      "Marks a variable as set by the application and constant for the whole draw call. Global variables are uniform "
      "unless they are `static`."},
    {"unorm", "unorm float", "Limits a float to the range [0, 1]. Used in typed UAVs and buffers, such as `RWTexture2D<unorm float4>`."},
    {"unsigned", "unsigned int", "The same as `uint`."},
    {"volatile", "volatile T name", "A hint that a local variable changes often. The compiler ignores it."},
    {"while", "while (condition) { ... }", "A loop that runs while the condition is true."},
  };
  constexpr ref::Entry kResourceTypes[] = {
    {"void", "void", "No value: the return type of a function that returns nothing."},
    {"string", "string", "A string. Only annotations and `printf` use strings; shader calculations cannot."},
    {"sampler", "sampler name;", "A sampler in the legacy syntax, used with `tex2D` and the other `tex*` functions."},
    {"SamplerState", "SamplerState name;",
      "Sampling settings, such as filtering and wrap mode, passed to texture methods like `Sample`. In Unity, "
      "`sampler_<TextureName>` uses that texture's import settings."},
    {"SamplerComparisonState", "SamplerComparisonState name;",
      "A sampler that compares texels against a reference value, for shadow map lookups with `SampleCmp` and "
      "`SampleCmpLevelZero`."},
    {"sampler1D", "sampler1D name;", "A 1D texture and its sampler in the legacy syntax, read with `tex1D`."},
    {"sampler2D", "sampler2D name;",
      "A 2D texture and its sampler in the legacy syntax, read with `tex2D`. In Unity, the texture comes from the "
      "material property of the same name."},
    {"sampler3D", "sampler3D name;", "A 3D texture and its sampler in the legacy syntax, read with `tex3D`."},
    {"samplerCUBE", "samplerCUBE name;", "A cubemap and its sampler in the legacy syntax, read with `texCUBE`."},
    {"texture", "texture name;", "A texture in the effect framework's legacy syntax."},
    {"Texture1D", "Texture1D<T> name;", "A 1D texture, read with `Sample`, `Load` and the other texture methods. `T` defaults to `float4`."},
    {"Texture1DArray", "Texture1DArray<T> name;", "An array of 1D textures; the last coordinate selects the slice."},
    {"Texture2D", "Texture2D<T> name;", "A 2D texture, read with `Sample`, `Load` and the other texture methods. `T` defaults to `float4`."},
    {"Texture2DArray", "Texture2DArray<T> name;", "An array of 2D textures; the last coordinate selects the slice."},
    {"Texture2DMS", "Texture2DMS<T, Samples> name;", "A multisampled 2D texture. Read single samples with `Load(location, sampleIndex)`."},
    {"Texture2DMSArray", "Texture2DMSArray<T, Samples> name;", "An array of multisampled 2D textures."},
    {"Texture3D", "Texture3D<T> name;", "A 3D (volume) texture."},
    {"TextureCube", "TextureCube<T> name;", "A cubemap, sampled with a direction vector."},
    {"TextureCubeArray", "TextureCubeArray<T> name;", "An array of cubemaps."},
    {"Buffer", "Buffer<T> name;", "A read-only buffer of typed elements, read with `Load` or `[]`."},
    {"ByteAddressBuffer", "ByteAddressBuffer name;",
      "A read-only raw buffer addressed in bytes, read with `Load`, `Load2`, `Load3` and `Load4` at addresses that are "
      "multiples of 4."},
    {"StructuredBuffer", "StructuredBuffer<T> name;", "A read-only buffer of structures, indexed with `[]`."},
    {"RWStructuredBuffer", "RWStructuredBuffer<T> name;",
      "A read-write buffer of structures (a UAV). It can have a hidden counter, used with `IncrementCounter` and "
      "`DecrementCounter`."},
    {"AppendStructuredBuffer", "AppendStructuredBuffer<T> name;", "An output buffer that threads add structures to with `Append`."},
    {"ConsumeStructuredBuffer", "ConsumeStructuredBuffer<T> name;", "An input buffer that threads take structures from with `Consume`."},
    {"RWBuffer", "RWBuffer<T> name;", "A read-write buffer of typed elements (a UAV), indexed with `[]`."},
    {"RWByteAddressBuffer", "RWByteAddressBuffer name;",
      "A read-write raw buffer addressed in bytes (a UAV), with `Load*`, `Store*` and `Interlocked*` methods at "
      "addresses that are multiples of 4."},
    {"RWTexture1D", "RWTexture1D<T> name;", "A read-write 1D texture (a UAV), indexed with `[]`."},
    {"RWTexture1DArray", "RWTexture1DArray<T> name;", "A read-write array of 1D textures (a UAV), indexed with `[]`."},
    {"RWTexture2D", "RWTexture2D<T> name;", "A read-write 2D texture (a UAV), indexed with `[]` in pixel coordinates."},
    {"RWTexture2DArray", "RWTexture2DArray<T> name;", "A read-write array of 2D textures (a UAV), indexed with `[]`."},
    {"RWTexture3D", "RWTexture3D<T> name;", "A read-write 3D texture (a UAV), indexed with `[]`."},
    {"InputPatch", "InputPatch<T, N> name", "The control points of a patch, as input to a hull shader."},
    {"OutputPatch", "OutputPatch<T, N> name", "The control points a hull shader outputs, as input to the patch constant function and the domain shader."},
    {"PointStream", "inout PointStream<T> stream", "Geometry shader output: a stream of points, written with `Append`."},
    {"LineStream", "inout LineStream<T> stream", "Geometry shader output: a stream of line strips, written with `Append` and `RestartStrip`."},
    {"TriangleStream", "inout TriangleStream<T> stream", "Geometry shader output: a stream of triangle strips, written with `Append` and `RestartStrip`."},
    {"vector", "vector<T, N>", "A vector of N components (1 to 4) of type T: `vector<float, 4>` is `float4`. A bare `vector` is `float4`."},
    {"matrix", "matrix<T, R, C>", "A matrix of R rows and C columns of type T: `matrix<float, 4, 4>` is `float4x4`. A bare `matrix` is `float4x4`."},
  };
  // Scalar types, each also available as a vector (float3) and a matrix (float4x4), except dword.
  constexpr ref::Entry kScalars[] = {
    {"bool", "bool", "A Boolean: `true` or `false`."},
    {"int", "int", "A 32-bit signed integer."},
    {"uint", "uint", "A 32-bit unsigned integer."},
    {"dword", "dword", "A 32-bit unsigned integer, the same as `uint`."},
    {"half", "half", "A 16-bit floating-point number. Mobile GPUs compute it at 16 bits; most desktop GPUs at full 32-bit precision."},
    {"float", "float", "A 32-bit floating-point number."},
    {"double", "double", "A 64-bit floating-point number. Needs a GPU with double-precision support."},
    {"min16float", "min16float", "A floating-point number with at least 16 bits of precision; the GPU may use more."},
    {"min10float", "min10float", "A floating-point number with at least 10 bits of precision; the GPU may use more."},
    {"min16int", "min16int", "A signed integer with at least 16 bits; the GPU may use more."},
    {"min12int", "min12int", "A signed integer with at least 12 bits; the GPU may use more."},
    {"min16uint", "min16uint", "An unsigned integer with at least 16 bits; the GPU may use more."},
  };
  // Signatures use T for a float, int or uint scalar, vector or matrix, and floatN for a float vector. Functions marked
  // per component apply to each component of x on its own.
  constexpr ref::Entry kIntrinsics[] = {
    {"abort", "void abort()", "Terminates the current draw or dispatch call being executed."},
    {"abs", "T abs(T x)", "Returns the absolute value of `x`, per component."},
    {"acos", "T acos(T x)", "Returns the arccosine of `x`, per component. `x` must be in the range [-1, 1]; the result is in [0, π]."},
    {"all", "bool all(T x)", "Returns `true` if every component of `x` is nonzero."},
    {"AllMemoryBarrier", "void AllMemoryBarrier()",
      "Blocks execution of all threads in a group until all memory accesses (device and group shared) have completed.\n\n"
      "Compute shaders only."},
    {"AllMemoryBarrierWithGroupSync", "void AllMemoryBarrierWithGroupSync()",
      "Blocks execution of all threads in a group until all memory accesses (device and group shared) have completed "
      "and all threads in the group have reached this call.\n\nCompute shaders only."},
    {"any", "bool any(T x)", "Returns `true` if any component of `x` is nonzero."},
    {"asdouble", "double asdouble(uint lowbits, uint highbits)", "Reinterprets a pair of 32-bit values as the low and high halves of a 64-bit `double`, without conversion."},
    {"asfloat", "float asfloat(T x)", "Reinterprets the bits of `x` as a `float` (or float vector/matrix of the same shape), without conversion."},
    {"asin", "T asin(T x)", "Returns the arcsine of `x`, per component. `x` must be in the range [-1, 1]; the result is in [-π/2, π/2]."},
    {"asint", "int asint(T x)", "Reinterprets the bits of `x` as an `int` (or int vector/matrix of the same shape), without conversion."},
    {"asuint", "uint asuint(T x)\nvoid asuint(double value, out uint lowbits, out uint highbits)",
      "Reinterprets the bits of `x` as a `uint` (or uint vector/matrix of the same shape), without conversion. The "
      "`double` form splits the value into its low and high 32 bits."},
    {"atan", "T atan(T x)", "Returns the arctangent of `x`, per component. The result is in [-π/2, π/2]."},
    {"atan2", "T atan2(T y, T x)",
      "Returns the arctangent of `y / x`, per component, using the signs of both arguments to pick the quadrant. The "
      "result is in [-π, π]."},
    {"ceil", "T ceil(T x)", "Returns the smallest integer value that is greater than or equal to `x`, per component."},
    {"CheckAccessFullyMapped", "bool CheckAccessFullyMapped(uint status)",
      "Interprets the `status` returned by a texture `Sample`, `Gather` or `Load` call on a tiled resource, and returns "
      "`true` if every texel the operation touched was mapped."},
    {"clamp", "T clamp(T x, T min, T max)", "Clamps `x` to the range [`min`, `max`], per component."},
    {"clip", "void clip(T x)", "Discards the current pixel if any component of `x` is less than zero.\n\nPixel shaders only."},
    {"cos", "T cos(T x)", "Returns the cosine of `x` (in radians), per component."},
    {"cosh", "T cosh(T x)", "Returns the hyperbolic cosine of `x`, per component."},
    {"countbits", "uint countbits(uint x)", "Returns the number of bits set in `x`, per component."},
    {"cross", "float3 cross(float3 x, float3 y)", "Returns the cross product of two 3D vectors."},
    {"D3DCOLORtoUBYTE4", "int4 D3DCOLORtoUBYTE4(float4 x)",
      "Swizzles and scales the components of `x` to make up for the lack of UBYTE4 support on some hardware: returns "
      "`x.zyxw * 255.001953`, truncated to integers."},
    {"ddx", "T ddx(T x)", "Returns the partial derivative of `x` with respect to the screen-space x coordinate.\n\nPixel shaders only."},
    {"ddx_coarse", "T ddx_coarse(T x)",
      "Returns a low-precision partial derivative of `x` with respect to the screen-space x coordinate, which may be "
      "shared by the pixels of a 2x2 quad.\n\nPixel shaders only."},
    {"ddx_fine", "T ddx_fine(T x)",
      "Returns a high-precision partial derivative of `x` with respect to the screen-space x coordinate, computed per "
      "pixel.\n\nPixel shaders only."},
    {"ddy", "T ddy(T x)", "Returns the partial derivative of `x` with respect to the screen-space y coordinate.\n\nPixel shaders only."},
    {"ddy_coarse", "T ddy_coarse(T x)",
      "Returns a low-precision partial derivative of `x` with respect to the screen-space y coordinate, which may be "
      "shared by the pixels of a 2x2 quad.\n\nPixel shaders only."},
    {"ddy_fine", "T ddy_fine(T x)",
      "Returns a high-precision partial derivative of `x` with respect to the screen-space y coordinate, computed per "
      "pixel.\n\nPixel shaders only."},
    {"degrees", "T degrees(T x)", "Converts `x` from radians to degrees, per component."},
    {"determinant", "float determinant(floatNxN m)", "Returns the determinant of the square matrix `m`."},
    {"DeviceMemoryBarrier", "void DeviceMemoryBarrier()",
      "Blocks execution of all threads in a group until all device memory accesses have completed.\n\nCompute and pixel "
      "shaders."},
    {"DeviceMemoryBarrierWithGroupSync", "void DeviceMemoryBarrierWithGroupSync()",
      "Blocks execution of all threads in a group until all device memory accesses have completed and all threads in the "
      "group have reached this call.\n\nCompute shaders only."},
    {"distance", "float distance(floatN x, floatN y)", "Returns the distance between the points `x` and `y`."},
    {"dot", "float dot(floatN x, floatN y)", "Returns the dot product of two vectors."},
    {"dst", "float4 dst(float4 a, float4 b)",
      "Returns a distance vector for lighting: `(1, a.y * b.y, a.z, b.w)`. With `a = (_, d², d², _)` and "
      "`b = (_, 1/d, _, 1/d)` this is `(1, d, d², 1/d)`."},
    {"errorf", "void errorf(string format, ...)", "Submits an error message to the information queue. Only has an effect under a debug layer."},
    {"EvaluateAttributeAtCentroid", "T EvaluateAttributeAtCentroid(T x)", "Evaluates the interpolated input attribute `x` at the pixel centroid.\n\nPixel shaders only."},
    {"EvaluateAttributeAtSample", "T EvaluateAttributeAtSample(T x, uint index)", "Evaluates the interpolated input attribute `x` at the location of sample `index`.\n\nPixel shaders only."},
    {"EvaluateAttributeSnapped", "T EvaluateAttributeSnapped(T x, int2 offset)",
      "Evaluates the interpolated input attribute `x` at the pixel center moved by `offset`, in 1/16 pixel steps from -8 "
      "to 7.\n\nPixel shaders only."},
    {"exp", "T exp(T x)", "Returns e raised to the power `x`, per component."},
    {"exp2", "T exp2(T x)", "Returns 2 raised to the power `x`, per component."},
    {"f16tof32", "float f16tof32(uint x)", "Converts the half-precision float stored in the low 16 bits of `x` to a 32-bit float, per component."},
    {"f32tof16", "uint f32tof16(float x)", "Converts `x` to a half-precision float and returns it in the low 16 bits of a `uint`, per component."},
    {"faceforward", "floatN faceforward(floatN n, floatN i, floatN ng)", "Flips the surface normal `n` to face the viewer: returns `-n * sign(dot(i, ng))`."},
    {"firstbithigh", "int firstbithigh(int x)\nuint firstbithigh(uint x)",
      "Returns the position of the first set bit, searching from the most significant bit down, per component. For a "
      "negative `int` it finds the first clear bit instead. Returns -1 (0xFFFFFFFF) if there is none."},
    {"firstbitlow", "uint firstbitlow(uint x)",
      "Returns the position of the first set bit, searching from the least significant bit up, per component. Returns "
      "-1 (0xFFFFFFFF) if there is none."},
    {"floor", "T floor(T x)", "Returns the largest integer value that is less than or equal to `x`, per component."},
    {"fma", "double fma(double a, double b, double c)", "Returns `a * b + c` as a fused multiply-add, rounding only once. Doubles only."},
    {"fmod", "T fmod(T x, T y)", "Returns the floating-point remainder of `x / y`, per component. The result has the same sign as `x`."},
    {"frac", "T frac(T x)", "Returns the fractional part of `x`, `x - floor(x)`, per component. The result is in [0, 1)."},
    {"frexp", "T frexp(T x, out T exp)", "Splits `x` into a mantissa, which it returns, and a power-of-two exponent, which it writes to `exp`, per component."},
    {"fwidth", "T fwidth(T x)", "Returns `abs(ddx(x)) + abs(ddy(x))`, per component.\n\nPixel shaders only."},
    {"GetRenderTargetSampleCount", "uint GetRenderTargetSampleCount()", "Returns the number of samples in the render target."},
    {"GetRenderTargetSamplePosition", "float2 GetRenderTargetSamplePosition(int index)", "Returns the position of sample `index` relative to the pixel center."},
    {"GroupMemoryBarrier", "void GroupMemoryBarrier()",
      "Blocks execution of all threads in a group until all group shared memory accesses have completed.\n\nCompute "
      "shaders only."},
    {"GroupMemoryBarrierWithGroupSync", "void GroupMemoryBarrierWithGroupSync()",
      "Blocks execution of all threads in a group until all group shared memory accesses have completed and all threads "
      "in the group have reached this call.\n\nCompute shaders only."},
    {"InterlockedAdd", "void InterlockedAdd(inout R dest, T value, out T original_value)",
      "Atomically adds `value` to `dest`, a `groupshared` variable or an element of a UAV, and optionally returns the "
      "value `dest` held before. `int` and `uint` only."},
    {"InterlockedAnd", "void InterlockedAnd(inout R dest, T value, out T original_value)", "Atomically sets `dest` to `dest & value` and optionally returns the value it held before. `int` and `uint` only."},
    {"InterlockedCompareExchange", "void InterlockedCompareExchange(inout R dest, T compare_value, T value, out T original_value)",
      "Atomically writes `value` to `dest` if `dest` equals `compare_value`, and returns the value `dest` held before. "
      "`int` and `uint` only."},
    {"InterlockedCompareStore", "void InterlockedCompareStore(inout R dest, T compare_value, T value)", "Atomically writes `value` to `dest` if `dest` equals `compare_value`. `int` and `uint` only."},
    {"InterlockedExchange", "void InterlockedExchange(inout R dest, T value, out T original_value)", "Atomically writes `value` to `dest` and returns the value it held before. `int` and `uint` only."},
    {"InterlockedMax", "void InterlockedMax(inout R dest, T value, out T original_value)", "Atomically sets `dest` to `max(dest, value)` and optionally returns the value it held before. `int` and `uint` only."},
    {"InterlockedMin", "void InterlockedMin(inout R dest, T value, out T original_value)", "Atomically sets `dest` to `min(dest, value)` and optionally returns the value it held before. `int` and `uint` only."},
    {"InterlockedOr", "void InterlockedOr(inout R dest, T value, out T original_value)", "Atomically sets `dest` to `dest | value` and optionally returns the value it held before. `int` and `uint` only."},
    {"InterlockedXor", "void InterlockedXor(inout R dest, T value, out T original_value)", "Atomically sets `dest` to `dest ^ value` and optionally returns the value it held before. `int` and `uint` only."},
    {"isfinite", "bool isfinite(T x)", "Returns `true` for each component of `x` that is finite (neither infinite nor NaN)."},
    {"isinf", "bool isinf(T x)", "Returns `true` for each component of `x` that is +INF or -INF."},
    {"isnan", "bool isnan(T x)", "Returns `true` for each component of `x` that is NaN."},
    {"ldexp", "T ldexp(T x, T exp)", "Returns `x * 2^exp`, per component."},
    {"length", "float length(floatN x)", "Returns the length of the vector `x`."},
    {"lerp", "T lerp(T x, T y, T s)", "Linearly interpolates between `x` and `y` by `s`: returns `x + s * (y - x)`, per component."},
    {"lit", "float4 lit(float n_dot_l, float n_dot_h, float m)",
      "Returns a lighting coefficient vector `(ambient, diffuse, specular, 1)`: ambient is 1, diffuse is "
      "`max(n_dot_l, 0)`, and specular is `pow(n_dot_h, m)`, or 0 if `n_dot_l` or `n_dot_h` is negative."},
    {"log", "T log(T x)", "Returns the natural (base-e) logarithm of `x`, per component."},
    {"log10", "T log10(T x)", "Returns the base-10 logarithm of `x`, per component."},
    {"log2", "T log2(T x)", "Returns the base-2 logarithm of `x`, per component."},
    {"mad", "T mad(T m, T a, T b)", "Returns `m * a + b`, per component, as a single multiply-add instruction where the hardware has one."},
    {"max", "T max(T x, T y)", "Returns the greater of `x` and `y`, per component."},
    {"min", "T min(T x, T y)", "Returns the lesser of `x` and `y`, per component."},
    {"modf", "T modf(T x, out T ip)",
      "Splits `x` into a fractional part, which it returns, and an integer part, which it writes to `ip`, per component. "
      "Both parts have the same sign as `x`."},
    {"msad4", "uint4 msad4(uint reference, uint2 source, uint4 accum)",
      "Compares the 4 bytes of `reference` against each of the 4 byte-aligned windows of `source` and adds the masked "
      "sum of absolute differences to `accum`. Bytes of `reference` that are 0 are skipped."},
    {"mul", "mul(x, y)",
      "Multiplies `x` and `y` using matrix math. A vector on the left is a row vector, and on the right a column vector; "
      "a scalar argument scales the other one per component."},
    {"noise", "float noise(floatN x)", "Returns a Perlin noise value for `x`.\n\nLegacy texture shaders only; not available in vertex, pixel or compute shaders."},
    {"normalize", "floatN normalize(floatN x)", "Returns `x / length(x)`."},
    {"pow", "T pow(T x, T y)", "Returns `x` raised to the power `y`, per component. The result is undefined for negative `x`."},
    {"printf", "void printf(string format, ...)", "Submits a custom message to the information queue. Only has an effect under a debug layer."},
    {"Process2DQuadTessFactorsAvg",
      "void Process2DQuadTessFactorsAvg(float4 RawEdgeFactors, float2 InsideScale, out float4 RoundedEdgeTessFactors, "
      "out float2 RoundedInsideTessFactors, out float2 UnroundedInsideTessFactors)",
      "Computes rounded tessellation factors for a quad patch, with separate U and V inside factors each taken as the "
      "average of the opposite edge factors and then scaled by `InsideScale`.\n\nHull shaders only."},
    {"Process2DQuadTessFactorsMax",
      "void Process2DQuadTessFactorsMax(float4 RawEdgeFactors, float2 InsideScale, out float4 RoundedEdgeTessFactors, "
      "out float2 RoundedInsideTessFactors, out float2 UnroundedInsideTessFactors)",
      "Computes rounded tessellation factors for a quad patch, with separate U and V inside factors each taken as the "
      "maximum of the opposite edge factors and then scaled by `InsideScale`.\n\nHull shaders only."},
    {"Process2DQuadTessFactorsMin",
      "void Process2DQuadTessFactorsMin(float4 RawEdgeFactors, float2 InsideScale, out float4 RoundedEdgeTessFactors, "
      "out float2 RoundedInsideTessFactors, out float2 UnroundedInsideTessFactors)",
      "Computes rounded tessellation factors for a quad patch, with separate U and V inside factors each taken as the "
      "minimum of the opposite edge factors and then scaled by `InsideScale`.\n\nHull shaders only."},
    {"ProcessIsolineTessFactors",
      "void ProcessIsolineTessFactors(float RawDetailFactor, float RawDensityFactor, out float RoundedDetailFactor, out "
      "float RoundedDensityFactor)",
      "Rounds the detail and density tessellation factors of an isoline patch.\n\nHull shaders only."},
    {"ProcessQuadTessFactorsAvg",
      "void ProcessQuadTessFactorsAvg(float4 RawEdgeFactors, float InsideScale, out float4 RoundedEdgeTessFactors, out "
      "float2 RoundedInsideTessFactors, out float2 UnroundedInsideTessFactors)",
      "Computes rounded tessellation factors for a quad patch, with one inside factor taken as the average of the edge "
      "factors and then scaled by `InsideScale`.\n\nHull shaders only."},
    {"ProcessQuadTessFactorsMax",
      "void ProcessQuadTessFactorsMax(float4 RawEdgeFactors, float InsideScale, out float4 RoundedEdgeTessFactors, out "
      "float2 RoundedInsideTessFactors, out float2 UnroundedInsideTessFactors)",
      "Computes rounded tessellation factors for a quad patch, with one inside factor taken as the maximum of the edge "
      "factors and then scaled by `InsideScale`.\n\nHull shaders only."},
    {"ProcessQuadTessFactorsMin",
      "void ProcessQuadTessFactorsMin(float4 RawEdgeFactors, float InsideScale, out float4 RoundedEdgeTessFactors, out "
      "float2 RoundedInsideTessFactors, out float2 UnroundedInsideTessFactors)",
      "Computes rounded tessellation factors for a quad patch, with one inside factor taken as the minimum of the edge "
      "factors and then scaled by `InsideScale`.\n\nHull shaders only."},
    {"ProcessTriTessFactorsAvg",
      "void ProcessTriTessFactorsAvg(float3 RawEdgeFactors, float InsideScale, out float3 RoundedEdgeTessFactors, out "
      "float RoundedInsideTessFactor, out float UnroundedInsideTessFactor)",
      "Computes rounded tessellation factors for a triangle patch, with the inside factor taken as the average of the "
      "edge factors and then scaled by `InsideScale`.\n\nHull shaders only."},
    {"ProcessTriTessFactorsMax",
      "void ProcessTriTessFactorsMax(float3 RawEdgeFactors, float InsideScale, out float3 RoundedEdgeTessFactors, out "
      "float RoundedInsideTessFactor, out float UnroundedInsideTessFactor)",
      "Computes rounded tessellation factors for a triangle patch, with the inside factor taken as the maximum of the "
      "edge factors and then scaled by `InsideScale`.\n\nHull shaders only."},
    {"ProcessTriTessFactorsMin",
      "void ProcessTriTessFactorsMin(float3 RawEdgeFactors, float InsideScale, out float3 RoundedEdgeTessFactors, out "
      "float RoundedInsideTessFactor, out float UnroundedInsideTessFactor)",
      "Computes rounded tessellation factors for a triangle patch, with the inside factor taken as the minimum of the "
      "edge factors and then scaled by `InsideScale`.\n\nHull shaders only."},
    {"radians", "T radians(T x)", "Converts `x` from degrees to radians, per component."},
    {"rcp", "T rcp(T x)", "Returns a fast, approximate reciprocal `1 / x`, per component."},
    {"reflect", "floatN reflect(floatN i, floatN n)", "Reflects the incident vector `i` about the surface normal `n`: returns `i - 2 * n * dot(i, n)`."},
    {"refract", "floatN refract(floatN i, floatN n, float eta)",
      "Returns the refraction of the incident vector `i` through a surface with normal `n`, where `eta` is the ratio of "
      "refractive indices. `i` and `n` should be normalized. Returns a zero vector on total internal reflection."},
    {"reversebits", "uint reversebits(uint x)", "Reverses the order of the bits in `x`, per component."},
    {"round", "T round(T x)", "Rounds `x` to the nearest integer, per component."},
    {"rsqrt", "T rsqrt(T x)", "Returns the reciprocal of the square root, `1 / sqrt(x)`, per component."},
    {"saturate", "T saturate(T x)", "Clamps `x` to the range [0, 1], per component."},
    {"sign", "int sign(T x)", "Returns -1, 0 or 1 for each component of `x` that is negative, zero or positive."},
    {"sin", "T sin(T x)", "Returns the sine of `x` (in radians), per component."},
    {"sincos", "void sincos(T x, out T s, out T c)", "Writes the sine and cosine of `x` (in radians) to `s` and `c`, per component."},
    {"sinh", "T sinh(T x)", "Returns the hyperbolic sine of `x`, per component."},
    {"smoothstep", "T smoothstep(T min, T max, T x)",
      "Returns 0 if `x` is below `min`, 1 if it is above `max`, and otherwise a smooth Hermite interpolation between 0 "
      "and 1 (`t * t * (3 - 2 * t)`), per component."},
    {"sqrt", "T sqrt(T x)", "Returns the square root of `x`, per component."},
    {"step", "T step(T y, T x)", "Returns 1 where `x >= y` and 0 otherwise, per component."},
    {"tan", "T tan(T x)", "Returns the tangent of `x` (in radians), per component."},
    {"tanh", "T tanh(T x)", "Returns the hyperbolic tangent of `x`, per component."},
    {"tex1D", "float4 tex1D(sampler1D s, float t)\nfloat4 tex1D(sampler1D s, float t, float ddx, float ddy)", "Samples a 1D texture at `t`, optionally with explicit derivatives. Legacy sampler syntax."},
    {"tex1Dbias", "float4 tex1Dbias(sampler1D s, float4 t)", "Samples a 1D texture at `t.x`, after biasing the mip level by `t.w`. Legacy sampler syntax."},
    {"tex1Dgrad", "float4 tex1Dgrad(sampler1D s, float t, float ddx, float ddy)", "Samples a 1D texture at `t`, using the given derivatives to select the mip level. Legacy sampler syntax."},
    {"tex1Dlod", "float4 tex1Dlod(sampler1D s, float4 t)", "Samples a 1D texture at `t.x`, from mip level `t.w`. Legacy sampler syntax."},
    {"tex1Dproj", "float4 tex1Dproj(sampler1D s, float4 t)", "Samples a 1D texture at `t.x / t.w` (projective sampling). Legacy sampler syntax."},
    {"tex2D", "float4 tex2D(sampler2D s, float2 t)\nfloat4 tex2D(sampler2D s, float2 t, float2 ddx, float2 ddy)", "Samples a 2D texture at `t`, optionally with explicit derivatives. Legacy sampler syntax."},
    {"tex2Dbias", "float4 tex2Dbias(sampler2D s, float4 t)", "Samples a 2D texture at `t.xy`, after biasing the mip level by `t.w`. Legacy sampler syntax."},
    {"tex2Dgrad", "float4 tex2Dgrad(sampler2D s, float2 t, float2 ddx, float2 ddy)", "Samples a 2D texture at `t`, using the given derivatives to select the mip level. Legacy sampler syntax."},
    {"tex2Dlod", "float4 tex2Dlod(sampler2D s, float4 t)", "Samples a 2D texture at `t.xy`, from mip level `t.w`. Legacy sampler syntax."},
    {"tex2Dproj", "float4 tex2Dproj(sampler2D s, float4 t)", "Samples a 2D texture at `t.xy / t.w` (projective sampling). Legacy sampler syntax."},
    {"tex3D", "float4 tex3D(sampler3D s, float3 t)\nfloat4 tex3D(sampler3D s, float3 t, float3 ddx, float3 ddy)", "Samples a 3D texture at `t`, optionally with explicit derivatives. Legacy sampler syntax."},
    {"tex3Dbias", "float4 tex3Dbias(sampler3D s, float4 t)", "Samples a 3D texture at `t.xyz`, after biasing the mip level by `t.w`. Legacy sampler syntax."},
    {"tex3Dgrad", "float4 tex3Dgrad(sampler3D s, float3 t, float3 ddx, float3 ddy)", "Samples a 3D texture at `t`, using the given derivatives to select the mip level. Legacy sampler syntax."},
    {"tex3Dlod", "float4 tex3Dlod(sampler3D s, float4 t)", "Samples a 3D texture at `t.xyz`, from mip level `t.w`. Legacy sampler syntax."},
    {"tex3Dproj", "float4 tex3Dproj(sampler3D s, float4 t)", "Samples a 3D texture at `t.xyz / t.w` (projective sampling). Legacy sampler syntax."},
    {"texCUBE", "float4 texCUBE(samplerCUBE s, float3 t)\nfloat4 texCUBE(samplerCUBE s, float3 t, float3 ddx, float3 ddy)", "Samples a cube texture in the direction `t`, optionally with explicit derivatives. Legacy sampler syntax."},
    {"texCUBEbias", "float4 texCUBEbias(samplerCUBE s, float4 t)", "Samples a cube texture in the direction `t.xyz`, after biasing the mip level by `t.w`. Legacy sampler syntax."},
    {"texCUBEgrad", "float4 texCUBEgrad(samplerCUBE s, float3 t, float3 ddx, float3 ddy)",
      "Samples a cube texture in the direction `t`, using the given derivatives to select the mip level. Legacy sampler "
      "syntax."},
    {"texCUBElod", "float4 texCUBElod(samplerCUBE s, float4 t)", "Samples a cube texture in the direction `t.xyz`, from mip level `t.w`. Legacy sampler syntax."},
    {"texCUBEproj", "float4 texCUBEproj(samplerCUBE s, float4 t)", "Samples a cube texture in the direction `t.xyz / t.w` (projective sampling). Legacy sampler syntax."},
    {"transpose", "floatMxN transpose(floatNxM m)", "Returns the transpose of the matrix `m`."},
    {"trunc", "T trunc(T x)", "Truncates `x` to its integer part, rounding toward zero, per component."},
    // Shader Model 6. Unity compiles these only with DXC: `#pragma use_dxc`, with `#pragma require` for the wave ops.
    {"and", "bool and(bool x, bool y)", "Returns `x && y` per component, evaluating both sides. HLSL 2021, where `&&` no longer works on vectors. Needs DXC."},
    {"dot2add", "float dot2add(half2 a, half2 b, float acc)", "Returns `dot(a, b) + acc`, with 16-bit inputs and a 32-bit result. Shader Model 6.4; needs DXC."},
    {"dot4add_i8packed", "int dot4add_i8packed(uint a, uint b, int acc)",
      "Reads `a` and `b` as four packed signed 8-bit integers each, and returns their dot product plus `acc`. Shader Model "
      "6.4; needs DXC."},
    {"dot4add_u8packed", "uint dot4add_u8packed(uint a, uint b, uint acc)",
      "Reads `a` and `b` as four packed unsigned 8-bit integers each, and returns their dot product plus `acc`. Shader "
      "Model 6.4; needs DXC."},
    {"IsHelperLane", "bool IsHelperLane()",
      "Returns `true` on a helper lane: a pixel shader invocation that runs only so its neighbours can compute "
      "derivatives. Shader Model 6.6; needs DXC."},
    {"or", "bool or(bool x, bool y)", "Returns `x || y` per component, evaluating both sides. HLSL 2021, where `||` no longer works on vectors. Needs DXC."},
    {"QuadAll", "bool QuadAll(bool expr)", "Returns `true` if `expr` is true on all four lanes of the 2x2 pixel quad. Shader Model 6.7; needs DXC."},
    {"QuadAny", "bool QuadAny(bool expr)", "Returns `true` if `expr` is true on any of the four lanes of the 2x2 pixel quad. Shader Model 6.7; needs DXC."},
    {"QuadReadAcrossDiagonal", "T QuadReadAcrossDiagonal(T value)", "Returns `value` from the diagonally opposite lane of the 2x2 pixel quad. Shader Model 6.0; needs DXC."},
    {"QuadReadAcrossX", "T QuadReadAcrossX(T value)", "Returns `value` from the horizontally adjacent lane of the 2x2 pixel quad. Shader Model 6.0; needs DXC."},
    {"QuadReadAcrossY", "T QuadReadAcrossY(T value)", "Returns `value` from the vertically adjacent lane of the 2x2 pixel quad. Shader Model 6.0; needs DXC."},
    {"QuadReadLaneAt", "T QuadReadLaneAt(T value, uint quadLane)", "Returns `value` from lane `quadLane` (0 to 3) of the 2x2 pixel quad. Shader Model 6.0; needs DXC."},
    {"select", "T select(bool cond, T x, T y)", "Returns `x` where `cond` is true and `y` elsewhere, per component. HLSL 2021, in place of `?:` on vectors. Needs DXC."},
    {"WaveActiveAllEqual", "bool WaveActiveAllEqual(T value)", "Returns `true` if `value` is the same on every active lane of the wave, per component. Shader Model 6.0; needs DXC."},
    {"WaveActiveAllTrue", "bool WaveActiveAllTrue(bool expr)", "Returns `true` if `expr` is true on every active lane of the wave. Shader Model 6.0; needs DXC."},
    {"WaveActiveAnyTrue", "bool WaveActiveAnyTrue(bool expr)", "Returns `true` if `expr` is true on any active lane of the wave. Shader Model 6.0; needs DXC."},
    {"WaveActiveBallot", "uint4 WaveActiveBallot(bool expr)", "Returns a 128-bit mask with a bit set for each active lane where `expr` is true. Shader Model 6.0; needs DXC."},
    {"WaveActiveBitAnd", "T WaveActiveBitAnd(T value)", "Returns the bitwise AND of `value` over the active lanes of the wave. Integers only. Shader Model 6.0; needs DXC."},
    {"WaveActiveBitOr", "T WaveActiveBitOr(T value)", "Returns the bitwise OR of `value` over the active lanes of the wave. Integers only. Shader Model 6.0; needs DXC."},
    {"WaveActiveBitXor", "T WaveActiveBitXor(T value)", "Returns the bitwise XOR of `value` over the active lanes of the wave. Integers only. Shader Model 6.0; needs DXC."},
    {"WaveActiveCountBits", "uint WaveActiveCountBits(bool expr)", "Returns the number of active lanes where `expr` is true. Shader Model 6.0; needs DXC."},
    {"WaveActiveMax", "T WaveActiveMax(T value)", "Returns the largest `value` over the active lanes of the wave. Shader Model 6.0; needs DXC."},
    {"WaveActiveMin", "T WaveActiveMin(T value)", "Returns the smallest `value` over the active lanes of the wave. Shader Model 6.0; needs DXC."},
    {"WaveActiveProduct", "T WaveActiveProduct(T value)", "Returns the product of `value` over the active lanes of the wave. Shader Model 6.0; needs DXC."},
    {"WaveActiveSum", "T WaveActiveSum(T value)", "Returns the sum of `value` over the active lanes of the wave. Shader Model 6.0; needs DXC."},
    {"WaveGetLaneCount", "uint WaveGetLaneCount()", "Returns the number of lanes (threads) in a wave on this GPU, such as 32 or 64. Shader Model 6.0; needs DXC."},
    {"WaveGetLaneIndex", "uint WaveGetLaneIndex()", "Returns this lane's index within its wave. Shader Model 6.0; needs DXC."},
    {"WaveIsFirstLane", "bool WaveIsFirstLane()", "Returns `true` on the active lane with the lowest index in the wave. Shader Model 6.0; needs DXC."},
    {"WaveMatch", "uint4 WaveMatch(T value)", "Returns a mask of the active lanes whose `value` equals this lane's. Shader Model 6.5; needs DXC."},
    {"WaveMultiPrefixBitAnd", "T WaveMultiPrefixBitAnd(T value, uint4 mask)",
      "Like a prefix `WaveActiveBitAnd`: the bitwise AND of `value` over the lanes in `mask` below this one. Shader Model "
      "6.5; needs DXC."},
    {"WaveMultiPrefixBitOr", "T WaveMultiPrefixBitOr(T value, uint4 mask)",
      "Like a prefix `WaveActiveBitOr`: the bitwise OR of `value` over the lanes in `mask` below this one. Shader Model "
      "6.5; needs DXC."},
    {"WaveMultiPrefixBitXor", "T WaveMultiPrefixBitXor(T value, uint4 mask)",
      "Like a prefix `WaveActiveBitXor`: the bitwise XOR of `value` over the lanes in `mask` below this one. Shader Model "
      "6.5; needs DXC."},
    {"WaveMultiPrefixCountBits", "uint WaveMultiPrefixCountBits(bool expr, uint4 mask)", "Like `WavePrefixCountBits`, over only the lanes in `mask`. Shader Model 6.5; needs DXC."},
    {"WaveMultiPrefixProduct", "T WaveMultiPrefixProduct(T value, uint4 mask)", "Like `WavePrefixProduct`, over only the lanes in `mask`. Shader Model 6.5; needs DXC."},
    {"WaveMultiPrefixSum", "T WaveMultiPrefixSum(T value, uint4 mask)", "Like `WavePrefixSum`, over only the lanes in `mask`. Shader Model 6.5; needs DXC."},
    {"WavePrefixCountBits", "uint WavePrefixCountBits(bool expr)", "Returns the number of active lanes below this one where `expr` is true. Shader Model 6.0; needs DXC."},
    {"WavePrefixProduct", "T WavePrefixProduct(T value)", "Returns the product of `value` over the active lanes below this one, not counting this lane. Shader Model 6.0; needs DXC."},
    {"WavePrefixSum", "T WavePrefixSum(T value)", "Returns the sum of `value` over the active lanes below this one, not counting this lane. Shader Model 6.0; needs DXC."},
    {"WaveReadLaneAt", "T WaveReadLaneAt(T value, uint lane)", "Returns `value` from the given lane of the wave. Shader Model 6.0; needs DXC."},
    {"WaveReadLaneFirst", "T WaveReadLaneFirst(T value)", "Returns `value` from the active lane with the lowest index. Shader Model 6.0; needs DXC."},
  };
  // Semantics, matched case-insensitively. Entries whose detail ends in [n] also take an index: TEXCOORD3.
  constexpr ref::Entry kSemantics[] = {
    {"POSITION", "POSITION[n]",
      "Vertex shader input: the vertex position from the mesh, in object space. As a vertex shader output, the old "
      "name of `SV_POSITION`."},
    {"NORMAL", "NORMAL[n]", "Vertex shader input: the vertex normal from the mesh, in object space."},
    {"TANGENT", "TANGENT[n]", "Vertex shader input: the vertex tangent from the mesh. In Unity, `w` is ±1: the sign to give the bitangent."},
    {"BINORMAL", "BINORMAL[n]", "Vertex shader input: the vertex bitangent. Unity meshes don't provide one; compute it from the normal and tangent."},
    {"TEXCOORD", "TEXCOORD[n]",
      "Vertex shader input: UV channel n of the mesh, so `TEXCOORD0` is the first UV set. Between shader stages, a "
      "general-purpose interpolated value."},
    {"COLOR", "COLOR[n]", "Vertex shader input: the vertex color from the mesh. As a pixel shader output, the old name of `SV_Target`."},
    {"BLENDINDICES", "BLENDINDICES[n]", "Vertex shader input: the indices of the bones that skin this vertex."},
    {"BLENDWEIGHT", "BLENDWEIGHT[n]", "Vertex shader input: the weights of the bones that skin this vertex."},
    {"BLENDWEIGHTS", "BLENDWEIGHTS[n]", "Vertex shader input: the weights of the bones that skin this vertex."},
    {"PSIZE", "PSIZE[n]", "Vertex shader output: the size of a point sprite, in pixels."},
    {"FOG", "FOG", "Vertex shader output: the fog value, in Shader Model 3 and earlier."},
    {"DEPTH", "DEPTH[n]", "Pixel shader output: the old name of `SV_Depth`."},
    {"VFACE", "VFACE",
      "Pixel shader input: positive when the triangle faces the camera and negative when it faces away. The Shader "
      "Model 3 form of `SV_IsFrontFace`."},
    {"VPOS", "VPOS", "Pixel shader input: the pixel's position on screen, in pixels. The Shader Model 3 form of `SV_Position`."},
    {"SV_Barycentrics", "SV_Barycentrics", "Pixel shader input: the pixel's barycentric coordinates within the triangle. Shader Model 6.1; needs DXC."},
    {"SV_ClipDistance", "SV_ClipDistance[n]", "Vertex shader output: the distance to a user clip plane. The primitive is clipped where it is negative."},
    {"SV_Coverage", "SV_Coverage", "Pixel shader input or output: the mask of MSAA samples the pixel covers."},
    {"SV_CullDistance", "SV_CullDistance[n]", "Vertex shader output: a distance that culls the whole primitive when it is negative at all of its vertices."},
    {"SV_Depth", "SV_Depth", "Pixel shader output: replaces the depth written to the depth buffer. Writing it turns off early depth testing."},
    {"SV_DepthGreaterEqual", "SV_DepthGreaterEqual",
      "Pixel shader output: a depth that is at least the rasterized depth. The promise keeps some early depth testing "
      "that `SV_Depth` loses."},
    {"SV_DepthLessEqual", "SV_DepthLessEqual",
      "Pixel shader output: a depth that is at most the rasterized depth. The promise keeps some early depth testing "
      "that `SV_Depth` loses."},
    {"SV_DispatchThreadID", "SV_DispatchThreadID", "Compute shader input: the thread's index in the whole dispatch, `SV_GroupID * numthreads + SV_GroupThreadID`."},
    {"SV_DomainLocation", "SV_DomainLocation",
      "Domain shader input: where the vertex lies in the tessellated patch, as UV for quads and barycentric "
      "coordinates for triangles."},
    {"SV_GroupID", "SV_GroupID", "Compute shader input: the index of the thread group within the dispatch."},
    {"SV_GroupIndex", "SV_GroupIndex", "Compute shader input: the thread's index within its group, flattened to one number."},
    {"SV_GroupThreadID", "SV_GroupThreadID", "Compute shader input: the thread's index within its thread group."},
    {"SV_GSInstanceID", "SV_GSInstanceID", "Geometry shader input: which instance of the geometry shader this is, with `[instance(n)]`."},
    {"SV_InnerCoverage", "SV_InnerCoverage", "Pixel shader input: whether the triangle covers the whole pixel, with conservative rasterization."},
    {"SV_InsideTessFactor", "SV_InsideTessFactor", "Patch constant function output: how finely the inside of the patch is tessellated."},
    {"SV_InstanceID", "SV_InstanceID", "Vertex shader input: the index of the instance being drawn, with GPU instancing."},
    {"SV_IsFrontFace", "SV_IsFrontFace", "Pixel shader input: `true` when the triangle faces the camera."},
    {"SV_OutputControlPointID", "SV_OutputControlPointID", "Hull shader input: the index of the control point being computed."},
    {"SV_Position", "SV_Position",
      "Vertex shader output: the clip-space position the GPU rasterizes. Pixel shader input: the pixel's position in "
      "pixels, with centers at .5, and its depth in `z`."},
    {"SV_PrimitiveID", "SV_PrimitiveID", "The index of the primitive (triangle, line or point) within the draw call, generated by the GPU."},
    {"SV_RenderTargetArrayIndex", "SV_RenderTargetArrayIndex", "The render target array slice, such as a cubemap face, that the primitive is drawn to."},
    {"SV_SampleIndex", "SV_SampleIndex", "Pixel shader input: the index of the MSAA sample being shaded. Declaring it runs the pixel shader once per sample."},
    {"SV_ShadingRate", "SV_ShadingRate", "The variable rate shading rate of the primitive or pixel. Shader Model 6.4; needs DXC."},
    {"SV_StencilRef", "SV_StencilRef", "Pixel shader output: the stencil reference value for this pixel."},
    {"SV_Target", "SV_Target[n]", "Pixel shader output: the color written to render target n. `SV_Target` alone is render target 0."},
    {"SV_TessFactor", "SV_TessFactor", "Patch constant function output: how finely each edge of the patch is tessellated."},
    {"SV_VertexID", "SV_VertexID", "Vertex shader input: the index of the vertex, generated by the GPU."},
    {"SV_ViewID", "SV_ViewID", "The index of the view being drawn, for multiview rendering such as single-pass stereo. Shader Model 6.1; needs DXC."},
    {"SV_ViewportArrayIndex", "SV_ViewportArrayIndex", "The viewport the primitive is drawn to."},
  };
  constexpr ref::Entry kAttributes[] = {
    {"allow_uav_condition", "[allow_uav_condition]", "Lets the loop's exit condition depend on a value read from a UAV."},
    {"branch", "[branch]", "Compiles the `if` or `switch` as a real branch, so only the side that is taken runs."},
    {"call", "[call]", "Compiles each `case` of the `switch` as a subroutine call."},
    {"domain", "[domain(\"tri\" | \"quad\" | \"isoline\")]", "Hull and domain shaders: the kind of patch to tessellate."},
    {"earlydepthstencil", "[earlydepthstencil]", "Pixel shader: runs the depth and stencil tests before the shader, even if it writes to UAVs."},
    {"fastopt", "[fastopt]", "Compiles the loop faster by skipping some optimizations."},
    {"flatten", "[flatten]", "Runs both sides of the `if` or every `case` of the `switch` and keeps the right result, so there is no branch."},
    {"forcecase", "[forcecase]", "Compiles the `switch` as a jump table of cases."},
    {"instance", "[instance(n)]", "Geometry shader: runs the shader n times per primitive; read which one with `SV_GSInstanceID`."},
    {"loop", "[loop]", "Keeps the loop as a real loop instead of unrolling it."},
    {"maxtessfactor", "[maxtessfactor(f)]", "Hull shader: the largest tessellation factor the patch constant function returns."},
    {"maxvertexcount", "[maxvertexcount(n)]", "Geometry shader: the most vertices one invocation can output."},
    {"numthreads", "[numthreads(x, y, z)]", "Compute shader: the size of a thread group, up to 1024 threads in all."},
    {"outputcontrolpoints", "[outputcontrolpoints(n)]", "Hull shader: the number of control points it outputs; the shader runs once for each."},
    {"outputtopology", "[outputtopology(\"point\" | \"line\" | \"triangle_cw\" | \"triangle_ccw\")]", "Hull shader: the primitives the tessellator outputs."},
    {"partitioning", "[partitioning(\"integer\" | \"fractional_even\" | \"fractional_odd\" | \"pow2\")]", "Hull shader: how tessellation factors turn into subdivisions."},
    {"patchconstantfunc", "[patchconstantfunc(\"function\")]", "Hull shader: the function that computes the tessellation factors and other per-patch data."},
    {"unroll", "[unroll] | [unroll(n)]", "Unrolls the loop completely, or n times, so it runs without branching."},
    {"WaveSize", "[WaveSize(n)]", "Compute shader: requires a wave width of n lanes. Shader Model 6.6; needs DXC."},
  };
  // Methods of texture, buffer and stream objects.
  constexpr ref::Entry kMethods[] = {
    {"Append", "Object.Append(T value)", "On an `AppendStructuredBuffer`, adds `value` to the end of the buffer. On a geometry shader stream, outputs a vertex."},
    {"CalculateLevelOfDetail", "float Texture.CalculateLevelOfDetail(SamplerState s, location)", "Returns the mip level that sampling at `location` would use, clamped to the texture's mips.\n\nPixel shaders only."},
    {"CalculateLevelOfDetailUnclamped", "float Texture.CalculateLevelOfDetailUnclamped(SamplerState s, location)",
      "Returns the mip level that sampling at `location` would use, without clamping it to the texture's mips.\n\nPixel "
      "shaders only."},
    {"Consume", "T ConsumeStructuredBuffer.Consume()", "Removes a value from the buffer and returns it."},
    {"DecrementCounter", "uint RWStructuredBuffer.DecrementCounter()", "Decrements the buffer's hidden counter and returns the new value."},
    {"Gather", "float4 Texture.Gather(SamplerState s, float2 location [, int2 offset])", "Returns the red channel of the four texels bilinear filtering would blend at `location`, unfiltered."},
    {"GatherAlpha", "float4 Texture.GatherAlpha(SamplerState s, float2 location [, int2 offset])", "Returns the alpha channel of the four texels bilinear filtering would blend, unfiltered."},
    {"GatherBlue", "float4 Texture.GatherBlue(SamplerState s, float2 location [, int2 offset])", "Returns the blue channel of the four texels bilinear filtering would blend, unfiltered."},
    {"GatherCmp", "float4 Texture.GatherCmp(SamplerComparisonState s, float2 location, float compareValue [, int2 offset])",
      "Compares `compareValue` with the red channel of the four texels bilinear filtering would blend, and returns the "
      "four results."},
    {"GatherCmpAlpha", "float4 Texture.GatherCmpAlpha(SamplerComparisonState s, float2 location, float compareValue [, int2 offset])",
      "Compares `compareValue` with the alpha channel of the four texels bilinear filtering would blend, and returns the "
      "four results."},
    {"GatherCmpBlue", "float4 Texture.GatherCmpBlue(SamplerComparisonState s, float2 location, float compareValue [, int2 offset])",
      "Compares `compareValue` with the blue channel of the four texels bilinear filtering would blend, and returns the "
      "four results."},
    {"GatherCmpGreen", "float4 Texture.GatherCmpGreen(SamplerComparisonState s, float2 location, float compareValue [, int2 offset])",
      "Compares `compareValue` with the green channel of the four texels bilinear filtering would blend, and returns the "
      "four results."},
    {"GatherCmpRed", "float4 Texture.GatherCmpRed(SamplerComparisonState s, float2 location, float compareValue [, int2 offset])",
      "Compares `compareValue` with the red channel of the four texels bilinear filtering would blend, and returns the "
      "four results."},
    {"GatherGreen", "float4 Texture.GatherGreen(SamplerState s, float2 location [, int2 offset])", "Returns the green channel of the four texels bilinear filtering would blend, unfiltered."},
    {"GatherRed", "float4 Texture.GatherRed(SamplerState s, float2 location [, int2 offset])", "Returns the red channel of the four texels bilinear filtering would blend, unfiltered."},
    {"GetDimensions", "void Object.GetDimensions([uint mipLevel,] out width [, out height] [, out elementsOrDepth] [, out mipCount])",
      "Writes the size of the texture or buffer to the `out` arguments. Which arguments there are depends on the "
      "object's type."},
    {"GetSamplePosition", "float2 Texture2DMS.GetSamplePosition(int sampleIndex)", "Returns the position of the sample relative to the pixel center."},
    {"IncrementCounter", "uint RWStructuredBuffer.IncrementCounter()", "Increments the buffer's hidden counter and returns the value it had before."},
    {"Load", "T Object.Load(int3 location [, int2 offset])",
      "Reads one texel, unfiltered and without a sampler. For a texture, `location` is the pixel coordinates with the mip "
      "level last; for `Texture2DMS`, `Load(int2 location, int sampleIndex)`; for a buffer, `Load(index)`; for a "
      "`ByteAddressBuffer`, the 32-bit value at a byte address."},
    {"Load2", "uint2 ByteAddressBuffer.Load2(uint address)", "Reads two consecutive 32-bit values starting at the byte address, a multiple of 4."},
    {"Load3", "uint3 ByteAddressBuffer.Load3(uint address)", "Reads three consecutive 32-bit values starting at the byte address, a multiple of 4."},
    {"Load4", "uint4 ByteAddressBuffer.Load4(uint address)", "Reads four consecutive 32-bit values starting at the byte address, a multiple of 4."},
    {"RestartStrip", "void Stream.RestartStrip()", "Ends the current line or triangle strip of a geometry shader stream; the next `Append` starts a new one."},
    {"Sample", "T Texture.Sample(SamplerState s, location [, offset] [, float clamp] [, out uint status])",
      "Samples the texture at `location`, filtered, with the mip level chosen from screen-space derivatives.\n\nPixel "
      "shaders only; elsewhere use `SampleLevel`."},
    {"SampleBias", "T Texture.SampleBias(SamplerState s, location, float bias [, offset])", "Samples the texture after adding `bias` to the mip level it would use.\n\nPixel shaders only."},
    {"SampleCmp", "float Texture.SampleCmp(SamplerComparisonState s, location, float compareValue [, offset])",
      "Compares `compareValue` with the texels and returns the filtered result of the comparisons, from 0 to 1: "
      "hardware shadow map filtering.\n\nPixel shaders only."},
    {"SampleCmpLevelZero", "float Texture.SampleCmpLevelZero(SamplerComparisonState s, location, float compareValue [, offset])", "Like `SampleCmp`, but always reads mip level 0, so it works in every shader stage."},
    {"SampleGrad", "T Texture.SampleGrad(SamplerState s, location, ddx, ddy [, offset])",
      "Samples the texture with the mip level chosen from the given derivatives. It works inside branches and outside "
      "pixel shaders, where `Sample` cannot compute derivatives."},
    {"SampleLevel", "T Texture.SampleLevel(SamplerState s, location, float lod [, offset])", "Samples the texture at mip level `lod`. It works in every shader stage."},
    {"Store", "void RWByteAddressBuffer.Store(uint address, uint value)", "Writes a 32-bit value at the byte address, a multiple of 4."},
    {"Store2", "void RWByteAddressBuffer.Store2(uint address, uint2 values)", "Writes two consecutive 32-bit values starting at the byte address, a multiple of 4."},
    {"Store3", "void RWByteAddressBuffer.Store3(uint address, uint3 values)", "Writes three consecutive 32-bit values starting at the byte address, a multiple of 4."},
    {"Store4", "void RWByteAddressBuffer.Store4(uint address, uint4 values)", "Writes four consecutive 32-bit values starting at the byte address, a multiple of 4."},
  };
  const ref::Entry *findExact(std::span<const ref::Entry> table, std::string_view name) {
    auto it = std::find_if(table.begin(), table.end(), [&](const ref::Entry &entry) { return entry.name == name; });
    return it == table.end() ? nullptr : &*it;
  }
  const ref::Entry *findScalar(std::string_view name) {
    return findExact(kScalars, name);
  }
} // namespace
std::span<const ref::Entry> keywords() {
  return kKeywords;
}
const std::vector<std::string> &types() {
  static const std::vector<std::string> result = [] {
    std::vector<std::string> list;
    for (const ref::Entry &type : kResourceTypes)
      list.emplace_back(type.name);
    for (const ref::Entry &scalar : kScalars) {
      std::string base(scalar.name);
      list.push_back(base);
      if (base == "dword")
        continue;
      for (int rows = 1; rows <= 4; ++rows) {
        list.push_back(base + std::to_string(rows));
        for (int cols = 1; cols <= 4; ++cols)
          list.push_back(base + std::to_string(rows) + "x" + std::to_string(cols));
      }
    }
    return list;
  }();
  return result;
}
std::span<const ref::Entry> intrinsics() {
  return kIntrinsics;
}
const ref::Entry *findKeyword(std::string_view name) {
  return findExact(kKeywords, name);
}
const ref::Entry *findIntrinsic(std::string_view name) {
  // HLSL is case-sensitive, unlike ShaderLab, so ref::find does not apply.
  return findExact(kIntrinsics, name);
}
const ref::Entry *findSemantic(std::string_view name) {
  if (const ref::Entry *entry = ref::find(kSemantics, name))
    return entry;
  size_t digits = name.size();
  while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1])))
    --digits;
  if (digits == name.size() || digits == 0)
    return nullptr;
  const ref::Entry *entry = ref::find(kSemantics, name.substr(0, digits));
  return entry && entry->detail.ends_with("[n]") ? entry : nullptr;
}
const ref::Entry *findAttribute(std::string_view name) {
  return ref::find(kAttributes, name);
}
const ref::Entry *findMethod(std::string_view name) {
  return findExact(kMethods, name);
}
std::optional<TypeDoc> describeType(std::string_view name) {
  if (const ref::Entry *entry = findExact(kResourceTypes, name))
    return TypeDoc{std::string(entry->detail), std::string(entry->doc)};
  if (const ref::Entry *scalar = findScalar(name))
    return TypeDoc{std::string(scalar->detail), std::string(scalar->doc)};
  // A scalar followed by N (vector) or RxC (matrix), each from 1 to 4.
  for (const ref::Entry &scalar : kScalars) {
    if (!name.starts_with(scalar.name) || scalar.name == "dword")
      continue;
    std::string_view shape = name.substr(scalar.name.size());
    auto dimension = [](char c) { return c >= '1' && c <= '4'; };
    std::string base(scalar.name);
    std::string about = "`" + base + "`: " + std::string(scalar.doc);
    if (shape.size() == 1 && dimension(shape[0])) {
      const char *components[] = {"x", "x, y", "x, y, z", "x, y, z, w"};
      return TypeDoc{std::string(name), "A vector of " + std::string(shape) + " `" + base + "` components (" + components[shape[0] - '1'] + ").\n\n" + about};
    }
    if (shape.size() == 3 && dimension(shape[0]) && shape[1] == 'x' && dimension(shape[2])) {
      return TypeDoc{std::string(name), "A matrix of " + std::string(1, shape[0]) + " rows and " + std::string(1, shape[2]) + " columns of `" + base + "` values.\n\n" + about};
    }
  }
  return std::nullopt;
}
bool isKeywordOrType(std::string_view name) {
  static const std::unordered_set<std::string_view> set = [] {
    std::unordered_set<std::string_view> s;
    for (const ref::Entry &k : kKeywords)
      s.insert(k.name);
    for (const auto &t : types())
      s.insert(t);
    return s;
  }();
  return set.contains(name);
}
} // namespace sls::hlsl
