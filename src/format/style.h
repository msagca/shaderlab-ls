#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sls {

// clang-format, resolved for one document: the executable to run, the style to run it with and the file name it
// resolves a .clang-format from. Unavailable when clang-format is not installed or cannot be run.
class ClangFormat {
 public:
  ClangFormat() = default;
  ClangFormat(std::string exe, std::string style, std::string assumeFilename)
      : exe_(std::move(exe)), style_(std::move(style)), assumeFilename_(std::move(assumeFilename)) {}

  bool available() const { return !exe_.empty(); }
  // The code of one HLSL block, formatted and dedented. Nothing when clang-format fails, in which case the
  // caller keeps the code as it was written.
  std::optional<std::string> format(std::string_view code) const;
  // The effective configuration, as `--dump-config` prints it. Nothing when clang-format cannot be run.
  std::optional<std::string> dumpConfig() const;

 private:
  std::vector<std::string> arguments(std::string_view mode) const;

  std::string exe_;
  std::string style_;
  std::string assumeFilename_;
};

struct FormatOptions {
  int indentSize = 4;
  int tabWidth = 4;  // the width of a '\t' when measuring indentation that is already there
  int maxEmptyLines = 0;  // from MaxEmptyLinesToKeep: empty lines in a row to keep
  bool useTabs = false;
  bool bracesOnOwnLine = true;  // from BraceWrapping.AfterStruct: `SubShader\n{` or `SubShader {`
  ClangFormat clangFormat;
};

// Whether a block comment is still open at the end of `line`, given whether one was open before it.
bool inBlockComment(std::string_view line, bool open);

// The style to format `file` with: the .clang-format that applies to it, as clang-format itself resolves it, or
// Unity's own layout (four spaces, braces on their own line) when no folder from `file` up to the root has one.
// `file` may be empty for text that is not on disk; the search then starts at the current directory. `clangFormatExe`
// overrides the clang-format to run; by default it is looked up on PATH.
FormatOptions resolveStyle(const std::filesystem::path& file, const std::string& clangFormatExe = {});

}  // namespace sls
