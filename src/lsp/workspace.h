#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "analysis/analysis.h"
#include "index/index.h"
namespace sls {
// The files that references and renames reach beyond the document they start in: the shader files in the workspace
// folders, each as the editor has it - an open document as it is in the editor, any other as it is on disk.
class Workspace {
public:
  struct OpenDocument {
    std::string uri; // as the client spelled it, which is how edits to it must be addressed
    std::shared_ptr<const Analysis> analysis;
  };
  Workspace() = default;
  // `open` is keyed by pathKey(). Without an index, the folders are read for every question.
  Workspace(std::vector<std::filesystem::path> roots, std::map<std::string, OpenDocument> open, const Index *index = nullptr);
  // Whether `path` is in a workspace folder, outside the folders Unity and other tools generate (Library, Temp, ...):
  // a file a rename may edit.
  bool contains(const std::filesystem::path &path) const;
  // The shader files of the workspace, open documents included.
  std::vector<std::filesystem::path> files() const;
  // The shader files of the workspace whose text has `word` in it, open documents included.
  std::vector<std::filesystem::path> filesMentioning(std::string_view word) const;
  // The files that include `path`, directly or not, nearest first; nothing without an index to say.
  std::optional<std::vector<std::filesystem::path>> includers(const std::filesystem::path &path) const;
  // The files `from` include, directly or not; nothing without an index to say.
  std::optional<std::vector<std::filesystem::path>> included(const std::vector<std::filesystem::path> &from) const;
  const Index *index() const {
    return index_;
  }
  // The file as the editor has it, or null when it can't be read. A file that is not open is analyzed once for each
  // time it changes on disk.
  std::shared_ptr<const Analysis> analysis(const std::filesystem::path &path) const;
  std::string uri(const std::filesystem::path &path) const;
private:
  std::vector<std::filesystem::path> roots_;
  std::map<std::string, OpenDocument> open_;
  const Index *index_ = nullptr;
};
} // namespace sls
