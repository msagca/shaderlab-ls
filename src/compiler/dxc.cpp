#include "compiler/dxc.h"

#ifdef _WIN32
#include <windows.h>

#include <objbase.h>
#else
#include <dlfcn.h>
#endif

// DXC's API headers, fetched at configure time (see CMakeLists.txt). On Linux WinAdapter.h stands in for the COM
// types; its IUnknown is header-only, so nothing needs linking: the library is loaded when first used.
#include "dxc/dxcapi.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "common/util.h"

namespace fs = std::filesystem;

namespace sls {

namespace {

// DXC's LPCWSTR is wchar_t text: UTF-16 on Windows, UTF-32 elsewhere.
std::wstring widen(std::string_view text) {
  std::wstring result;
  for (size_t i = 0; i < text.size();) {
    unsigned char lead = static_cast<unsigned char>(text[i]);
    int extra = lead < 0x80 ? 0 : lead < 0xE0 ? 1 : lead < 0xF0 ? 2 : 3;
    char32_t code = extra == 0 ? lead : lead & (0x3F >> extra);
    for (int k = 1; k <= extra && i + k < text.size(); ++k) code = (code << 6) | (text[i + k] & 0x3F);
    i += static_cast<size_t>(extra) + 1;
    if (sizeof(wchar_t) == 2 && code >= 0x10000) {
      code -= 0x10000;
      result.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
      result.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
    } else {
      result.push_back(static_cast<wchar_t>(code));
    }
  }
  return result;
}

std::string narrow(std::wstring_view text) {
  std::string result;
  for (size_t i = 0; i < text.size(); ++i) {
    char32_t code = static_cast<char32_t>(text[i]);
    if (sizeof(wchar_t) == 2 && code >= 0xD800 && code < 0xDC00 && i + 1 < text.size()) {
      code = 0x10000 + ((code - 0xD800) << 10) + (static_cast<char32_t>(text[++i]) - 0xDC00);
    }
    if (code < 0x80) {
      result.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      result.push_back(static_cast<char>(0xC0 | (code >> 6)));
      result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      result.push_back(static_cast<char>(0xE0 | (code >> 12)));
      result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      result.push_back(static_cast<char>(0xF0 | (code >> 18)));
      result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }
  return result;
}

// Forward slashes and no "./" steps, so that paths DXC builds can be compared with the ones handed to it.
std::string normalize(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  for (size_t at; (at = path.find("/./")) != std::string::npos;) path.erase(at, 2);
  while (path.rfind("./", 0) == 0) path.erase(0, 2);
  return path;
}

template <typename T>
struct Releaser {
  void operator()(T* object) const {
    if (object) object->Release();
  }
};
template <typename T>
using ComPtr = std::unique_ptr<T, Releaser<T>>;

// The names in a file's #include lines, as written.
std::set<std::string> includeNames(std::string_view text) {
  std::set<std::string> names;
  for (size_t at = 0; (at = text.find("include", at)) != std::string_view::npos; at += 7) {
    size_t hash = at;
    while (hash > 0 && (text[hash - 1] == ' ' || text[hash - 1] == '\t')) --hash;
    if (hash == 0 || text[hash - 1] != '#') continue;
    size_t open = text.find_first_not_of(" \t", at + 7);
    if (open == std::string_view::npos || (text[open] != '"' && text[open] != '<')) continue;
    size_t close = text.find(text[open] == '"' ? '"' : '>', open + 1);
    if (close == std::string_view::npos) continue;
    names.insert(normalize(std::string(text.substr(open + 1, close - open - 1))));
  }
  return names;
}

// DXC asks for an include by the path it would have on disk: the including file's folder joined with the name in
// the #include. Unity resolves includes its own way (Packages/, Assets/, CGIncludes), so a request has to be split
// back into the #include name and the real folder of the file that included it. Every served file is remembered
// with the folder DXC believes it is in, the folder it really is in and the names it #includes. Several files can
// share the folder DXC believes in (HLSLSupport.cginc is "next to" the shader that includes it), so the includer is
// the one whose own #include lines hold the name asked for.
class IncludeHandler final : public IDxcIncludeHandler {
 public:
  IncludeHandler(IDxcUtils* utils, const IncludeOpener& opener, const std::string& mainDir, const fs::path& realDir,
                 std::string_view mainSource)
      : utils_(utils), opener_(opener) {
    folders_.push_back({mainDir, realDir, includeNames(mainSource)});
  }

  HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR fileName, IDxcBlob** source) override {
    *source = nullptr;
    // Exceptions must not unwind through dxcompiler.
    try {
      return load(narrow(fileName), source);
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
    if (id == __uuidof(IDxcIncludeHandler) || id == __uuidof(IUnknown)) {
      *object = this;
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  // The handler lives on the stack for one compile.
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  std::map<std::string, fs::path> takeIncludes() { return std::move(includes_); }

 private:
  struct Folder {
    std::string asked;  // the folder DXC believes the file is in, normalized
    fs::path real;  // the folder it is in
    std::set<std::string> includes;  // the names its #include lines ask for
  };

  struct Candidate {
    const Folder* folder;
    std::string name;
    bool named;  // the file in `folder` has an #include of exactly `name`
  };

  HRESULT load(const std::string& requested, IDxcBlob** source) {
    std::string asked = normalize(requested);
    std::vector<Candidate> candidates;
    for (size_t i = folders_.size(); i-- > 0;) {  // the latest first
      const Folder& folder = folders_[i];
      if (asked.size() <= folder.asked.size() + 1 || asked.compare(0, folder.asked.size(), folder.asked) != 0 ||
          asked[folder.asked.size()] != '/') {
        continue;
      }
      std::string name = asked.substr(folder.asked.size() + 1);
      bool named = folder.includes.contains(name);
      candidates.push_back({&folder, std::move(name), named});
    }
    // Includers that have this very #include first, then the deepest folder.
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
      if (a.named != b.named) return a.named;
      return a.folder->asked.size() > b.folder->asked.size();
    });
    if (candidates.empty()) candidates.push_back({&folders_.front(), asked, false});  // an absolute path

    for (const Candidate& candidate : candidates) {
      auto file = opener_(candidate.name, candidate.folder->real);
      if (file && file->text) return serve(requested, asked, *file, source);
    }
    return E_FAIL;
  }

  HRESULT serve(const std::string& requested, const std::string& asked, const IncludeFile& file, IDxcBlob** source) {
    includes_[requested] = file.path;
    includes_[asked] = file.path;
    size_t slash = asked.rfind('/');
    folders_.push_back({slash == std::string::npos ? std::string() : asked.substr(0, slash), file.path.parent_path(),
                        includeNames(*file.text)});

    std::string key = pathKey(file.path);
    bool repeat = file.pragmaOnce && seen_.contains(key);
    seen_.insert(key);
    const std::string empty = "\n";
    const std::string& text = repeat ? empty : *file.text;
    IDxcBlobEncoding* blob = nullptr;
    HRESULT hr = utils_->CreateBlob(text.data(), static_cast<UINT32>(text.size()), DXC_CP_UTF8, &blob);
    if (FAILED(hr)) return hr;
    *source = blob;
    return S_OK;
  }

  IDxcUtils* utils_;
  const IncludeOpener& opener_;
  std::vector<Folder> folders_;
  std::set<std::string> seen_;
  std::map<std::string, fs::path> includes_;
};

#ifdef _WIN32
void* openLibrary(const fs::path& path) {
  HMODULE module = path.has_parent_path()
                       // Its dependencies (dxil.dll) come from its own folder.
                       ? LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)
                       : LoadLibraryW(path.c_str());
  return reinterpret_cast<void*>(module);
}

void* findSymbol(void* library, const char* name) {
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
}

std::string loadedFrom(void* library, const fs::path& fallback) {
  wchar_t path[MAX_PATH];
  if (GetModuleFileNameW(static_cast<HMODULE>(library), path, MAX_PATH)) return displayPath(fs::path(path));
  return fallback.string();
}
#else
void* openLibrary(const fs::path& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }

void* findSymbol(void* library, const char* name) { return dlsym(library, name); }

std::string loadedFrom(void* library, const fs::path& fallback) {
  Dl_info info{};
  if (void* create = dlsym(library, "DxcCreateInstance"); create && dladdr(create, &info) && info.dli_fname) {
    return displayPath(fs::path(info.dli_fname));
  }
  return fallback.string();
}
#endif

}  // namespace

std::string_view dxcLibraryName() {
#ifdef _WIN32
  return "dxcompiler.dll";
#else
  return "libdxcompiler.so";
#endif
}

const Dxc& Dxc::load(const fs::path& library) {
  static std::mutex mutex;
  static std::map<std::string, std::unique_ptr<Dxc>> loaded;
  std::lock_guard lock(mutex);
  std::unique_ptr<Dxc>& dxc = loaded[library.empty() ? std::string() : pathKey(library)];
  if (!dxc) dxc.reset(new Dxc(library));
  return *dxc;
}

Dxc::Dxc(const fs::path& library) {
  fs::path path = library.empty() ? fs::path(dxcLibraryName()) : library;
  void* module = openLibrary(path);
  if (module) create_ = findSymbol(module, "DxcCreateInstance");
  if (create_) {
    libraryPath_ = loadedFrom(module, path);
    return;
  }
  loadError_ = library.empty() ? std::string(dxcLibraryName()) + " (DXC) was not found."
                               : displayPath(library) + " could not be loaded as DXC.";
}

std::string Dxc::profile(std::string_view stage, int model) const {
  int version = std::max(model, 60);
  return std::string(stage) + "_" + std::to_string(version / 10) + "_" + std::to_string(version % 10);
}

CompileResult Dxc::compile(const std::string& source, const std::string& sourceName, const fs::path& sourceDir,
                           const std::vector<ShaderDefine>& defines, const std::string& entry,
                           const std::string& profile, const IncludeOpener& opener) const {
  return run(source, sourceName, sourceDir, {"-E", entry, "-T", profile}, defines, opener);
}

CompileResult Dxc::preprocess(const std::string& source, const std::string& sourceName, const fs::path& sourceDir,
                              const std::vector<ShaderDefine>& defines, const IncludeOpener& opener) const {
  return run(source, sourceName, sourceDir, {"-P"}, defines, opener);
}

CompileResult Dxc::run(const std::string& source, const std::string& sourceName, const fs::path& sourceDir,
                       std::vector<std::string> arguments, const std::vector<ShaderDefine>& defines,
                       const IncludeOpener& opener) const {
  CompileResult result;
  if (!create_) return result;
  auto create = reinterpret_cast<DxcCreateInstanceProc>(create_);
  IDxcUtils* rawUtils = nullptr;
  IDxcCompiler3* rawCompiler = nullptr;
  if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&rawUtils)))) return result;
  ComPtr<IDxcUtils> utils(rawUtils);
  if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&rawCompiler)))) return result;
  ComPtr<IDxcCompiler3> compiler(rawCompiler);

  // The main file gets a path in the document's folder, so that DXC asks for its includes relative to it.
  std::string mainDir = normalize(displayPath(sourceDir));
  arguments.push_back(mainDir + "/" + sourceName);
  // Unity's HLSL is written for the 2018 language; 2021 rejects much of it (?: and && on vectors). Diagnostics need
  // neither optimization nor the DXIL validator, which would also need dxil.dll.
  for (const char* flag : {"-HV", "2018", "-Od", "-Vd"}) arguments.emplace_back(flag);
  for (const ShaderDefine& define : defines) {
    arguments.push_back("-D");
    arguments.push_back(define.value.empty() ? define.name : define.name + "=" + define.value);
  }
  std::vector<std::wstring> wide;
  for (const std::string& argument : arguments) wide.push_back(widen(argument));
  std::vector<LPCWSTR> pointers;
  for (const std::wstring& argument : wide) pointers.push_back(argument.c_str());

  IncludeHandler include(utils.get(), opener, mainDir, sourceDir, source);
  DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
  IDxcResult* rawResult = nullptr;
  if (FAILED(compiler->Compile(&buffer, pointers.data(), static_cast<UINT32>(pointers.size()), &include,
                               IID_PPV_ARGS(&rawResult)))) {
    return result;
  }
  ComPtr<IDxcResult> compiled(rawResult);
  HRESULT status = E_FAIL;
  compiled->GetStatus(&status);
  result.success = SUCCEEDED(status);
  IDxcBlobUtf8* rawErrors = nullptr;
  if (SUCCEEDED(compiled->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&rawErrors), nullptr)) && rawErrors) {
    ComPtr<IDxcBlobUtf8> errors(rawErrors);
    result.messages = parseDxcMessages(std::string_view(errors->GetStringPointer(), errors->GetStringLength()));
  }
  result.includes = include.takeIncludes();
  return result;
}

