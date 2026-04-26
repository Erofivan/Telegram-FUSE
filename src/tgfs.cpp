#include "tgfs.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iostream>

static bool IsInternalDirSentinel(const std::string& name) {
    return name.find(".tgfs_dir_") != std::string::npos;
}

static TgFsContext* Ctx() {
    return static_cast<TgFsContext*>(fuse_get_context()->private_data);
}

ParsedPath ParsedPath::Parse(const char* path) {
    ParsedPath pp;
    pp.is_root_dir = false;
    std::string p(path ? path : "/");
    if (p == "/") {
        pp.is_root_dir = true;
        return pp;
    }
    if (!p.empty() && p[0] == '/') p = p.substr(1);
    auto slash = p.find('/');
    if (slash == std::string::npos) {
        pp.name = p;
        return pp;
    }
    pp.tag = p.substr(0, slash);
    pp.name = p.substr(slash + 1);
    return pp;
}

// ---- guarded wrappers: never let an exception escape into FUSE ----
#define TGFS_GUARD_BEGIN try {
#define TGFS_GUARD_END(default_err)                                            \
    }                                                                          \
    catch (const std::exception& e) {                                          \
        std::cerr << "[tgfs] exception in " << __func__ << ": " << e.what()    \
                  << "\n";                                                     \
        return (default_err);                                                  \
    }                                                                          \
    catch (...) {                                                              \
        std::cerr << "[tgfs] unknown exception in " << __func__ << "\n";       \
        return (default_err);                                                  \
    }

static int TgGetattrImpl(const char* path, struct stat* st) {
    memset(st, 0, sizeof(*st));
    auto& ctx = *Ctx();
    ParsedPath pp = ParsedPath::Parse(path);
    if (pp.is_root_dir) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_atime = st->st_mtime = st->st_ctime = time(nullptr);
        return 0;
    }
    if (pp.tag.empty()) {
        auto dirs = ctx.tg->ListDirs();
        if (std::find(dirs.begin(), dirs.end(), pp.name) != dirs.end()) {
            st->st_mode = S_IFDIR | 0755;
            st->st_nlink = 2;
            st->st_atime = st->st_mtime = st->st_ctime = time(nullptr);
            return 0;
        }
        auto tf = ctx.tg->FindFile(pp.name, "");
        if (!tf) {
            size_t psz = 0;
            if (!ctx.HasPendingFile(pp.name, "", &psz)) return -ENOENT;
            st->st_mode = S_IFREG | 0644;
            st->st_nlink = 1;
            st->st_size = static_cast<off_t>(psz);
            st->st_atime = st->st_mtime = st->st_ctime = time(nullptr);
            return 0;
        }
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        st->st_size = tf->size;
        st->st_mtime = tf->date;
        st->st_ctime = tf->date;
        st->st_atime = tf->date;
        return 0;
    }
    if (pp.name.empty()) {
        auto dirs = ctx.tg->ListDirs();
        if (std::find(dirs.begin(), dirs.end(), pp.tag) != dirs.end()) {
            st->st_mode = S_IFDIR | 0755;
            st->st_nlink = 2;
            st->st_atime = st->st_mtime = st->st_ctime = time(nullptr);
            return 0;
        }
        return -ENOENT;
    }
    auto tf = ctx.tg->FindFile(pp.name, pp.tag);
    if (!tf) {
        size_t psz = 0;
        if (!ctx.HasPendingFile(pp.name, pp.tag, &psz)) return -ENOENT;
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        st->st_size = static_cast<off_t>(psz);
        st->st_atime = st->st_mtime = st->st_ctime = time(nullptr);
        return 0;
    }
    st->st_mode = S_IFREG | 0644;
    st->st_nlink = 1;
    st->st_size = tf->size;
    st->st_mtime = tf->date;
    st->st_ctime = tf->date;
    st->st_atime = tf->date;
    return 0;
}

