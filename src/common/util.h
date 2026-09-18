#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace sls {

bool iequals(std::string_view a, std::string_view b);
bool istartsWith(std::string_view text, std::string_view prefix);
std::string toLower(std::string_view text);
std::string_view trim(std::string_view text);
bool isIdentStart(char c);
bool isIdentChar(char c);

std::optional<std::string> readFile(const std::filesystem::path& path);

std::filesystem::path uriToPath(std::string_view uri);
std::string pathToUri(const std::filesystem::path& path);
// Absolute, lexically normal, forward slashes, and lowercase where the file system ignores case: for comparing paths.
std::string pathKey(const std::filesystem::path& path);
// Absolute path with forward slashes, preserving case: for #line directives and display.
std::string displayPath(const std::filesystem::path& path);

}  // namespace sls