namespace {

constexpr std::string_view kSeverities[] = {"fatal error", "error", "warning", "note"};

// Splits off a trailing ":<digits>" from `location`; 0 when there is none.
int popNumber(std::string_view& location) {
  size_t colon = location.rfind(':');
  if (colon == std::string_view::npos || colon + 1 >= location.size()) return 0;
  std::string_view digits = location.substr(colon + 1);
  if (digits.find_first_not_of("0123456789") != std::string_view::npos) return 0;
  location = location.substr(0, colon);
  return std::atoi(std::string(digits).c_str());
}

// A diagnostic line is "<severity>: <text>" or "<path>:<line>[:<column>]: <severity>: <text>", where the path may
// hold ':' itself (C:/...). Anything else is a quoted source line, a caret or an "In file included from".
bool splitDiagnostic(std::string_view line, std::string_view& location, std::string_view& severity,
                     std::string_view& text) {
  // A severity may start the line, or follow any ": " whose left side ends in a line or column number.
  for (size_t at = 0; at != std::string_view::npos; at = line.find(": ", at + 1)) {
    std::string_view rest = line.substr(at == 0 ? 0 : at + 2);
    if (at != 0 && !std::isdigit(static_cast<unsigned char>(line[at - 1]))) continue;
    for (std::string_view name : kSeverities) {
      if (rest.size() < name.size() + 2 || rest.substr(0, name.size()) != name || rest.substr(name.size(), 2) != ": ") {
        continue;
      }
      location = line.substr(0, at);
      severity = name;
      text = rest.substr(name.size() + 2);
      return true;
    }
  }
  return false;
}

}  // namespace