static int TgGetattr(const char* path, struct stat* st,
                     struct fuse_file_info* /*fi*/) {
    TGFS_GUARD_BEGIN
    return TgGetattrImpl(path, st);
    TGFS_GUARD_END(-EIO)
}

static int TgReaddirImpl(const char* path, void* buf, fuse_fill_dir_t filler) {
    auto& ctx = *Ctx();
    ParsedPath pp = ParsedPath::Parse(path);
    filler(buf, ".", nullptr, 0, FUSE_FILL_DIR_PLUS);
    filler(buf, "..", nullptr, 0, FUSE_FILL_DIR_PLUS);
    if (pp.is_root_dir || (pp.tag.empty() && pp.name.empty())) {
        for (auto& dir : ctx.tg->ListDirs()) {
            struct stat st{};
            st.st_mode = S_IFDIR | 0755;
            st.st_nlink = 2;
            filler(buf, dir.c_str(), &st, 0, FUSE_FILL_DIR_PLUS);
        }
        for (auto& tf : ctx.tg->ListFiles("")) {
            if (IsInternalDirSentinel(tf.name)) continue;
            struct stat st{};
            st.st_mode = S_IFREG | 0644;
            st.st_size = tf.size;
            st.st_mtime = tf.date;
            st.st_ctime = tf.date;
            filler(buf, tf.name.c_str(), &st, 0, FUSE_FILL_DIR_PLUS);
        }
        for (const auto& p : ctx.ListPendingByTag("")) {
            struct stat st{};
            st.st_mode = S_IFREG | 0644;
            st.st_size = static_cast<off_t>(p.second);
            filler(buf, p.first.c_str(), &st, 0, FUSE_FILL_DIR_PLUS);
        }
        return 0;
    }
    std::string tag = pp.tag.empty() ? pp.name : pp.tag;
    for (auto& tf : ctx.tg->ListFiles(tag)) {
        if (IsInternalDirSentinel(tf.name)) continue;
        struct stat st{};
        st.st_mode = S_IFREG | 0644;
        st.st_size = tf.size;
        st.st_mtime = tf.date;
        st.st_ctime = tf.date;
        filler(buf, tf.name.c_str(), &st, 0, FUSE_FILL_DIR_PLUS);
    }
    for (const auto& p : ctx.ListPendingByTag(tag)) {
        struct stat st{};
        st.st_mode = S_IFREG | 0644;
        st.st_size = static_cast<off_t>(p.second);
        filler(buf, p.first.c_str(), &st, 0, FUSE_FILL_DIR_PLUS);
    }
    return 0;
}

static int TgReaddir(const char* path, void* buf, fuse_fill_dir_t filler,
                     off_t /*offset*/, struct fuse_file_info* /*fi*/,
                     enum fuse_readdir_flags /*flags*/) {
    TGFS_GUARD_BEGIN
    return TgReaddirImpl(path, buf, filler);
    TGFS_GUARD_END(-EIO)
}

static int TgMkdir(const char* path, mode_t /*mode*/) {
    TGFS_GUARD_BEGIN
    auto& ctx = *Ctx();
    ParsedPath pp = ParsedPath::Parse(path);
    std::string tag = pp.tag.empty() ? pp.name : pp.tag;
    if (tag.empty()) return -EINVAL;
    auto dirs = ctx.tg->ListDirs();
    if (std::find(dirs.begin(), dirs.end(), tag) != dirs.end()) {
        return -EEXIST;
    }
    std::string sentinel_name = ".tgfs_dir_" + tag;
    std::vector<uint8_t> empty_data(1, 0);
    auto tf = ctx.tg->UploadFile(sentinel_name, empty_data, tag);
    if (!tf) return -EIO;
    return 0;
    TGFS_GUARD_END(-EIO)
}

