#include "daos_arrow_fs.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <vector>

#include <arrow/buffer.h>
#include <arrow/filesystem/path_util.h>
#include <arrow/util/checked_cast.h>
#include <arrow/util/uri.h>

namespace daos_arrow {

namespace fsint = arrow::fs::internal;

namespace {

arrow::Status DfsError(int rc, const std::string& context) {
  return arrow::Status::IOError(context, ": ", std::strerror(rc));
}

// dfs_lookup/dfs_open require an absolute ("/"-rooted) path; Arrow's
// FileSystem paths are relative (no leading slash). Normalize + reject
// anything that looks like an actual daos:// URI slipping in here (callers
// should go through arrow's FileSystemFromUri for that).
arrow::Result<std::string> ToDfsPath(const std::string& path) {
  if (fsint::IsLikelyUri(path)) {
    return arrow::Status::Invalid("Expected a daos filesystem path, got a URI: '", path,
                                  "'");
  }
  if (path.empty() || path == "/") {
    return std::string("/");
  }
  return "/" + std::string(fsint::RemoveLeadingSlash(path));
}

arrow::fs::FileType ModeToFileType(mode_t mode) {
  if (S_ISDIR(mode)) return arrow::fs::FileType::Directory;
  if (S_ISREG(mode)) return arrow::fs::FileType::File;
  return arrow::fs::FileType::Unknown;
}

arrow::fs::TimePoint StatToTimePoint(const struct stat& st) {
  auto ns = std::chrono::seconds(st.st_mtim.tv_sec) +
            std::chrono::nanoseconds(st.st_mtim.tv_nsec);
  return arrow::fs::TimePoint(std::chrono::duration_cast<arrow::fs::TimePoint::duration>(ns));
}

}  // namespace

// ---- construction / lifecycle ----

std::shared_ptr<daos::fs::Library> DaosFileSystem::GetLibrary() {
  static std::shared_ptr<daos::fs::Library> lib = std::make_shared<daos::fs::Library>();
  return lib;
}

DaosFileSystem::DaosFileSystem(std::string pool, std::string container,
                               const arrow::io::IOContext& io_context)
    : arrow::fs::FileSystem(io_context), pool_(std::move(pool)), container_(std::move(container)) {}

arrow::Result<std::shared_ptr<DaosFileSystem>> DaosFileSystem::Make(
    const std::string& pool, const std::string& cont,
    const arrow::io::IOContext& io_context) {
  auto lib = GetLibrary();
  if (lib->rc() != 0) {
    return DfsError(lib->rc(), "dfs_init");
  }
  // std::shared_ptr<DaosFileSystem> can't use std::make_shared with a
  // protected constructor; this local subclass trick is the usual
  // workaround (same one used for other private-constructor FileSystems
  // upstream, e.g. S3FileSystem::Make).
  struct MakeableDaosFileSystem : public DaosFileSystem {
    MakeableDaosFileSystem(std::string p, std::string c, const arrow::io::IOContext& ctx)
        : DaosFileSystem(std::move(p), std::move(c), ctx) {}
  };
  auto fs = std::make_shared<MakeableDaosFileSystem>(pool, cont, io_context);
  int rc = fs->mount_.connect(pool.c_str(), cont.c_str(), O_RDWR);
  if (rc != 0) {
    return DfsError(rc, "dfs_connect(pool=" + pool + ", cont=" + cont + ")");
  }
  return fs;
}

bool DaosFileSystem::Equals(const arrow::fs::FileSystem& other) const {
  if (other.type_name() != type_name()) return false;
  const auto& o = ::arrow::internal::checked_cast<const DaosFileSystem&>(other);
  return pool_ == o.pool_ && container_ == o.container_;
}

// ---- path resolution helpers ----

arrow::Result<std::pair<daos::fs::Obj, std::string>> DaosFileSystem::LookupParent(
    const std::string& path, int flags) {
  ARROW_ASSIGN_OR_RAISE(auto dfs_path, ToDfsPath(path));
  if (dfs_path == "/") {
    // Root has no parent; caller must special-case this (null Obj + empty name).
    return std::make_pair(daos::fs::Obj(), std::string());
  }
  auto slash = dfs_path.find_last_of('/');
  std::string parent_path = (slash == 0) ? "/" : dfs_path.substr(0, slash);
  std::string leaf = dfs_path.substr(slash + 1);

  daos::fs::Obj parent;
  int rc = mount_.lookup(parent_path, flags, &parent);
  if (rc != 0) {
    return DfsError(rc, "lookup parent '" + parent_path + "' of '" + path + "'");
  }
  return std::make_pair(std::move(parent), leaf);
}

arrow::Result<arrow::fs::FileInfo> DaosFileSystem::StatPath(const std::string& path) {
  ARROW_ASSIGN_OR_RAISE(auto dfs_path, ToDfsPath(path));
  arrow::fs::FileInfo info(path);

  struct stat st;
  daos::fs::Obj obj;
  int rc = mount_.lookup(dfs_path, O_RDONLY, &obj, &st);
  if (rc == ENOENT) {
    info.set_type(arrow::fs::FileType::NotFound);
    return info;
  }
  if (rc != 0) {
    return DfsError(rc, "stat '" + path + "'");
  }
  info.set_type(ModeToFileType(st.st_mode));
  info.set_mtime(StatToTimePoint(st));
  if (info.type() == arrow::fs::FileType::File) {
    info.set_size(st.st_size);
  }
  return info;
}

arrow::Status DaosFileSystem::CollectFileInfo(const std::string& dir_path,
                                              int32_t max_recursion,
                                              std::vector<arrow::fs::FileInfo>* out) {
  ARROW_ASSIGN_OR_RAISE(auto dfs_path, ToDfsPath(dir_path));
  daos::fs::Obj dir;
  // O_RDONLY is correct here: listing (dfs_readdir) is not a mutation of the
  // directory, unlike CreateDir/DeleteFile/Move below which must look their
  // parent up O_RDWR. See the big comment on daos::fs::Mount::lookup.
  int rc = mount_.lookup(dfs_path, O_RDONLY, &dir);
  if (rc != 0) {
    return DfsError(rc, "opendir '" + dir_path + "'");
  }

  std::vector<arrow::fs::FileInfo> children;
  rc = daos::fs::readdir_all(mount_, dir.get(), [&](const struct dirent& de) {
    std::string name(de.d_name);
    std::string child_path =
        dir_path.empty() ? name : fsint::ConcatAbstractPath(dir_path, name);
    children.push_back(arrow::fs::FileInfo(child_path));
  });
  if (rc != 0) {
    return DfsError(rc, "readdir '" + dir_path + "'");
  }

  for (auto& child : children) {
    // dfs_readdir only gives us d_name; stat each entry individually to get
    // type/size/mtime. (A readdirplus-based version could fetch stat for
    // free per entry -- left as a possible follow-up, not required for the
    // demo; see PLAN.md.)
    ARROW_ASSIGN_OR_RAISE(auto info, StatPath(child.path()));
    out->push_back(info);
    if (info.type() == arrow::fs::FileType::Directory && max_recursion > 0) {
      RETURN_NOT_OK(CollectFileInfo(info.path(), max_recursion - 1, out));
    }
  }
  return arrow::Status::OK();
}

// ---- FileSystem interface ----

arrow::Result<arrow::fs::FileInfo> DaosFileSystem::GetFileInfo(const std::string& path) {
  return StatPath(path);
}

arrow::Result<std::vector<arrow::fs::FileInfo>> DaosFileSystem::GetFileInfo(
    const arrow::fs::FileSelector& select) {
  ARROW_ASSIGN_OR_RAISE(auto base_info, StatPath(select.base_dir));
  if (base_info.type() == arrow::fs::FileType::NotFound) {
    if (select.allow_not_found) return std::vector<arrow::fs::FileInfo>();
    return arrow::Status::IOError("GetFileInfo: base_dir '", select.base_dir,
                                  "' does not exist");
  }
  std::vector<arrow::fs::FileInfo> infos;
  RETURN_NOT_OK(CollectFileInfo(select.base_dir,
                                select.recursive ? select.max_recursion : 0, &infos));
  return infos;
}

arrow::Status DaosFileSystem::CreateDir(const std::string& path, bool recursive) {
  ARROW_ASSIGN_OR_RAISE(auto dfs_path, ToDfsPath(path));
  if (dfs_path == "/") return arrow::Status::OK();

  if (!recursive) {
    ARROW_ASSIGN_OR_RAISE(auto parent_leaf, LookupParent(path, O_RDWR));
    auto& [parent, leaf] = parent_leaf;
    int rc = mount_.mkdir(parent.get(), leaf.c_str());
    if (rc != 0 && rc != EEXIST) return DfsError(rc, "mkdir '" + path + "'");
    return arrow::Status::OK();
  }

  // Recursive: walk from root, creating each missing component. Each
  // component's parent must be looked up O_RDWR to be allowed to mkdir a
  // child under it (see Mount::lookup comment) -- this is the same trap
  // this whole plan execution ran into on the first real test.
  auto parts = fsint::SplitAbstractPath(dfs_path);
  daos::fs::Obj parent;  // null == root
  std::string built;
  for (const auto& part : parts) {
    if (part.empty()) continue;
    int rc = mount_.mkdir(parent.get(), part.c_str());
    if (rc != 0 && rc != EEXIST) {
      return DfsError(rc, "mkdir '" + built + "/" + part + "' (recursive)");
    }
    built += "/" + part;
    daos::fs::Obj next;
    rc = mount_.lookup(built, O_RDWR, &next);
    if (rc != 0) return DfsError(rc, "lookup '" + built + "' after mkdir");
    parent = std::move(next);
  }
  return arrow::Status::OK();
}

arrow::Status DaosFileSystem::DeleteDir(const std::string& path) {
  ARROW_ASSIGN_OR_RAISE(auto parent_leaf, LookupParent(path, O_RDWR));
  auto& [parent, leaf] = parent_leaf;
  if (leaf.empty()) {
    return arrow::Status::NotImplemented("DeleteDir: cannot remove the root of a DAOS container");
  }
  int rc = mount_.remove(parent.get(), leaf.c_str(), /*force=*/true);
  if (rc != 0) return DfsError(rc, "rmdir '" + path + "'");
  return arrow::Status::OK();
}

arrow::Status DaosFileSystem::DeleteDirContents(const std::string& path,
                                                bool missing_dir_ok) {
  ARROW_ASSIGN_OR_RAISE(auto dfs_path, ToDfsPath(path));
  daos::fs::Obj dir;
  int rc = mount_.lookup(dfs_path, O_RDWR, &dir);
  if (rc == ENOENT && missing_dir_ok) return arrow::Status::OK();
  if (rc != 0) return DfsError(rc, "opendir '" + path + "' for DeleteDirContents");

  std::vector<std::string> names;
  rc = daos::fs::readdir_all(mount_, dir.get(),
                             [&](const struct dirent& de) { names.push_back(de.d_name); });
  if (rc != 0) return DfsError(rc, "readdir '" + path + "' for DeleteDirContents");

  for (const auto& name : names) {
    rc = mount_.remove(dir.get(), name.c_str(), /*force=*/true);
    if (rc != 0) {
      return DfsError(rc, "remove '" + name + "' under '" + path + "'");
    }
  }
  return arrow::Status::OK();
}

arrow::Status DaosFileSystem::DeleteRootDirContents() {
  return DeleteDirContents("/", /*missing_dir_ok=*/false);
}

arrow::Status DaosFileSystem::DeleteFile(const std::string& path) {
  ARROW_ASSIGN_OR_RAISE(auto parent_leaf, LookupParent(path, O_RDWR));
  auto& [parent, leaf] = parent_leaf;
  if (leaf.empty()) {
    return arrow::Status::Invalid("DeleteFile: '", path, "' is the root directory");
  }
  int rc = mount_.remove(parent.get(), leaf.c_str(), /*force=*/false);
  if (rc != 0) return DfsError(rc, "unlink '" + path + "'");
  return arrow::Status::OK();
}

arrow::Status DaosFileSystem::Move(const std::string& src, const std::string& dest) {
  ARROW_ASSIGN_OR_RAISE(auto src_parent_leaf, LookupParent(src, O_RDWR));
  ARROW_ASSIGN_OR_RAISE(auto dest_parent_leaf, LookupParent(dest, O_RDWR));
  auto& [src_parent, src_leaf] = src_parent_leaf;
  auto& [dest_parent, dest_leaf] = dest_parent_leaf;
  if (src_leaf.empty() || dest_leaf.empty()) {
    return arrow::Status::Invalid("Move: cannot move the root directory");
  }
  int rc = mount_.move(src_parent.get(), src_leaf.c_str(), dest_parent.get(),
                       dest_leaf.c_str());
  if (rc != 0) return DfsError(rc, "move '" + src + "' -> '" + dest + "'");
  return arrow::Status::OK();
}

arrow::Status DaosFileSystem::CopyFile(const std::string& src, const std::string& dest) {
  if (src == dest) return arrow::Status::OK();
  // No server-side copy in DAOS (see PLAN.md); read+write via the stream
  // classes below, same approach LocalFileSystem uses on non-Windows.
  ARROW_ASSIGN_OR_RAISE(auto is, OpenInputStream(src));
  ARROW_ASSIGN_OR_RAISE(auto os, OpenOutputStream(dest));
  constexpr int64_t kChunkSize = 1024 * 1024;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(auto buf, is->Read(kChunkSize));
    if (buf->size() == 0) break;
    RETURN_NOT_OK(os->Write(buf));
  }
  RETURN_NOT_OK(os->Close());
  return is->Close();
}

