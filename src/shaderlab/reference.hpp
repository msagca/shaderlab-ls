#pragma once
#include <span>
#include <string_view>
namespace sls::ref {
// Every table here is transcribed from the Unity 6.6 Manual "ShaderLab language reference"
// (docs.unity3d.com/Manual/SL-Reference.html) and the pages it links to.
struct Entry {
  std::string_view name;
  std::string_view detail; // signature or short label
  std::string_view doc;
  std::string_view snippet = {}; // LSP snippet body, empty for plain insertion
};
// Keywords valid directly inside each block (commands are listed separately).
std::span<const Entry> shaderBlockKeywords();
std::span<const Entry> subShaderBlockKeywords();
std::span<const Entry> passBlockKeywords();
// GPU render state commands, valid in SubShader and Pass.
std::span<const Entry> commands();
std::span<const Entry> stencilFields();
std::span<const Entry> blendFactors();
std::span<const Entry> blendOperations();
std::span<const Entry> cullModes();
std::span<const Entry> zTestOperations();
std::span<const Entry> stencilComparisons();
std::span<const Entry> stencilOperations();
std::span<const Entry> onOff();
std::span<const Entry> trueFalse();
std::span<const Entry> colorMaskValues();
std::span<const Entry> propertyTypes();
std::span<const Entry> propertyAttributes();
std::span<const Entry> subShaderTags();
std::span<const Entry> passTags();
// Known values for a tag key, or empty if the tag takes free-form values.
std::span<const Entry> tagValues(std::string_view key);
// HLSL pragma directives (SL-PragmaDirectives and linked pages).
std::span<const Entry> pragmas();
std::span<const Entry> pragmaTargets();
std::span<const Entry> pragmaRequires();
std::span<const Entry> renderers();
std::span<const Entry> preprocessorDirectives();
// Legacy fixed-function commands (see isLegacyCommand) and the commands inside their blocks.
std::span<const Entry> legacyCommands();
std::span<const Entry> legacySubCommands();
// Built-in texture names a texture property can default to: "white", "bump", ...
std::span<const Entry> textureDefaults();
const Entry *find(std::span<const Entry> table, std::string_view name); // case-insensitive
const Entry *findCommand(std::string_view name);
const Entry *findKeywordAnywhere(std::string_view name);
// Values accepted by a single-valued command (Cull, ZWrite, ...). Empty for Blend, ColorMask, Offset, Stencil.
std::span<const Entry> commandValues(std::string_view command);
// Values for a Stencil field.
std::span<const Entry> stencilFieldValues(std::string_view field);
// Non-command keywords that begin a statement in a Shader, SubShader or Pass block (Pass, Tags, LOD, ...).
bool isScopeKeyword(std::string_view word);
// Fixed-function commands missing from the current reference but still used by Unity's own shaders
// (`Lighting Off`, `Fog { Mode Off }`, `SetTexture [_MainTex] { ... }`).
bool isLegacyCommand(std::string_view word);
// Pragmas that FXC understands; every other #pragma is a Unity directive.
bool isStandardHlslPragma(std::string_view name);
// multi_compile, shader_feature, dynamic_branch plus optional _local / stage suffixes.
bool isKeywordPragma(std::string_view name);
bool isShortcutKeywordPragma(std::string_view name); // multi_compile_fog etc.
} // namespace sls::ref
