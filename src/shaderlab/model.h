#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/text.h"

namespace sls {

// Shader code blocks, per "Shader code blocks in ShaderLab reference".
enum class BlockKind { HlslProgram, HlslInclude, CgProgram, CgInclude, GlslProgram };

inline bool isProgramBlock(BlockKind kind) {
  return kind == BlockKind::HlslProgram || kind == BlockKind::CgProgram;
}
inline bool isCgBlock(BlockKind kind) { return kind == BlockKind::CgProgram || kind == BlockKind::CgInclude; }
const char* blockKeyword(BlockKind kind);

struct CodeBlock {
  BlockKind kind = BlockKind::HlslProgram;
  Span keyword;  // HLSLPROGRAM
  Span content;  // text between the keywords
  Span endKeyword;  // ENDHLSL (empty when unterminated)
  int subShader = -1;  // owning SubShader, -1 at Shader level
  int pass = -1;  // owning Pass within the SubShader, -1 if none
};

struct PropertyAttribute {
  std::string name;
  std::string arguments;  // raw text inside (...), if any
  Span nameSpan;
  Span span;  // including [ ]
};

struct MaterialProperty {
  std::string name;
  Span nameSpan;
  std::string displayName;
  std::string type;  // as written: Float, Range, 2D, ...
  Span typeSpan;
  std::string defaultValue;  // raw source text
  std::vector<PropertyAttribute> attributes;
  Span span;
};

struct Tag {
  std::string key;
  std::string value;
  Span keySpan;
  Span valueSpan;
};

struct CommandArg {
  std::string text;  // for property references, the property name without brackets
  Span span;
  bool propertyRef = false;
  bool comma = false;  // a "," separator
};

struct Command {
  std::string name;
  Span nameSpan;
  std::vector<CommandArg> args;
  std::vector<Command> children;  // Stencil { ... }
  Span span;
};

struct Pass {
  Span keyword;
  Span span;
  std::string name;
  Span nameSpan;
  std::vector<Tag> tags;
  std::vector<Command> commands;
};

struct SubShader {
  Span keyword;
  Span span;
  std::vector<Tag> tags;
  std::vector<Command> commands;
  std::vector<Pass> passes;
  std::optional<long> lod;
};

enum class ScopeKind { Shader, Properties, SubShader, Pass, Tags, Stencil, PackageRequirements, GrabPass };

// A brace-delimited region, used to work out completion context.
struct Scope {
  ScopeKind kind;
  ScopeKind parent;
  Span span;  // from '{' to just past '}' (or end of file if unclosed)
};

struct ShaderFile {
  bool hasShader = false;
  std::string name;
  Span keyword;
  Span nameSpan;
  Span span;
  Span propertiesKeyword;
  std::vector<MaterialProperty> properties;
  std::vector<SubShader> subShaders;
  std::string fallback;
  std::vector<CodeBlock> blocks;  // every code block in source order
  std::vector<Scope> scopes;
  std::vector<Diagnostic> diagnostics;

  const MaterialProperty* findProperty(std::string_view propertyName) const;
};

}  // namespace sls