// ---- streams ----

namespace {

class DaosRandomAccessFile : public arrow::io::RandomAccessFile {
 public:
  DaosRandomAccessFile(daos::fs::Mount* mount, daos::fs::Obj obj, std::string path)
      : mount_(mount), obj_(std::move(obj)), path_(std::move(path)) {}

  ~DaosRandomAccessFile() override {
    if (!closed_) { (void)Close(); }
  }

  arrow::Status Close() override {
    obj_.reset();
    closed_ = true;
    return arrow::Status::OK();
  }
  bool closed() const override { return closed_; }
  arrow::Result<int64_t> Tell() const override { return pos_; }

  arrow::Status Seek(int64_t position) override {
    if (position < 0) return arrow::Status::Invalid("Seek to negative position");
    pos_ = position;
    return arrow::Status::OK();
  }

  arrow::Result<int64_t> GetSize() override {
    daos_size_t sz;
    int rc = mount_->get_size(obj_.get(), &sz);
    if (rc != 0) return DfsError(rc, "get_size '" + path_ + "'");
    return static_cast<int64_t>(sz);
  }

  arrow::Result<int64_t> ReadAt(int64_t position, int64_t nbytes, void* out) override {
    if (nbytes == 0) return 0;
    daos_size_t nread = 0;
    int rc = mount_->read_at(obj_.get(), out, static_cast<size_t>(nbytes), position, &nread);
    if (rc != 0) return DfsError(rc, "read '" + path_ + "'");
    return static_cast<int64_t>(nread);
  }