static int TgRmdir(const char* path) {
    TGFS_GUARD_BEGIN
    auto& ctx = *Ctx();
    ParsedPath pp = ParsedPath::Parse(path);
    std::string tag = pp.tag.empty() ? pp.name : pp.tag;
    if (tag.empty()) return -EINVAL;
    auto files = ctx.tg->ListFiles(tag);
    if (files.empty()) return -ENOENT;
    for (auto& tf : files) {
        ctx.tg->DeleteMessage(tf.message_id);
    }
    ctx.tg->InvalidateCache();
    return 0;
    TGFS_GUARD_END(-EIO)
}

static int TgOpen(const char* path, struct fuse_file_info* fi) {
    TGFS_GUARD_BEGIN
    auto& ctx = *Ctx();
    ParsedPath pp = ParsedPath::Parse(path);
    auto tf = ctx.tg->FindFile(pp.name, pp.tag);
    if (!tf) {
        auto pending = ctx.GetPendingFile(pp.name, pp.tag);
        if (!pending) return -ENOENT;
        FileHandle fh;
        fh.message_id = 0;
        fh.name = pp.name;
        fh.tag = pp.tag;
        fh.buf = *pending;
        fh.is_new = true;
        fh.dirty = false;
        fi->fh = ctx.AllocFh(std::move(fh));
        return 0;
    }
    FileHandle fh;
    fh.message_id = tf->message_id;
    fh.name = pp.name;
    fh.tag = pp.tag;
    fh.is_new = false;
    fh.dirty = false;
    fi->fh = ctx.AllocFh(std::move(fh));
    return 0;
    TGFS_GUARD_END(-EIO)
}

static int TgRead(const char* /*path*/, char* buf, size_t size, off_t offset,
                  struct fuse_file_info* fi) {
    TGFS_GUARD_BEGIN
    auto& ctx = *Ctx();
    FileHandle* fh = ctx.GetFh(fi->fh);
    if (!fh) return -EBADF;
    if (fh->buf.empty()) {
        auto tf = ctx.tg->FindFile(fh->name, fh->tag);
        if (!tf) return -ENOENT;
        const auto* cached = ctx.cache.Get(tf->message_id);
        if (cached) {
            fh->buf = *cached;
        } else {
            fh->buf = ctx.tg->DownloadFile(tf->file_id);
            ctx.cache.Put(tf->message_id, fh->buf);
        }
    }
    if (offset < 0 || static_cast<size_t>(offset) >= fh->buf.size()) return 0;
    size_t avail = fh->buf.size() - static_cast<size_t>(offset);
    size_t to_copy = std::min(size, avail);
    memcpy(buf, fh->buf.data() + offset, to_copy);
    return static_cast<int>(to_copy);
    TGFS_GUARD_END(-EIO)
}

static int TgCreate(const char* path, mode_t /*mode*/,
                    struct fuse_file_info* fi) {
    TGFS_GUARD_BEGIN
    auto& ctx = *Ctx();
    ParsedPath pp = ParsedPath::Parse(path);
    if (pp.name.empty()) return -EINVAL;
    auto existing = ctx.tg->FindFile(pp.name, pp.tag);
    if (existing && (fi->flags & O_EXCL)) return -EEXIST;
    if (!existing) {
        if (ctx.HasPendingFile(pp.name, pp.tag)) {
            if (fi->flags & O_EXCL) return -EEXIST;
        } else {
            ctx.PutPendingFile(pp.name, pp.tag, {});
        }
    }
    FileHandle fh;
    fh.message_id = existing ? existing->message_id : 0;
    fh.name = pp.name;
    fh.tag = pp.tag;
    fh.is_new = !existing.has_value();
    fh.dirty = false;
    if (fh.is_new) {
        auto pending = ctx.GetPendingFile(pp.name, pp.tag);
        if (pending) fh.buf = *pending;
    }
    fi->fh = ctx.AllocFh(std::move(fh));
    return 0;
    TGFS_GUARD_END(-EIO)
}

