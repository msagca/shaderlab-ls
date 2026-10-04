#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include "analysis/analysis.h"
namespace sls {
// What the workspace's shader files declare, include and name, kept current as they change, so that questions about
// the whole workspace - which files name X, which files include Y, what is called Z - are lookups rather than
// searches. It is built in the background and answers from what it has.
//
// It is laid out for those questions rather than as an object per file. Names are interned to 32-bit ids once; a
// file's declarations are plain rows of ids and numbers; and the relations - name to files, file to includes, file to
// includers - are arrays of ids. A lookup reads one array and the rows it points at, and nothing else; updating a file
// replaces its rows and patches the arrays it was in.
class Index {
public:
  using FileId = uint32_t;
  using NameId = uint32_t;
  static constexpr uint8_t kShaderName = 0xFF; // a DeclRow for a .shader's Shader "Name"
  // A declaration, with where it is in both of the position encodings a client may ask for.
  struct DeclRow {
    NameId name;
    NameId container; // its struct or cbuffer, or kNoName
    uint8_t kind; // a DeclKind, or kShaderName
    uint32_t line;
    uint32_t column8, column16;
    uint32_t length8, length16;
  };
  static constexpr NameId kNoName = UINT32_MAX;
  struct Symbol {
    std::string name;
    std::string container;
    uint8_t kind;
    std::filesystem::path path;
    uint32_t line, column, length;
  };
  Index();
  ~Index();
  Index(const Index &) = delete;
  Index &operator=(const Index &) = delete;
  // Where the index is kept between sessions, one file per set of folders; empty to keep it in memory only. Takes
  // effect at the next setRoots().
  void setCacheDirectory(std::filesystem::path directory);
  // The user's cache folder for it: %LOCALAPPDATA% on Windows, $XDG_CACHE_HOME or ~/.cache elsewhere.
  static std::filesystem::path defaultCacheDirectory();
  // Told what the index did that a user may want to know of: that it was loaded from its cache, or couldn't be saved.
  void onLog(std::function<void(const std::string &)> log);
  // Called on the indexing thread whenever it has caught up with every change it was told of.
  void onIdle(std::function<void()> callback);
  // The folders to index and the Unity editor to resolve includes with; the folders are crawled again.
  void setRoots(std::vector<std::filesystem::path> roots, std::filesystem::path editorOverride);
  // Adds a folder to the ones indexed, if it is not in one already.
  void addRoot(const std::filesystem::path &root);
  // A file changed or appeared on disk, or went away.
  void fileChanged(const std::filesystem::path &path);
  void fileDeleted(const std::filesystem::path &path);
  // An open document stands for its file, as it is in the editor, until it is closed and the file is read again.
  void documentChanged(std::shared_ptr<const Analysis> analysis);
  void documentClosed(const std::filesystem::path &path);
  // Crawls the folders again for changes, for a client that does not report them. At most every few seconds.
  void refresh();
  // Waits until everything queued is indexed, at most `timeout`. True once the first crawl is.
  bool waitReady(std::chrono::milliseconds timeout) const;
  // Bumped by every change, for caches of what is derived from the index.
  uint64_t generation() const;
  // The workspace's shader files.
  std::vector<std::filesystem::path> workspaceFiles() const;
  // The workspace's shader files whose code has the identifier `name` in it.
  std::vector<std::filesystem::path> filesNaming(std::string_view name) const;
  // The files that include `path`, directly or not, nearest first: workspace and package files alike.
  std::vector<std::filesystem::path> includers(const std::filesystem::path &path, size_t limit = 400) const;
  // The files that include `path` directly.
  std::vector<std::filesystem::path> directIncluders(const std::filesystem::path &path) const;
  // The files that `from` include, directly or not, nearest first.
  std::vector<std::filesystem::path> included(const std::vector<std::filesystem::path> &from, size_t limit = 2000) const;
  // The declarations of the workspace's files whose names have `query`'s letters in order, case aside.
  std::vector<Symbol> symbols(std::string_view query, Encoding encoding, size_t limit) const;
private:
  struct Job {
    std::filesystem::path path;
    std::shared_ptr<const Analysis> analysis; // an open document's; null to read the file
    bool deleted = false;
  };
  // What indexing one file finds, worked out before the index is locked.
  struct Facts {
    std::string key;
    std::filesystem::path path;
    std::filesystem::file_time_type time;
    std::vector<std::string> names; // every identifier in its code, unique
    struct Decl {
      std::string name, container;
      uint8_t kind;
      uint32_t line, column8, column16, length8, length16;
    };
    std::vector<Decl> decls; // with the shader's name among them
    std::vector<std::filesystem::path> includes; // resolved
  };
  void run();
  void crawl();
  void enqueue(Job job);
  std::optional<Facts> read(const Job &job) const;
  void store(Facts facts, bool workspace, bool open);
  void remove(const std::string &key);
  FileId fileFor(const std::string &key, const std::filesystem::path &path);
  NameId intern(std::string_view name);
  bool inWorkspace(const std::filesystem::path &path) const;
  // The saved index: read before the first crawl, which then only looks for what changed since; written when the
  // index has caught up and something changed, at most every few seconds, and when it is destroyed.
  void load();
  void save();
  // The tables. Guarded by tables_: read under a shared lock, written by the indexing thread under a unique one.
  mutable std::shared_mutex tables_;
  std::deque<std::string> nameStore_; // stable storage for the views below
  std::vector<std::string_view> nameText_;
  std::unordered_map<std::string_view, NameId> nameIds_;
  std::vector<std::vector<FileId>> namePostings_; // name -> files whose code names it
  std::unordered_map<std::string, FileId> fileIds_;
  std::vector<std::filesystem::path> filePath_;
  std::vector<std::filesystem::file_time_type> fileTime_;
  std::vector<uint8_t> fileFlags_;
  std::vector<std::vector<NameId>> fileNames_; // sorted
  std::vector<std::vector<DeclRow>> fileDecls_;
  std::vector<std::vector<FileId>> fileIncludes_;
  std::vector<std::vector<FileId>> fileIncluders_;
  uint64_t generation_ = 0;
  // The work queue and settings. Guarded by queueMutex_.
  mutable std::mutex queueMutex_;
  mutable std::condition_variable wake_;
  mutable std::condition_variable idle_;
  std::deque<Job> queue_;
  bool crawlPending_ = false;
  bool ready_ = false;
  bool busy_ = false;
  bool stopping_ = false;
  std::chrono::steady_clock::time_point lastCrawl_{};
  std::vector<std::filesystem::path> roots_;
  std::filesystem::path editorOverride_;
  std::filesystem::path cacheDirectory_;
  std::filesystem::path cacheFile_;
  bool loadPending_ = false;
  std::function<void(const std::string &)> log_;
  std::function<void()> idleCallback_;
  // Written by the indexing thread only.
  uint64_t savedGeneration_ = 0;
  std::chrono::steady_clock::time_point lastSave_{};
  std::thread thread_;
};
} // namespace sls
