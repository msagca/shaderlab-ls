#include "format/style.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
extern char **environ;
#endif
#include <algorithm>
#include <charconv>
#include <map>
#include <thread>
#include <vector>
#include "common/util.h"
namespace sls {
namespace {
  // Unity's own layout: four spaces, braces on their own line, no empty lines. Lines stay where the author broke
  // them (ColumnLimit 0), and includes are never reordered: in HLSL their order is part of the program. Trailing
  // comments are left where they land rather than aligned, which clang-format versions disagree about.
  constexpr std::string_view kUnityStyle =
    "{BasedOnStyle: Microsoft, AlignTrailingComments: false, ColumnLimit: 0, MaxEmptyLinesToKeep: 0, "
    "SortIncludes: Never}";
  constexpr std::string_view kFileStyle = "{BasedOnStyle: InheritParentConfig, SortIncludes: Never}";
  // Hides a line from clang-format; the number is an index into the lines taken out.
  constexpr std::string_view kMarker = "<<shaderlab-ls:";
// Runs `argv` with `input` on its standard input and collects its standard output. Its standard error goes nowhere:
// the only thing the caller can do with a failure is leave the code alone. False if the process cannot be started or
// exits with a failure.
#ifdef _WIN32
  std::wstring wide(std::string_view text) {
    if (text.empty())
      return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
  }
  bool run(const std::vector<std::string> &argv, std::string_view input, std::string &output) {
    std::string commandLine;
    for (const std::string &argument : argv) {
      if (!commandLine.empty())
        commandLine += ' ';
      commandLine += '"' + argument + '"';
    }
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &inherit, 0))
      return false;
    if (!CreatePipe(&outRead, &outWrite, &inherit, 0)) {
      CloseHandle(inRead);
      CloseHandle(inWrite);
      return false;
    }
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inRead;
    startup.hStdOutput = outWrite;
    startup.hStdError = nul == INVALID_HANDLE_VALUE ? nullptr : nul;
    PROCESS_INFORMATION process{};
    std::wstring line = wide(commandLine);
    BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (nul != INVALID_HANDLE_VALUE)
      CloseHandle(nul);
    if (!started) {
      CloseHandle(inWrite);
      CloseHandle(outRead);
      return false;
    }
    // The child can fill the output pipe before it has read all of its input, so write from another thread.
    std::thread writer([inWrite, input] {
      for (size_t at = 0; at < input.size();) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(input.size() - at, 1u << 16));
        DWORD written = 0;
        if (!WriteFile(inWrite, input.data() + at, chunk, &written, nullptr) || written == 0)
          break;
        at += written;
      }
      CloseHandle(inWrite);
    });
    std::vector<char> buffer(1u << 16);
    for (DWORD read = 0; ReadFile(outRead, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0;) {
      output.append(buffer.data(), read);
    }
    writer.join();
    CloseHandle(outRead);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return exitCode == 0;
  }