static int TgMknod(const char* path, mode_t mode, dev_t /*rdev*/) {
    TGFS_GUARD_BEGIN
    if (!S_ISREG(mode)) return -EPERM;
    auto& ctx = *Ctx();
    ParsedPath pp = ParsedPath::Parse(path);
    if (pp.name.empty()) return -EINVAL;
    auto exists = ctx.tg->FindFile(pp.name, pp.tag);
    if (exists) return -EEXIST;
    if (ctx.HasPendingFile(pp.name, pp.tag)) return -EEXIST;
    ctx.PutPendingFile(pp.name, pp.tag, {});
    return 0;
    TGFS_GUARD_END(-EIO)
}

static int TgWrite(const char* /*path*/, const char* buf, size_t size,
                   off_t offset, struct fuse_file_info* fi) {
    TGFS_GUARD_BEGIN
    auto& ctx = *Ctx();
    FileHandle* fh = ctx.GetFh(fi->fh);
    if (!fh) return -EBADF;
    size_t needed = static_cast<size_t>(offset) + size;
    if (fh->buf.size() < needed) fh->buf.resize(needed, 0);
    memcpy(fh->buf.data() + offset, buf, size);
    fh->dirty = true;
    if (fh->is_new) ctx.PutPendingFile(fh->name, fh->tag, fh->buf);
    return static_cast<int>(size);
    TGFS_GUARD_END(-EIO)
}

static int TgRelease(const char* /*path*/, struct fuse_file_info* fi) {
    TGFS_GUARD_BEGIN
    auto& ctx = *Ctx();
    FileHandle* fh = ctx.GetFh(fi->fh);
    if (!fh) return 0;
    if (fh->dirty) {
        int64_t old_id = fh->message_id;
        if (!fh->buf.empty()) {
            auto new_tf = ctx.tg->UploadFile(fh->name, fh->buf, fh->tag);
            if (!new_tf) {
                std::cerr << "[tgfs] upload failed for " << fh->name << "\n";
                ctx.ReleaseFh(fi->fh);
                return -EIO;
            }
            if (old_id) ctx.cache.Evict(old_id);
            ctx.cache.Put(new_tf->message_id, fh->buf);
            if (old_id) ctx.tg->DeleteMessage(old_id);
            ctx.RemovePendingFile(fh->name, fh->tag);
        } else {
            if (old_id) {
                ctx.cache.Evict(old_id);
                ctx.tg->DeleteMessage(old_id);
            }
            ctx.PutPendingFile(fh->name, fh->tag, fh->buf);
        }
    } else if (fh->is_new) {
        ctx.PutPendingFile(fh->name, fh->tag, fh->buf);
    }
    ctx.ReleaseFh(fi->fh);
    return 0;
    TGFS_GUARD_END(-EIO)
}

static int TgTruncate(const char* path, off_t size,
                      struct fuse_file_info* fi) {
    TGFS_GUARD_BEGIN
    auto& ctx = *Ctx();
    if (fi && fi->fh) {
        FileHandle* fh = ctx.GetFh(fi->fh);
        if (!fh) return -EBADF;
        fh->buf.resize(static_cast<size_t>(size), 0);
        fh->dirty = true;
        return 0;
    }
    ParsedPath pp = ParsedPath::Parse(path);
    auto tf = ctx.tg->FindFile(pp.name, pp.tag);
    if (!tf) {
        if (!ctx.HasPendingFile(pp.name, pp.tag) && size != 0) return -ENOENT;
        std::vector<uint8_t> pdata(static_cast<size_t>(size), 0);
        ctx.PutPendingFile(pp.name, pp.tag, pdata);
        if (size > 0) {
            auto new_tf = ctx.tg->UploadFile(pp.name, pdata, pp.tag);
            if (!new_tf) return -EIO;
            ctx.RemovePendingFile(pp.name, pp.tag);
        }
        return 0;
    }
    std::vector<uint8_t> data;
    if (tf && size > 0) {
        data = ctx.tg->DownloadFile(tf->file_id);
        data.resize(static_cast<size_t>(size), 0);
    } else {
        data.resize(static_cast<size_t>(size), 0);
    }
    int64_t old_id = tf ? tf->message_id : 0;
    auto new_tf = ctx.tg->UploadFile(pp.name, data, pp.tag);
    if (!new_tf) return -EIO;
    if (old_id) {
        ctx.tg->DeleteMessage(old_id);
        ctx.cache.Evict(old_id);
    }
    return 0;
    TGFS_GUARD_END(-EIO)
}

