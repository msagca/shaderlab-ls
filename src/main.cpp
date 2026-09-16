#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>

#include "analysis/analysis.h"
#include "analysis/hlsl_check.h"
#include "common/util.h"
#include "format/formatter.h"
#include "fxc/fxc.h"
#include "lsp/server.h"
#include "lsp/transport.h"

namespace {

void printUsage() {
  std::printf("shaderlab-ls %s - ShaderLab/HLSL language server for Unity shaders (Windows, FXC)\n\n"
              "Usage:\n"
              "  shaderlab-ls [--stdio]                  Run the language server over stdio.\n"
              "  shaderlab-ls --check <file> [--editor <path>]\n"
              "                                          Print diagnostics for a .shader/.compute/.hlsl file.\n"
              "  shaderlab-ls --format <file|-> [--indent <n>] [--tabs]\n"
              "                                          Write a formatted .shader file to stdout (- reads stdin).\n"
              "  shaderlab-ls --version\n",
              SHADERLAB_LS_VERSION);
}

int formatFile(const std::string& file, const sls::FormatOptions& options) {
  _setmode(_fileno(stdout), _O_BINARY);
  std::string source;
  if (file == "-") {
    _setmode(_fileno(stdin), _O_BINARY);
    source.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
  } else {
    auto text = sls::readFile(std::filesystem::path(std::u8string(file.begin(), file.end())));
    if (!text) {
      std::fprintf(stderr, "Cannot read %s\n", file.c_str());
      return 2;
    }
    source = std::move(*text);
  }
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

int check(const std::string& file, const std::string& editor) {
  std::filesystem::path path(std::u8string(file.begin(), file.end()));
  auto text = sls::readFile(path);
  if (!text) {
    std::fprintf(stderr, "Cannot read %s\n", file.c_str());
    return 2;
  }
  auto analysis = sls::analyze(std::filesystem::absolute(path), std::move(*text));
  sls::CheckOptions options;
  options.editorOverride = std::filesystem::path(std::u8string(editor.begin(), editor.end()));
  std::vector<sls::Diagnostic> diagnostics = analysis->diagnostics;
  if (!sls::Fxc::instance().available()) {
    std::fprintf(stderr, "%s\n", sls::Fxc::instance().loadError().c_str());
  } else {
    auto fxc = sls::checkHlsl(*analysis, options, [] { return false; });
    diagnostics.insert(diagnostics.end(), fxc.begin(), fxc.end());
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
  std::string checkFile;
  std::string formatPath;
  std::string editor;
  sls::FormatOptions formatOptions;
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
    } else if (std::strcmp(argv[i], "--indent") == 0 && i + 1 < argc) {
      formatOptions.indentSize = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--tabs") == 0) {
      formatOptions.useTabs = true;
    } else if (std::strcmp(argv[i], "--stdio") != 0) {
      std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      printUsage();
      return 2;
    }
  }
  if (!formatPath.empty()) return formatFile(formatPath, formatOptions);
  if (!checkFile.empty()) return check(checkFile, editor);

  sls::Transport transport;
  sls::Server server(transport);
  return server.run();
}