  arrow::Result<std::shared_ptr<arrow::Buffer>> ReadAt(int64_t position,
                                                        int64_t nbytes) override {
    ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateResizableBuffer(nbytes));
    ARROW_ASSIGN_OR_RAISE(int64_t bytes_read,
                          ReadAt(position, nbytes, buf->mutable_data()));
    RETURN_NOT_OK(buf->Resize(bytes_read, /*shrink_to_fit=*/true));
    buf->ZeroPadding();
    return std::shared_ptr<arrow::Buffer>(std::move(buf));
  }

  arrow::Result<int64_t> Read(int64_t nbytes, void* out) override {
    ARROW_ASSIGN_OR_RAISE(int64_t n, ReadAt(pos_, nbytes, out));
    pos_ += n;
    return n;
  }

  arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t nbytes) override {
    ARROW_ASSIGN_OR_RAISE(auto buf, ReadAt(pos_, nbytes));
    pos_ += buf->size();
    return buf;
  }

 private:
  daos::fs::Mount* mount_;
  daos::fs::Obj obj_;
  std::string path_;
  int64_t pos_ = 0;
  bool closed_ = false;
};

class DaosOutputStream : public arrow::io::OutputStream {
 public:
  DaosOutputStream(daos::fs::Mount* mount, daos::fs::Obj obj, std::string path,
                   int64_t start_pos)
      : mount_(mount), obj_(std::move(obj)), path_(std::move(path)), pos_(start_pos) {}