#else
  bool run(const std::vector<std::string> &argv, std::string_view input, std::string &output) {
    int in[2] = {-1, -1};
    int out[2] = {-1, -1};
    if (pipe(in) != 0)
      return false;
    if (pipe(out) != 0) {
      close(in[0]);
      close(in[1]);
      return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, in[1]);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    std::vector<char *> arguments;
    for (const std::string &argument : argv)
      arguments.push_back(const_cast<char *>(argument.c_str()));
    arguments.push_back(nullptr);
    pid_t child = 0;
    int failure = posix_spawnp(&child, argv[0].c_str(), &actions, nullptr, arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in[0]);
    close(out[1]);
    if (failure != 0) {
      close(in[1]);
      close(out[0]);
      return false;
    }
    // The child can fill the output pipe before it has read all of its input, so write from another thread.
    std::thread writer([descriptor = in[1], input] {
      for (size_t at = 0; at < input.size();) {
        ssize_t written = write(descriptor, input.data() + at, std::min<size_t>(input.size() - at, 1u << 16));
        if (written < 0 && errno == EINTR)
          continue;
        if (written <= 0)
          break;
        at += static_cast<size_t>(written);
      }
      close(descriptor);
    });
    std::vector<char> buffer(1u << 16);
    for (;;) {
      ssize_t got = read(out[0], buffer.data(), buffer.size());
      if (got < 0 && errno == EINTR)
        continue;
      if (got <= 0)
        break;
      output.append(buffer.data(), static_cast<size_t>(got));
    }
    writer.join();
    close(out[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
      if (errno != EINTR)
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
#endif
  std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    for (size_t start = 0; start <= text.size();) {
      size_t end = text.find('\n', start);
      std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
      if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
      lines.push_back(line);
      if (end == std::string_view::npos)
        break;
      start = end + 1;
    }
    return lines;
  }
  // clang-format reads the code as C++, and two whole-line things in HLSL are not: attributes such as
  // `[numthreads(8, 8, 1)]`, which it glues onto the function below them, and Unity's #pragma directives, where it
  // would turn `maxcount:50` into `maxcount : 50`. Both are hidden behind a short line comment while it runs.
  bool hidden(std::string_view line) {
    std::string_view text = trim(line);
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']')
      return true;
    return istartsWith(text, "#pragma") && text.back() != '\\';
  }
  std::string hide(std::string_view code, std::vector<std::string> &lines) {
    std::string result;
    bool comment = false;
    bool continuation = false;
    for (std::string_view line : splitLines(code)) {
      std::string_view text = trim(line);
      // The block starts after its keyword, so what clang-format gets begins with the rest of the keyword's line.
      // Empty, that is an empty first line, and it would keep it.
      if (result.empty() && text.empty())
        continue;
      bool outsideComment = !comment; // a #pragma or an attribute inside a /* */ comment is text, not a directive
      comment = inBlockComment(line, comment);
      bool afterContinuation = continuation;
      continuation = !text.empty() && text.back() == '\\';
      // MaxEmptyLinesToKeep: 0 would take away the empty line that ends a macro, making the line below it part of
      // the macro instead, so that one is hidden as well.
      if (text.empty() && afterContinuation) {
        result += "//";
        result += kMarker;
        result += std::to_string(lines.size());
        result += ">>\n";
        lines.emplace_back();
        continue;
      }
      if (outsideComment && hidden(line)) {
        result += "//";
        result += kMarker;
        result += std::to_string(lines.size());
        result += ">>";
        lines.emplace_back(text);
      } else {
        result += line;
      }
      result += '\n';
    }
    return result;
  }
  // Puts the hidden lines back where clang-format left their comments, keeping the indentation it gave them.
  std::string unhide(std::string_view formatted, const std::vector<std::string> &lines) {
    std::string result;
    for (std::string_view line : splitLines(formatted)) {
      size_t marker = line.find(kMarker);
      size_t slashes = marker == std::string_view::npos ? std::string_view::npos : line.rfind("//", marker);
      size_t end = marker == std::string_view::npos ? std::string_view::npos : line.find(">>", marker);
      size_t index = 0;
      if (end != std::string_view::npos && trim(line.substr(0, slashes)).empty() &&
          trim(line.substr(slashes + 2, marker - slashes - 2)).empty()) {
        std::string_view digits = line.substr(marker + kMarker.size(), end - marker - kMarker.size());
        auto [_, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
        if (error == std::errc() && index < lines.size()) {
          result += line.substr(0, slashes);
          result += lines[index];
          result += '\n';
          continue;
        }
      }
      result += line;
      result += '\n';
    }
    if (!result.empty())
      result.pop_back(); // splitLines() already gave the trailing newline a line of its own
    return result;
  }
  // An HLSL return semantic (`float4 frag(v2f i) : SV_Target`) reads as a constructor initializer list in C++, and
  // clang-format up to at least 18 puts one on a line of its own. Joining it back keeps the colon with the signature
  // it belongs to, and keeps the output the same whichever clang-format is installed. A line that ends in ')' and
  // holds a '?' is a wrapped ternary, not a signature.
  std::string joinSemantics(std::string_view formatted) {
    std::vector<std::string> lines;
    for (std::string_view line : splitLines(formatted)) {
      std::string_view text = trim(line);
      std::string_view above = lines.empty() ? std::string_view() : trim(lines.back());
      bool signature = !above.empty() && !text.empty() && above.find(')') != std::string_view::npos &&
                       above.find('?') == std::string_view::npos;
      // The colon goes before the break, or after it with BreakConstructorInitializers: AfterColon.
      if (signature && ((above.back() == ')' && text.front() == ':') || above.back() == ':')) {
        lines.back() += ' ';
        lines.back() += text;
        continue;
      }
      lines.emplace_back(line);
    }
    std::string result;
    for (const std::string &line : lines) {
      result += line;
      result += '\n';
    }
    if (!result.empty())
      result.pop_back();
    return result;
  }
  bool hasConfigFile(const std::filesystem::path &start) {
    std::error_code error;
    for (std::filesystem::path dir = start; !dir.empty();) {
      if (std::filesystem::exists(dir / ".clang-format", error))
        return true;
      if (std::filesystem::exists(dir / "_clang-format", error))
        return true;
      std::filesystem::path parent = dir.parent_path();
      if (parent == dir)
        return false;
      dir = parent;
    }
    return false;
  }
  // `--dump-config` prints one `key: value` per line, nested under their section for BraceWrapping and friends.
  std::map<std::string, std::string> parseConfig(std::string_view config) {
    std::map<std::string, std::string> values;
    for (std::string_view line : splitLines(config)) {
      std::string_view text = trim(line);
      size_t colon = text.find(':');
      if (colon == std::string_view::npos || text.empty() || text.front() == '-' || text.front() == '#')
        continue;
      std::string_view value = trim(text.substr(colon + 1));
      if (!value.empty())
        values.emplace(text.substr(0, colon), value);
    }
    return values;
  }
  int intValue(const std::map<std::string, std::string> &values, const std::string &key, int fallback, int minimum = 1) {
    auto found = values.find(key);
    if (found == values.end())
      return fallback;
    int result = 0;
    const std::string &text = found->second;
    auto [_, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    return error == std::errc() && result >= minimum ? result : fallback;
  }
} // namespace
bool inBlockComment(std::string_view line, bool open) {
  for (size_t i = 0; i + 1 < line.size(); ++i) {
    if (open) {
      if (line[i] == '*' && line[i + 1] == '/') {
        open = false;
        ++i;
      }
    } else if (line[i] == '/' && line[i + 1] == '*') {
      open = true;
      ++i;
    } else if (line[i] == '/' && line[i + 1] == '/') {
      break;
    } else if (line[i] == '"') {
      while (++i < line.size() && line[i] != '"') {
        if (line[i] == '\\')
          ++i;
      }
    }
  }
  return open;
}
std::vector<std::string> ClangFormat::arguments(std::string_view mode) const {
  std::vector<std::string> arguments{exe_, "--style=" + style_};
  if (!assumeFilename_.empty())
    arguments.push_back("--assume-filename=" + assumeFilename_);
  arguments.emplace_back(mode);
  return arguments;
}
std::optional<std::string> ClangFormat::format(std::string_view code) const {
  if (!available())
    return std::nullopt;
  std::vector<std::string> lines;
  std::string output;
  if (!run(arguments("-"), hide(code, lines), output))
    return std::nullopt;
  return joinSemantics(unhide(output, lines));
}
std::optional<std::string> ClangFormat::dumpConfig() const {
  if (!available())
    return std::nullopt;
  std::string output;
  if (!run(arguments("--dump-config"), {}, output))
    return std::nullopt;
  return output;
}
FormatOptions resolveStyle(const std::filesystem::path &file, const std::string &clangFormatExe) {
  namespace fs = std::filesystem;
  std::error_code error;
  fs::path path = file.empty() ? fs::current_path(error) / "shader.shader" : fs::absolute(file, error);
  // clang-format picks the language from the extension, and nothing maps to HLSL: ask it about a C++ file
  // in the same folder, so that the .clang-format it finds is the one that applies to the shader.
  ClangFormat clangFormat(clangFormatExe.empty() ? "clang-format" : clangFormatExe,
    std::string(hasConfigFile(path.parent_path()) ? kFileStyle : kUnityStyle),
    displayPath(fs::path(path).replace_extension(".hlsl")));
  FormatOptions options; // Unity's defaults, for when clang-format isn't installed
  std::optional<std::string> config = clangFormat.dumpConfig();
  if (!config)
    return options;
  std::map<std::string, std::string> values = parseConfig(*config);
  options.indentSize = intValue(values, "IndentWidth", options.indentSize);
  options.tabWidth = intValue(values, "TabWidth", options.tabWidth);
  options.maxEmptyLines = intValue(values, "MaxEmptyLinesToKeep", options.maxEmptyLines, 0);
  auto useTab = values.find("UseTab");
  if (useTab != values.end())
    options.useTabs = useTab->second != "Never";
  auto afterStruct = values.find("AfterStruct");
  if (afterStruct != values.end())
    options.bracesOnOwnLine = afterStruct->second == "true";
  options.clangFormat = std::move(clangFormat);
  return options;
}
} // namespace sls
