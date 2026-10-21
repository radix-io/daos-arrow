#ifndef DAOS_CPP_DFS_H
#define DAOS_CPP_DFS_H

/*
 * DFS (POSIX-like) layer companion to daos-cpp.h.
 *
 * Deliberately NOT exception-based, unlike the object-API classes in
 * daos-cpp.h: every operation returns a plain errno code so it can be mapped
 * straight onto arrow::Status / arrow::Result without a try/catch in each of
 * the ~15 FileSystem overrides.  The value this header adds is RAII for the
 * two handle types and sgl-setup boilerplate removal, not error translation.
 *
 * NB: the whole dfs_ and duns_ surface returns *positive errno*, never -DER_ codes.
 * Use strerror(), not d_errstr(); d_errstr() would silently decode ENOENT(2)
 * as a GURT code.  (daos_fs.h: 68 "errno code on failure", 0 occurrences of DER_.)
 */

#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <fcntl.h>

#include <daos.h>
#include <daos_fs.h>
#include <daos_uns.h>

namespace daos {
namespace fs {

/** strerror() for the DFS layer; see header comment on why not d_errstr(). */
inline std::string strerr(int rc) { return std::strerror(rc); }

/* ---------- RAII handle wrappers ---------- */

struct ObjDeleter {
	void operator()(dfs_obj_t *obj) const noexcept { (void)dfs_release(obj); }
};
/** Owning dfs_obj_t*.  Release is automatic on every early return. */
using Obj = std::unique_ptr<dfs_obj_t, ObjDeleter>;

/** Scoped dfs_init()/dfs_fini().  One per process. */
class Library {
    public:
	Library() : rc_(dfs_init()) {}
	~Library() { if (rc_ == 0) (void)dfs_fini(); }
	int rc() const { return rc_; }

	Library(const Library&) = delete;
	Library& operator=(const Library&) = delete;
    private:
	int rc_;
};

/** Resolved daos://pool/cont/relpath, with duns_destroy_attr() handled. */
class Uri {
    public:
	Uri() : attr_{}, rc_(EINVAL) {}

	int resolve(const std::string& uri) {
		reset();
		attr_ = duns_attr_t{};
		rc_ = duns_resolve_path(uri.c_str(), &attr_);
		if (rc_ == 0) valid_ = true;
		return rc_;
	}
	~Uri() { reset(); }

	const char *pool() const { return attr_.da_pool; }
	const char *cont() const { return attr_.da_cont; }
	/** Relative path inside the container; "/" when the URI names the root. */
	std::string rel_path() const {
		return (attr_.da_rel_path && *attr_.da_rel_path) ? attr_.da_rel_path : "/";
	}

	Uri(const Uri&) = delete;
	Uri& operator=(const Uri&) = delete;
    private:
	void reset() { if (valid_) { duns_destroy_attr(&attr_); valid_ = false; } }
	duns_attr_t attr_;
	int rc_;
	bool valid_ = false;
};

/* ---------- the mount ---------- */

/**
 * One dfs_connect()ed container.  Matches one arrow::fs::DaosFileSystem
 * instance (pool+container fixed at construction).
 */
class Mount {
    public:
	Mount() = default;
	~Mount() { close(); }

	int connect(const char *pool, const char *cont, int flags = O_RDWR,
		    const char *sys = nullptr) {
		close();
		return dfs_connect(pool, sys, cont, flags, nullptr, &dfs_);
	}
	void close() {
		if (dfs_) { (void)dfs_disconnect(dfs_); dfs_ = nullptr; }
	}

	dfs_t *get() const { return dfs_; }
	explicit operator bool() const { return dfs_ != nullptr; }

	/* --- path ops; all return errno --- */

	/**
	 * A NOTE about DAOS object permissions: `flags` here is not just an
	 * access check on the looked-up object -- it sets the DAOS object open
	 * mode (DAOS_OO_RO vs DAOS_OO_RW, see get_daos_obj_mode() in
	 * dfs_internal.h) that server-side enforcement checks on *every
	 * subsequent mutation through this handle*. Concretely: if you
	 * `lookup(dir, O_RDONLY)` and then `open(dir.get(), "f",
	 * O_CREAT|O_RDWR)` a child under it, the create fails with EPERM
	 * (DER_NO_PERM, logged server-side as "object opc N is opened with
	 * mode 0x2") -- *not* because the file couldn't be created, but
	 * because the *parent directory handle* was opened read-only. Any
	 * parent object you intend to create/remove/rename children under must
	 * be looked up (or opened) O_RDWR, even though the parent object
	 * itself isn't being written to directly.
	 */
	int lookup(const std::string& path, int flags, Obj *out,
		   struct stat *stbuf = nullptr) const {
		dfs_obj_t *raw = nullptr;
		int rc = dfs_lookup(dfs_, path.c_str(), flags, &raw, nullptr, stbuf);
		if (rc == 0) out->reset(raw);
		return rc;
	}