  ~DaosOutputStream() override {
    if (!closed_) { (void)Close(); }
  }

  arrow::Status Close() override {
    obj_.reset();
    closed_ = true;
    return arrow::Status::OK();
  }
  bool closed() const override { return closed_; }
  arrow::Result<int64_t> Tell() const override { return pos_; }

  arrow::Status Write(const void* data, int64_t nbytes) override {
    if (nbytes == 0) return arrow::Status::OK();
    int rc = mount_->write_at(obj_.get(), data, static_cast<size_t>(nbytes), pos_);
    if (rc != 0) return DfsError(rc, "write '" + path_ + "'");
    pos_ += nbytes;
    return arrow::Status::OK();
  }

  // Flush() left as the Writable default no-op: DAOS has no single-object
  // fsync-equivalent needed for this demo (see PLAN.md known limitations).

 private:
  daos::fs::Mount* mount_;
  daos::fs::Obj obj_;
  std::string path_;
  int64_t pos_;
  bool closed_ = false;
};

}  // namespace

arrow::Result<std::shared_ptr<arrow::io::InputStream>> DaosFileSystem::OpenInputStream(
    const std::string& path) {
  return OpenInputFile(path);
}

arrow::Result<std::shared_ptr<arrow::io::RandomAccessFile>> DaosFileSystem::OpenInputFile(
    const std::string& path) {
  ARROW_ASSIGN_OR_RAISE(auto dfs_path, ToDfsPath(path));
  daos::fs::Obj obj;
  struct stat st;
  int rc = mount_.lookup(dfs_path, O_RDONLY, &obj, &st);
  if (rc != 0) return DfsError(rc, "open '" + path + "' for read");
  if (!S_ISREG(st.st_mode)) {
    return arrow::Status::IOError("'", path, "' is not a regular file");
  }
  return std::make_shared<DaosRandomAccessFile>(&mount_, std::move(obj), path);
}

