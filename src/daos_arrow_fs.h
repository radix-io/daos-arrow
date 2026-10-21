#ifndef PARQUET_S3_DAOS_ARROW_FS_H
#define PARQUET_S3_DAOS_ARROW_FS_H

/*
 * arrow::fs::FileSystem backend for DAOS, via libdfs. See PLAN.md for design
 * rationale. Modeled on arrow/filesystem/localfs.{h,cc}.
 *
 * URI form: daos://pool/container/relpath, resolved via duns_resolve_path().
 * One DaosFileSystem instance == one mounted (pool, container) pair.
 */

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <arrow/filesystem/filesystem.h>
#include <arrow/io/interfaces.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include <daos_obj_class.h>  // daos_oclass_id_t

#include "daos-cpp-dfs.h"

namespace arrow::util {
class Uri;
}  // namespace arrow::util

namespace daos_arrow {

/// \brief Layout knobs for newly-created files, consulted by DAOS only at
/// object-create time (dfs_open with O_CREAT); both are no-ops when opening
/// an existing file. No other Arrow filesystem backend has an equivalent --
/// see PLAN.md optimization #3.
struct DaosFileSystemOptions {
  /// DAOS object class for new files (0 = DAOS default, MAX_RW). Prefer
  /// `oclass_hint` below over setting this directly unless you already know
  /// the numeric daos_oclass_id_t you want.
  daos_oclass_id_t file_oclass = 0;

  /// Chunk size (bytes) for new files' backing array object (0 = DAOS
  /// default, 1 MiB -- also Parquet's default data page size, so files
  /// written in large pages may benefit from a larger chunk size here).
  daos_size_t chunk_size = 0;

  /// A dfs_suggest_oclass() hint such as "file:single" or "file:max" (see
  /// daos_fs.h). When non-empty, resolved once in DaosFileSystem::Make() and
  /// used in place of `file_oclass`; leave empty to use `file_oclass` as-is
  /// (which defaults to 0, i.e. no override).
  std::string oclass_hint;

  static DaosFileSystemOptions Defaults() { return DaosFileSystemOptions(); }

  bool Equals(const DaosFileSystemOptions& other) const {
    return file_oclass == other.file_oclass && chunk_size == other.chunk_size &&
           oclass_hint == other.oclass_hint;
  }

  /// Parse "?oclass=file:single&chunk_size=4194304" query parameters off a
  /// daos:// URI. Unrecognized query keys are ignored (forward-compatible
  /// with URIs built for a later version of this backend).
  static arrow::Result<DaosFileSystemOptions> FromUri(const arrow::util::Uri& uri);
};

/// \brief A FileSystem implementation accessing a DAOS POSIX container via libdfs.
class DaosFileSystem : public arrow::fs::FileSystem {
 public:
  ~DaosFileSystem() override = default;

  /// Connect to pool/cont (dfs_connect) and return a usable filesystem.
  /// This is the primary constructor path; prefer it over the public
  /// constructor so connection failures surface as a Status rather than
  /// a half-constructed object.
  static arrow::Result<std::shared_ptr<DaosFileSystem>> Make(
      const std::string& pool, const std::string& cont,
      const DaosFileSystemOptions& options = DaosFileSystemOptions::Defaults(),
      const arrow::io::IOContext& io_context = arrow::io::default_io_context());

  std::string type_name() const override { return "daos"; }

  bool Equals(const arrow::fs::FileSystem& other) const override;

  arrow::Result<arrow::fs::FileInfo> GetFileInfo(const std::string& path) override;
  arrow::Result<std::vector<arrow::fs::FileInfo>> GetFileInfo(
      const arrow::fs::FileSelector& select) override;

  arrow::Status CreateDir(const std::string& path, bool recursive) override;

  arrow::Status DeleteDir(const std::string& path) override;
  arrow::Status DeleteDirContents(const std::string& path, bool missing_dir_ok) override;
  arrow::Status DeleteRootDirContents() override;

  arrow::Status DeleteFile(const std::string& path) override;

  arrow::Status Move(const std::string& src, const std::string& dest) override;

  arrow::Status CopyFile(const std::string& src, const std::string& dest) override;

  arrow::Result<std::shared_ptr<arrow::io::InputStream>> OpenInputStream(
      const std::string& path) override;
  arrow::Result<std::shared_ptr<arrow::io::RandomAccessFile>> OpenInputFile(
      const std::string& path) override;
  arrow::Result<std::shared_ptr<arrow::io::OutputStream>> OpenOutputStream(
      const std::string& path,
      const std::shared_ptr<const arrow::KeyValueMetadata>& metadata) override;
  arrow::Result<std::shared_ptr<arrow::io::OutputStream>> OpenAppendStream(
      const std::string& path,
      const std::shared_ptr<const arrow::KeyValueMetadata>& metadata) override;

  using arrow::fs::FileSystem::GetFileInfo;
  using arrow::fs::FileSystem::CreateDir;
  using arrow::fs::FileSystem::DeleteDirContents;
  using arrow::fs::FileSystem::OpenOutputStream;
  using arrow::fs::FileSystem::OpenAppendStream;

  const std::string& pool() const { return pool_; }
  const std::string& container() const { return container_; }
  const DaosFileSystemOptions& options() const { return options_; }

 protected:
  explicit DaosFileSystem(std::string pool, std::string container,
                          const DaosFileSystemOptions& options,
                          const arrow::io::IOContext& io_context);

 private:
  // Look up `path`'s parent directory with the given access flags (see the
  // big comment on daos::fs::Mount::lookup for why the flags matter), and
  // return (parent Obj, leaf name). The root directory is represented by a
  // null parent Obj + empty leaf name; callers must check for that.
  arrow::Result<std::pair<daos::fs::Obj, std::string>> LookupParent(
      const std::string& path, int flags);

  // dfs_lookup() + stat -> FileInfo, for GetFileInfo(path) and the
  // recursive selector walk.
  arrow::Result<arrow::fs::FileInfo> StatPath(const std::string& path);

  // Shared by GetFileInfo(FileSelector) recursion.
  arrow::Status CollectFileInfo(const std::string& dir_path, int32_t max_recursion,
                                std::vector<arrow::fs::FileInfo>* out);

  std::string pool_;
  std::string container_;
  DaosFileSystemOptions options_;
  daos::fs::Mount mount_;

  // dfs_init()/dfs_fini() must bracket all dfs_connect()/dfs_disconnect()
  // calls in the process; shared across every DaosFileSystem instance.
  static std::shared_ptr<daos::fs::Library> GetLibrary();
};

}  // namespace daos_arrow

#endif  // PARQUET_S3_DAOS_ARROW_FS_H
