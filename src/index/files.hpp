#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace sls {
// .shader, .compute, .hlsl, .cginc and .hlslinc: the files references, renames and the index look at. GLSL is left
// to GLSL servers.
bool isShaderSource(const std::filesystem::path &path);
// Folders no one edits shaders in: what Unity, IDEs and version control generate, and Unity's hidden folders - a
// name starting with '.' or ending with '~' - which it does not import.
bool isSkippedFolder(const std::filesystem::path &name);
// A file's contents, read again only when its modification time changes. The same version of a file is always the
// same string, so comparing the pointers tells whether a file changed.
std::shared_ptr<const std::string> readCached(const std::filesystem::path &path);
// Whether `path` is in one of `roots`, outside the folders isSkippedFolder() names.
bool underRoots(const std::filesystem::path &path, const std::vector<std::filesystem::path> &roots);
} // namespace sls
