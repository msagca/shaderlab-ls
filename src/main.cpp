#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#endif

#include <cstdio>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>

#include "analysis/analysis.h"
#include "analysis/hlsl_check.h"
#include "common/util.h"
#include "format/formatter.h"
#include "lsp/server.h"
#include "lsp/transport.h"

namespace {

void printUsage() {
  std::printf("shaderlab-ls %s - ShaderLab/HLSL language server for Unity shaders (FXC, DXC)\n\n"
              "Usage:\n"
              "  shaderlab-ls [--stdio]                  Run the language server over stdio.\n"
              "  shaderlab-ls --check <file> [--editor <path>] [--dxc <library>] [--compiler auto|fxc|dxc|none]\n"
              "                                          Print diagnostics for a .shader/.compute/.hlsl file.\n"
              "  shaderlab-ls --format <file|-> [--assume-filename <path>] [--clang-format <exe>]\n"
              "                                          Write a formatted .shader file to stdout (- reads stdin;\n"
              "                                          --assume-filename says which .clang-format applies to it).\n"
              "  shaderlab-ls --version\n",
              SHADERLAB_LS_VERSION);
}

// Windows opens the standard streams in text mode, which would rewrite the newlines of a formatted file.
void binaryStdout() {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif
}

void binaryStdin() {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
#endif
}

int formatFile(const std::string& file, const std::string& assumeFilename, const std::string& clangFormat) {
  binaryStdout();
  std::string source;
  if (file == "-") {
    binaryStdin();
    source.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
  } else {
    auto text = sls::readFile(std::filesystem::path(std::u8string(file.begin(), file.end())));
    if (!text) {
      std::fprintf(stderr, "Cannot read %s\n", file.c_str());
      return 2;
    }
    source = std::move(*text);
  }
  const std::string& styleFor = file == "-" ? assumeFilename : file;
  sls::FormatOptions options = sls::resolveStyle(std::filesystem::path(std::u8string(styleFor.begin(), styleFor.end())),
                                                clangFormat);
  sls::FormatResult result = sls::formatShaderLab(source, options);
  if (!result.ok) {
    std::fprintf(stderr, "shaderlab-ls: can't format %s: %s\n", file == "-" ? "stdin" : file.c_str(), result.error.c_str());
    return 1;
  }
  std::fwrite(result.text.data(), 1, result.text.size(), stdout);
  return 0;
}

const char* severityName(sls::Severity severity) {
  switch (severity) {
    case sls::Severity::Error: return "error";
    case sls::Severity::Warning: return "warning";
    case sls::Severity::Information: return "info";
    case sls::Severity::Hint: return "hint";
  }
  return "";
}

sls::CompilerChoice compilerChoice(std::string_view name) {
  if (name == "fxc") return sls::CompilerChoice::Fxc;
  if (name == "dxc") return sls::CompilerChoice::Dxc;
  if (name == "none") return sls::CompilerChoice::None;
  return sls::CompilerChoice::Auto;
}

int check(const std::string& file, const std::string& editor, const std::string& dxc, const std::string& compiler) {
  std::filesystem::path path(std::u8string(file.begin(), file.end()));
  auto text = sls::readFile(path);
  if (!text) {
    std::fprintf(stderr, "Cannot read %s\n", file.c_str());
    return 2;
  }
  auto analysis = sls::analyze(std::filesystem::absolute(path), std::move(*text));
  sls::CheckOptions options;
  options.editorOverride = std::filesystem::path(std::u8string(editor.begin(), editor.end()));
  options.dxcLibrary = std::filesystem::path(std::u8string(dxc.begin(), dxc.end()));
  options.compiler = compilerChoice(compiler);
  std::vector<sls::Diagnostic> diagnostics = analysis->diagnostics;
  if (sls::canCompileHlsl(analysis->path, options)) {
    auto compiled = sls::checkHlsl(*analysis, options, [] { return false; });
    diagnostics.insert(diagnostics.end(), compiled.begin(), compiled.end());
  } else if (options.compiler != sls::CompilerChoice::None) {
    std::fprintf(stderr, "No HLSL compiler: FXC exists on Windows only, and no DXC was found (see --dxc).\n");
  }
  int errors = 0;
  for (const sls::Diagnostic& diagnostic : diagnostics) {
    sls::Position position = analysis->lines.toPosition(analysis->text, diagnostic.span.begin, sls::Encoding::Utf8);
    std::printf("%s:%d:%d: %s: %s [%s%s%s]\n", file.c_str(), position.line + 1, position.character + 1,
                severityName(diagnostic.severity), diagnostic.message.c_str(), diagnostic.source.c_str(),
                diagnostic.code.empty() ? "" : " ", diagnostic.code.c_str());
    if (diagnostic.severity == sls::Severity::Error) ++errors;
  }
  return errors > 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
  // clang-format may exit before it has read all of its input; that must not take the server with it.
  std::signal(SIGPIPE, SIG_IGN);
#endif
  std::string checkFile;
  std::string formatPath;
  std::string editor;
  std::string assumeFilename;
  std::string clangFormat;
  std::string dxc;
  std::string compiler;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version") == 0) {
      std::printf("shaderlab-ls %s\n", SHADERLAB_LS_VERSION);
      return 0;
    }
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      printUsage();
      return 0;
    }
    if (std::strcmp(argv[i], "--check") == 0 && i + 1 < argc) {
      checkFile = argv[++i];
    } else if (std::strcmp(argv[i], "--editor") == 0 && i + 1 < argc) {
      editor = argv[++i];
    } else if (std::strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
      formatPath = argv[++i];
    } else if (std::strcmp(argv[i], "--assume-filename") == 0 && i + 1 < argc) {
      assumeFilename = argv[++i];
    } else if (std::strcmp(argv[i], "--clang-format") == 0 && i + 1 < argc) {
      clangFormat = argv[++i];
    } else if (std::strcmp(argv[i], "--dxc") == 0 && i + 1 < argc) {
      dxc = argv[++i];
    } else if (std::strcmp(argv[i], "--compiler") == 0 && i + 1 < argc) {
      compiler = argv[++i];
    } else if (std::strcmp(argv[i], "--stdio") != 0) {
      std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      printUsage();
      return 2;
    }
  }
  if (!formatPath.empty()) return formatFile(formatPath, assumeFilename, clangFormat);
  if (!checkFile.empty()) return check(checkFile, editor, dxc, compiler);

  sls::Transport transport;
  sls::Server server(transport);
  return server.run();
}