	/**
	 * Resolve a single name relative to an already-open parent, skipping
	 * the per-component path walk dfs_lookup() does internally. Same
	 * O_RDWR-on-parent rule as lookup() above applies to `flags` here --
	 * see the comment on lookup().
	 */
	int lookup_rel(dfs_obj_t *parent, const char *name, int flags, Obj *out,
		       struct stat *stbuf = nullptr) const {
		dfs_obj_t *raw = nullptr;
		int rc = dfs_lookup_rel(dfs_, parent, name, flags, &raw, nullptr, stbuf);
		if (rc == 0) out->reset(raw);
		return rc;
	}

	int open(dfs_obj_t *parent, const char *name, mode_t mode, int flags,
		 Obj *out) const {
		dfs_obj_t *raw = nullptr;
		int rc = dfs_open(dfs_, parent, name, mode, flags,
				  0 /*default oclass*/, 0 /*default chunk*/,
				  nullptr /*not a symlink*/, &raw);
		if (rc == 0) out->reset(raw);
		return rc;
	}

	int mkdir(dfs_obj_t *parent, const char *name, mode_t mode = 0755) const {
		return dfs_mkdir(dfs_, parent, name, mode, 0);
	}
	int remove(dfs_obj_t *parent, const char *name, bool force) const {
		return dfs_remove(dfs_, parent, name, force, nullptr);
	}
	int move(dfs_obj_t *parent, const char *name,
		 dfs_obj_t *new_parent, const char *new_name) const {
		return dfs_move(dfs_, parent, name, new_parent, new_name, nullptr);
	}
	int stat(dfs_obj_t *parent, const char *name, struct stat *stbuf) const {
		return dfs_stat(dfs_, parent, name, stbuf);
	}
	int ostat(dfs_obj_t *obj, struct stat *stbuf) const {
		return dfs_ostat(dfs_, obj, stbuf);
	}
	int get_size(dfs_obj_t *obj, daos_size_t *size) const {
		return dfs_get_size(dfs_, obj, size);
	}

	/* --- I/O: hides the d_iov_t/d_sg_list_t setup --- */

	int read_at(dfs_obj_t *obj, void *buf, size_t nbytes, daos_off_t off,
		    daos_size_t *nread) const {
		d_iov_t iov;
		d_sg_list_t sgl;
		d_iov_set(&iov, buf, nbytes);
		sgl.sg_nr = 1;
		sgl.sg_nr_out = 0;
		sgl.sg_iovs = &iov;
		return dfs_read(dfs_, obj, &sgl, off, nread, nullptr);
	}

	int write_at(dfs_obj_t *obj, const void *buf, size_t nbytes,
		     daos_off_t off) const {
		d_iov_t iov;
		d_sg_list_t sgl;
		/* dfs_write does not modify the buffer; d_iov_set takes void*. */
		d_iov_set(&iov, const_cast<void *>(buf), nbytes);
		sgl.sg_nr = 1;
		sgl.sg_nr_out = 0;
		sgl.sg_iovs = &iov;
		return dfs_write(dfs_, obj, &sgl, off, nullptr);
	}

	Mount(const Mount&) = delete;
	Mount& operator=(const Mount&) = delete;
    private:
	dfs_t *dfs_ = nullptr;
};

/* ---------- directory iteration ---------- */

/**
 * Wraps the dfs_readdir() anchor loop.  Calls fn(const dirent&) for each
 * entry; returns errno (0 on full enumeration).  This is the piece
 * GetFileInfo(FileSelector) needs, and the one most prone to leaking the
 * parent object on an error path -- hence Obj above.
 */
template <typename Fn>
int readdir_all(const Mount& m, dfs_obj_t *dir, Fn&& fn, uint32_t batch = 64) {
	daos_anchor_t anchor = {};
	std::vector<struct dirent> ents(batch);
	while (!daos_anchor_is_eof(&anchor)) {
		uint32_t nr = batch;
		int rc = dfs_readdir(m.get(), dir, &anchor, &nr, ents.data());
		if (rc != 0) return rc;
		for (uint32_t i = 0; i < nr; i++) fn(ents[i]);
	}
	return 0;
}

/**
 * Like readdir_all(), but fetches struct stat for every entry in the same
 * RPC (dfs_readdirplus() reuses the already-open parent handle internally
 * instead of re-walking each child's path component-by-component the way a
 * separate dfs_lookup() per entry would). Calls fn(const dirent&, const
 * stat&) per entry; returns errno (0 on full enumeration).
 */
template <typename Fn>
int readdirplus_all(const Mount& m, dfs_obj_t *dir, Fn&& fn, uint32_t batch = 64) {
	daos_anchor_t anchor = {};
	std::vector<struct dirent> ents(batch);
	std::vector<struct stat> stats(batch);
	while (!daos_anchor_is_eof(&anchor)) {
		uint32_t nr = batch;
		int rc = dfs_readdirplus(m.get(), dir, &anchor, &nr, ents.data(), stats.data());
		if (rc != 0) return rc;
		for (uint32_t i = 0; i < nr; i++) fn(ents[i], stats[i]);
	}
	return 0;
}

} // namespace fs
} // namespace daos

#endif /* DAOS_CPP_DFS_H */
