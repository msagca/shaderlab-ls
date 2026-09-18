#include "compiler/fxc.h"

#ifdef _WIN32
#include <windows.h>

#include <d3dcompiler.h>
#endif

#include <cstdlib>
#include <set>
#include <unordered_map>

#include "common/util.h"

namespace fs = std::filesystem;

namespace sls {

#ifdef _WIN32

namespace {

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);
using D3DPreprocessFn =
    HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, ID3DBlob**, ID3DBlob**);

class IncludeHandler final : public ID3DInclude {
 public:
  IncludeHandler(const IncludeOpener& opener, fs::path mainDir) : opener_(opener), mainDir_(std::move(mainDir)) {}

  HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR fileName, LPCVOID parentData, LPCVOID* data,
                                 UINT* bytes) noexcept override {
    // Exceptions must not unwind through d3dcompiler.
    try {
      return open(fileName, parentData, data, bytes);
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE Close(LPCVOID) noexcept override { return S_OK; }

  std::map<std::string, fs::path> takeIncludes() { return std::move(includes_); }

 private:
  HRESULT open(LPCSTR fileName, LPCVOID parentData, LPCVOID* data, UINT* bytes) {
    fs::path includerDir = mainDir_;
    if (parentData) {
      auto parent = parents_.find(parentData);
      if (parent != parents_.end()) includerDir = parent->second.parent_path();
    }
    auto file = opener_(fileName, includerDir);
    if (!file || !file->text) return E_FAIL;
    includes_[fileName] = file->path;
    std::string key = pathKey(file->path);
    if (file->pragmaOnce && seen_.contains(key)) {
      static const char kEmpty[] = "\n";
      *data = kEmpty;
      *bytes = 1;
      return S_OK;
    }
    seen_.insert(key);
    keepAlive_.push_back(file->text);
    *data = file->text->data();
    *bytes = static_cast<UINT>(file->text->size());
    parents_[file->text->data()] = file->path;
    return S_OK;
  }

  const IncludeOpener& opener_;
  fs::path mainDir_;
  std::unordered_map<LPCVOID, fs::path> parents_;
  std::set<std::string> seen_;
  std::vector<std::shared_ptr<const std::string>> keepAlive_;
  std::map<std::string, fs::path> includes_;
};

std::string blobText(ID3DBlob* blob) {
  if (!blob) return {};
  std::string text(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
  while (!text.empty() && text.back() == '\0') text.pop_back();
  return text;
}

struct Macros {
  explicit Macros(const std::vector<ShaderDefine>& defines) {
    for (const ShaderDefine& define : defines) list.push_back({define.name.c_str(), define.value.c_str()});
    list.push_back({nullptr, nullptr});
  }
  std::vector<D3D_SHADER_MACRO> list;
};

}  // namespace

Fxc& Fxc::instance() {
  static Fxc fxc;
  return fxc;
}

Fxc::Fxc() {
  std::vector<std::wstring> candidates{L"d3dcompiler_47.dll"};
  wchar_t programFiles[MAX_PATH];
  if (GetEnvironmentVariableW(L"ProgramFiles(x86)", programFiles, MAX_PATH)) {
    candidates.push_back(std::wstring(programFiles) + L"\\Windows Kits\\10\\Redist\\D3D\\x64\\d3dcompiler_47.dll");
  }
  for (const std::wstring& candidate : candidates) {
    HMODULE module = LoadLibraryW(candidate.c_str());
    if (!module) continue;
    compile_ = reinterpret_cast<void*>(GetProcAddress(module, "D3DCompile"));
    preprocess_ = reinterpret_cast<void*>(GetProcAddress(module, "D3DPreprocess"));
    if (compile_ && preprocess_) {
      wchar_t path[MAX_PATH];
      if (GetModuleFileNameW(module, path, MAX_PATH)) libraryPath_ = displayPath(fs::path(path));
      return;
    }
    compile_ = preprocess_ = nullptr;
  }
  loadError_ = "d3dcompiler_47.dll (FXC) could not be loaded.";
}

CompileResult Fxc::compile(const std::string& source, const std::string& sourceName, const fs::path& sourceDir,
                       const std::vector<ShaderDefine>& defines, const std::string& entry, const std::string& profile,
                       const IncludeOpener& opener) const {
  CompileResult result;
  if (!compile_) return result;
  IncludeHandler include(opener, sourceDir);
  Macros macros(defines);
  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  UINT flags = D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_SKIP_OPTIMIZATION;
  HRESULT hr = reinterpret_cast<D3DCompileFn>(compile_)(source.data(), source.size(), sourceName.c_str(), macros.list.data(),
                                                   &include, entry.c_str(), profile.c_str(), flags, 0, &code, &errors);
  result.success = SUCCEEDED(hr);
  result.messages = parseFxcMessages(blobText(errors));
  result.includes = include.takeIncludes();
  if (code) code->Release();
  if (errors) errors->Release();
  return result;
}

CompileResult Fxc::preprocess(const std::string& source, const std::string& sourceName, const fs::path& sourceDir,
                          const std::vector<ShaderDefine>& defines, const IncludeOpener& opener) const {
  CompileResult result;
  if (!preprocess_) return result;
  IncludeHandler include(opener, sourceDir);
  Macros macros(defines);
  ID3DBlob* text = nullptr;
  ID3DBlob* errors = nullptr;
  HRESULT hr = reinterpret_cast<D3DPreprocessFn>(preprocess_)(source.data(), source.size(), sourceName.c_str(),
                                                         macros.list.data(), &include, &text, &errors);
  result.success = SUCCEEDED(hr);
  result.messages = parseFxcMessages(blobText(errors));
  result.includes = include.takeIncludes();
  if (text) text->Release();
  if (errors) errors->Release();
  return result;
}

#else

// FXC is d3dcompiler_47.dll, which exists on Windows only. Elsewhere HLSL is compiled with DXC, if at all; callers
// ask available().
Fxc& Fxc::instance() {
  static Fxc fxc;
  return fxc;
}

Fxc::Fxc() { loadError_ = "FXC (d3dcompiler_47.dll) exists on Windows only."; }

CompileResult Fxc::compile(const std::string&, const std::string&, const fs::path&, const std::vector<ShaderDefine>&,
                       const std::string&, const std::string&, const IncludeOpener&) const {
  return {};
}

CompileResult Fxc::preprocess(const std::string&, const std::string&, const fs::path&, const std::vector<ShaderDefine>&,
                          const IncludeOpener&) const {
  return {};
}

#endif

std::string Fxc::profile(std::string_view stage, int model) const {
  return std::string(stage) + (model >= 50 ? "_5_0" : "_4_0");
}

// Lines look like:
//   C:/path/file.shader(40,34-48): error X3004: undeclared identifier 'foo'
//   file.hlsl(1,9): warning X3568: 'vertex' : unknown pragma ignored
//   error X3501: 'Nope': entrypoint not found
std::vector<CompilerMessage> parseFxcMessages(std::string_view output) {
  std::vector<CompilerMessage> messages;
  size_t start = 0;
  while (start < output.size()) {
    size_t end = output.find('\n', start);
    if (end == std::string_view::npos) end = output.size();
    std::string_view line = output.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (trim(line).empty()) continue;

    CompilerMessage message;
    std::string_view rest = line;
    size_t severityAt = std::string_view::npos;
    for (std::string_view marker : {": error ", ": warning "}) {
      size_t at = line.find(marker);
      if (at != std::string_view::npos && (severityAt == std::string_view::npos || at < severityAt)) severityAt = at;
    }
    if (severityAt != std::string_view::npos) {
      std::string_view location = line.substr(0, severityAt);
      rest = line.substr(severityAt + 2);
      if (!location.empty() && location.back() == ')') {
        size_t open = location.rfind('(');
        if (open != std::string_view::npos) {
          message.file = std::string(location.substr(0, open));
          std::string numbers(location.substr(open + 1, location.size() - open - 2));
          char* cursor = numbers.data();
          message.line = static_cast<int>(std::strtol(cursor, &cursor, 10));
          if (*cursor == ',') message.column = static_cast<int>(std::strtol(cursor + 1, &cursor, 10));
          if (*cursor == '-') message.columnEnd = static_cast<int>(std::strtol(cursor + 1, &cursor, 10));
        }
      }
    } else if (!(line.rfind("error ", 0) == 0 || line.rfind("warning ", 0) == 0)) {
      // Continuation of the previous message (for example candidate lists).
      if (!messages.empty() && line.find("compilation failed") == std::string_view::npos) {
        messages.back().text += "\n" + std::string(trim(line));
      }
      continue;
    }
    message.warning = rest.rfind("warning", 0) == 0;
    rest.remove_prefix(message.warning ? 7 : 5);
    rest = trim(rest);
    size_t colon = rest.find(':');
    if (colon != std::string_view::npos && colon > 0 && rest[0] == 'X') {
      message.code = std::string(rest.substr(0, colon));
      rest = trim(rest.substr(colon + 1));
    }
    message.text = std::string(rest);
    // FXC repeats the location and code on each continuation line of a multi-line message.
    if (!messages.empty()) {
      CompilerMessage& previous = messages.back();
      if (previous.file == message.file && previous.line == message.line && previous.column == message.column &&
          previous.code == message.code && previous.warning == message.warning && !message.code.empty()) {
        previous.text += "\n" + message.text;
        continue;
      }
    }
    messages.push_back(std::move(message));
  }
  return messages;
}

}  // namespace sls