static int TgUnlink(const char* path) {
    TGFS_GUARD_BEGIN
    auto& ctx = *Ctx();
    ParsedPath pp = ParsedPath::Parse(path);
    auto tf = ctx.tg->FindFile(pp.name, pp.tag);
    if (!tf) {
        if (ctx.HasPendingFile(pp.name, pp.tag)) {
            ctx.RemovePendingFile(pp.name, pp.tag);
            return 0;
        }
        return -ENOENT;
    }
    ctx.cache.Evict(tf->message_id);
    bool ok = ctx.tg->DeleteMessage(tf->message_id);
    return ok ? 0 : -EIO;
    TGFS_GUARD_END(-EIO)
}

static int TgRename(const char* from, const char* to,
                    unsigned int /*flags*/) {
    TGFS_GUARD_BEGIN
    auto& ctx = *Ctx();
    ParsedPath src = ParsedPath::Parse(from);
    ParsedPath dst = ParsedPath::Parse(to);
    auto pending_src = ctx.GetPendingFile(src.name, src.tag);
    if (pending_src) {
        ctx.PutPendingFile(dst.name, dst.tag, *pending_src);
        ctx.RemovePendingFile(src.name, src.tag);
        return 0;
    }
    auto tf = ctx.tg->FindFile(src.name, src.tag);
    if (!tf) return -ENOENT;
    auto data = ctx.tg->DownloadFile(tf->file_id);
    if (data.empty()) return -EIO;
    int64_t old_id = tf->message_id;
    ctx.cache.Evict(old_id);
    auto new_tf = ctx.tg->UploadFile(dst.name, data, dst.tag);
    if (!new_tf) return -EIO;
    ctx.tg->DeleteMessage(old_id);
    return 0;
    TGFS_GUARD_END(-EIO)
}

static int TgChmod(const char*, mode_t, struct fuse_file_info*) { return 0; }
static int TgChown(const char*, uid_t, gid_t, struct fuse_file_info*) {
    return 0;
}
static int TgUtimens(const char*, const struct timespec[2],
                     struct fuse_file_info*) {
    return 0;
}
static int TgSymlink(const char*, const char*) { return -EPERM; }
static int TgLink(const char*, const char*) { return -EPERM; }
static int TgReadlink(const char*, char*, size_t) { return -EINVAL; }

static int TgStatfs(const char*, struct statvfs* stbuf) {
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize = 4096;
    stbuf->f_frsize = 4096;
    stbuf->f_blocks = (1ULL << 30);
    stbuf->f_bfree = (1ULL << 30);
    stbuf->f_bavail = (1ULL << 30);
    stbuf->f_files = (1ULL << 20);
    stbuf->f_ffree = (1ULL << 20);
    return 0;
}

struct fuse_operations TgfsOps() {
    struct fuse_operations ops{};
    ops.getattr = TgGetattr;
    ops.readdir = TgReaddir;
    ops.mknod = TgMknod;
    ops.mkdir = TgMkdir;
    ops.rmdir = TgRmdir;
    ops.open = TgOpen;
    ops.read = TgRead;
    ops.create = TgCreate;
    ops.write = TgWrite;
    ops.release = TgRelease;
    ops.truncate = TgTruncate;
    ops.unlink = TgUnlink;
    ops.rename = TgRename;
    ops.chmod = TgChmod;
    ops.chown = TgChown;
    ops.utimens = TgUtimens;
    ops.symlink = TgSymlink;
    ops.link = TgLink;
    ops.readlink = TgReadlink;
    ops.statfs = TgStatfs;
    return ops;
}