std::vector<CompilerMessage> parseDxcMessages(std::string_view output) {
  std::vector<CompilerMessage> messages;
  for (size_t start = 0; start < output.size();) {
    size_t end = output.find('\n', start);
    if (end == std::string_view::npos) end = output.size();
    std::string_view line = output.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    std::string_view location, severity, text;
    if (!splitDiagnostic(line, location, severity, text)) continue;
    CompilerMessage message;
    text = trim(text);
    if (!location.empty()) {
      int last = popNumber(location);
      int first = popNumber(location);
      message.line = first > 0 ? first : last;
      message.column = first > 0 ? last : 0;
      message.file = std::string(location);
    }
    // "[-Wconversion]" names the warning; it is the closest thing DXC has to FXC's X codes.
    if (text.size() > 4 && text.back() == ']') {
      size_t open = text.rfind(" [-W");
      if (open != std::string_view::npos) {
        message.code = std::string(text.substr(open + 2, text.size() - open - 3));
        text = trim(text.substr(0, open));
      }
    }
    message.text = std::string(text);
    if (severity == "note") {
      if (!messages.empty()) messages.back().text += "\n" + message.text;
      continue;
    }
    message.warning = severity == "warning";
    messages.push_back(std::move(message));
  }
  return messages;
}

}  // namespace sls