arrow::Result<std::shared_ptr<arrow::io::OutputStream>> DaosFileSystem::OpenOutputStream(
    const std::string& path, const std::shared_ptr<const arrow::KeyValueMetadata>&) {
  ARROW_ASSIGN_OR_RAISE(auto parent_leaf, LookupParent(path, O_RDWR));
  auto& [parent, leaf] = parent_leaf;
  if (leaf.empty()) {
    return arrow::Status::Invalid("OpenOutputStream: '", path, "' is the root directory");
  }
  daos::fs::Obj obj;
  int rc = mount_.open(parent.get(), leaf.c_str(), S_IFREG | 0644,
                       O_CREAT | O_TRUNC | O_RDWR, &obj);
  if (rc != 0) return DfsError(rc, "open '" + path + "' for write");
  return std::make_shared<DaosOutputStream>(&mount_, std::move(obj), path, /*start_pos=*/0);
}

arrow::Result<std::shared_ptr<arrow::io::OutputStream>> DaosFileSystem::OpenAppendStream(
    const std::string& path, const std::shared_ptr<const arrow::KeyValueMetadata>&) {
  ARROW_ASSIGN_OR_RAISE(auto parent_leaf, LookupParent(path, O_RDWR));
  auto& [parent, leaf] = parent_leaf;
  if (leaf.empty()) {
    return arrow::Status::Invalid("OpenAppendStream: '", path, "' is the root directory");
  }
  daos::fs::Obj obj;
  int rc = mount_.open(parent.get(), leaf.c_str(), S_IFREG | 0644, O_CREAT | O_RDWR, &obj);
  if (rc != 0) return DfsError(rc, "open '" + path + "' for append");
  daos_size_t size = 0;
  rc = mount_.get_size(obj.get(), &size);
  if (rc != 0) return DfsError(rc, "get_size '" + path + "' for append");
  return std::make_shared<DaosOutputStream>(&mount_, std::move(obj), path,
                                            static_cast<int64_t>(size));
}

// ---- URI registration ----

namespace {

arrow::Result<std::shared_ptr<arrow::fs::FileSystem>> DaosFileSystemFactory(
    const arrow::util::Uri& uri, const arrow::io::IOContext& io_context,
    std::string* out_path) {
  // daos://pool/container/relpath -- host is the pool, first path segment
  // the container, the rest the in-container path. We use Arrow's own
  // Uri parsing (not duns_resolve_path) for this scheme 
  std::string pool = uri.host();
  std::string rest = uri.path();  // leading '/', e.g. "/container/a/b"
  rest = fsint::RemoveLeadingSlash(rest).data();
  auto slash = rest.find('/');
  std::string container = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  std::string relpath = (slash == std::string::npos) ? "" : rest.substr(slash + 1);
  if (pool.empty() || container.empty()) {
    return arrow::Status::Invalid("Invalid daos:// URI (need daos://pool/container[/path]): ",
                                  uri.ToString());
  }
  if (out_path != nullptr) *out_path = relpath;
  return DaosFileSystem::Make(pool, container, io_context);
}

arrow::fs::FileSystemRegistrar kDaosFileSystemModule{
    "daos", arrow::fs::FileSystemFactory{DaosFileSystemFactory, __FILE__, __LINE__}, {}};

}  // namespace

}  // namespace daos_arrow
