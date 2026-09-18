#include "common/util.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace sls {

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool istartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
}

std::string toLower(std::string_view text) {
  std::string result(text);
  for (char& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}

std::string_view trim(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }

bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::optional<std::string> readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

namespace {

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::filesystem::path uriToPath(std::string_view uri) {
  std::string_view rest = uri;
  if (istartsWith(rest, "file://")) rest.remove_prefix(7);
  std::string decoded;
  decoded.reserve(rest.size());
  for (size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' && i + 2 < rest.size() && hexValue(rest[i + 1]) >= 0 && hexValue(rest[i + 2]) >= 0) {
      decoded.push_back(static_cast<char>(hexValue(rest[i + 1]) * 16 + hexValue(rest[i + 2])));
      i += 2;
    } else {
      decoded.push_back(rest[i]);
    }
  }
  // "/C:/Users/..." -> "C:/Users/..."
  if (decoded.size() >= 3 && decoded[0] == '/' && std::isalpha(static_cast<unsigned char>(decoded[1])) &&
      decoded[2] == ':') {
    decoded.erase(0, 1);
  }
  return std::filesystem::path(std::u8string(decoded.begin(), decoded.end()));
}

std::string pathToUri(const std::filesystem::path& path) {
  std::string generic = displayPath(path);
  // "C:/Users/..." needs the third slash; "/home/..." brings its own.
  std::string uri = generic.empty() || generic.front() != '/' ? "file:///" : "file://";
  static const char* hex = "0123456789ABCDEF";
  for (unsigned char c : generic) {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') {
      uri.push_back(static_cast<char>(c));
    } else {
      uri.push_back('%');
      uri.push_back(hex[c >> 4]);
      uri.push_back(hex[c & 0xF]);
    }
  }
  return uri;
}

std::string displayPath(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  if (ec) absolute = path;
  std::u8string generic = absolute.lexically_normal().generic_u8string();
  return std::string(generic.begin(), generic.end());
}

std::string pathKey(const std::filesystem::path& path) {
#ifdef _WIN32
  return toLower(displayPath(path));  // file names differing only in case are the same file
#else
  return displayPath(path);
#endif
}

}  // namespace sls
