#include <nan.h>

#define FUSE_USE_VERSION 29

#include <fuse.h>
#include <fuse_opt.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>

#include "abstractions.h"

using namespace v8;

enum bindings_ops_t {
  OP_INIT = 0,
  OP_ERROR,
  OP_ACCESS,
  OP_STATFS,
  OP_FGETATTR,
  OP_GETATTR,
  OP_FLUSH,
  OP_FSYNC,
  OP_FSYNCDIR,
  OP_READDIR,
  OP_TRUNCATE,
  OP_FTRUNCATE,
  OP_UTIMENS,
  OP_READLINK,
  OP_CHOWN,
  OP_CHMOD,
  OP_MKNOD,
  OP_SETXATTR,
  OP_GETXATTR,
  OP_OPEN,
  OP_OPENDIR,
  OP_READ,
  OP_WRITE,
  OP_RELEASE,
  OP_RELEASEDIR,
  OP_CREATE,
  OP_UNLINK,
  OP_RENAME,
  OP_LINK,
  OP_SYMLINK,
  OP_MKDIR,
  OP_RMDIR,
  OP_DESTROY
};

static Nan::Persistent<Function> buffer_constructor;
static Nan::Callback *callback_constructor;
static struct stat empty_stat;

struct bindings_t {
  int index;
  int gc;

  // fuse context
  int context_uid;
  int context_gid;
  int context_pid;

  // fuse data
  char mnt[1024];
  char mntopts[1024];
  abstr_thread_t thread;
  bindings_sem_t semaphore;
  uv_async_t async;

  // methods
  Nan::Callback *ops_init;
  Nan::Callback *ops_error;
  Nan::Callback *ops_access;
  Nan::Callback *ops_statfs;
  Nan::Callback *ops_getattr;
  Nan::Callback *ops_fgetattr;
  Nan::Callback *ops_flush;
  Nan::Callback *ops_fsync;
  Nan::Callback *ops_fsyncdir;
  Nan::Callback *ops_readdir;
  Nan::Callback *ops_truncate;
  Nan::Callback *ops_ftruncate;
  Nan::Callback *ops_readlink;
  Nan::Callback *ops_chown;
  Nan::Callback *ops_chmod;
  Nan::Callback *ops_mknod;
  Nan::Callback *ops_setxattr;
  Nan::Callback *ops_getxattr;
  Nan::Callback *ops_open;
  Nan::Callback *ops_opendir;
  Nan::Callback *ops_read;
  Nan::Callback *ops_write;
  Nan::Callback *ops_release;
  Nan::Callback *ops_releasedir;
  Nan::Callback *ops_create;
  Nan::Callback *ops_utimens;
  Nan::Callback *ops_unlink;
  Nan::Callback *ops_rename;
  Nan::Callback *ops_link;
  Nan::Callback *ops_symlink;
  Nan::Callback *ops_mkdir;
  Nan::Callback *ops_rmdir;
  Nan::Callback *ops_destroy;

  Nan::Callback *callback;

  // method data
  bindings_ops_t op;
  fuse_fill_dir_t filler; // used in readdir
  struct fuse_file_info *info;
  char *path;
  char *name;
  FUSE_OFF_T offset;
  FUSE_OFF_T length;
  void *data; // various structs
  int mode;
  int dev;
  int uid;
  int gid;
  int result;
};

static bindings_t *bindings_mounted[1024];
static int bindings_mounted_count = 0;
static bindings_t *bindings_current = NULL;

static bindings_t *bindings_find_mounted (char *path) {
  for (int i = 0; i < bindings_mounted_count; i++) {
    bindings_t *b = bindings_mounted[i];
    if (b != NULL && !b->gc && !strcmp(b->mnt, path)) {
      return b;
    }
  }
  return NULL;
}

static void bindings_fusermount (char *path) {
  fusermount(path);
}

static void bindings_unmount (char *path) {
  mutex_lock(&mutex);
  bindings_t *b = bindings_find_mounted(path);
  if (b != NULL) b->gc = 1;
  bindings_fusermount(path);
  if (b != NULL) thread_join(b->thread);
  mutex_unlock(&mutex);
}


#if (NODE_MODULE_VERSION > NODE_0_10_MODULE_VERSION && NODE_MODULE_VERSION < IOJS_3_0_MODULE_VERSION)
NAN_INLINE v8::Local<v8::Object> bindings_buffer (char *data, size_t length) {
  Local<Object> buf = Nan::New(buffer_constructor)->NewInstance(0, NULL);
  Local<String> k = Nan::New<String>("length").ToLocalChecked();
  Local<Number> v = Nan::New<Number>(length);
  buf->Set(k, v);
  buf->SetIndexedPropertiesToExternalArrayData((char *) data, kExternalUnsignedByteArray, length);
  return buf;
}
#else
void noop (char *data, void *hint) {}
NAN_INLINE v8::Local<v8::Object> bindings_buffer (char *data, size_t length) {
  return NanNewBufferHandle(data, length, noop, NULL);
}
#endif

NAN_INLINE static int bindings_call (bindings_t *b) {
  uv_async_send(&(b->async));
  semaphore_wait(&(b->semaphore));
  return b->result;
}

static bindings_t *bindings_get_context () {
  fuse_context *ctx = fuse_get_context();
  bindings_t *b = (bindings_t *) ctx->private_data;
  b->context_pid = ctx->pid;
  b->context_uid = ctx->uid;
  b->context_gid = ctx->gid;
  return b;
}

static int bindings_mknod (const char *path, mode_t mode, dev_t dev) {
  bindings_t *b = bindings_get_context();

  b->op = OP_MKNOD;
  b->path = (char *) path;
  b->mode = mode;
  b->dev = dev;

  return bindings_call(b);
}

static int bindings_truncate (const char *path, FUSE_OFF_T size) {
  bindings_t *b = bindings_get_context();

  b->op = OP_TRUNCATE;
  b->path = (char *) path;
  b->length = size;

  return bindings_call(b);
}

static int bindings_ftruncate (const char *path, FUSE_OFF_T size, struct fuse_file_info *info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_FTRUNCATE;
  b->path = (char *) path;
  b->length = size;
  b->info = info;

  return bindings_call(b);
}

static int bindings_getattr (const char *path, struct stat *stat) {
  bindings_t *b = bindings_get_context();

  b->op = OP_GETATTR;
  b->path = (char *) path;
  b->data = stat;

  return bindings_call(b);
}

static int bindings_fgetattr (const char *path, struct stat *stat, struct fuse_file_info *info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_FGETATTR;
  b->path = (char *) path;
  b->data = stat;
  b->info = info;

  return bindings_call(b);
}

static int bindings_flush (const char *path, struct fuse_file_info *info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_FLUSH;
  b->path = (char *) path;
  b->info = info;

  return bindings_call(b);
}

static int bindings_fsync (const char *path, int datasync, struct fuse_file_info *info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_FSYNC;
  b->path = (char *) path;
  b->mode = datasync;
  b->info = info;

  return bindings_call(b);
}

static int bindings_fsyncdir (const char *path, int datasync, struct fuse_file_info *info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_FSYNCDIR;
  b->path = (char *) path;
  b->mode = datasync;
  b->info = info;

  return bindings_call(b);
}

static int bindings_readdir (const char *path, void *buf, fuse_fill_dir_t filler, FUSE_OFF_T offset, struct fuse_file_info *info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_READDIR;
  b->path = (char *) path;
  b->data = buf;
  b->filler = filler;

  return bindings_call(b);
}

static int bindings_readlink (const char *path, char *buf, size_t len) {
  bindings_t *b = bindings_get_context();

  b->op = OP_READLINK;
  b->path = (char *) path;
  b->data = (void *) buf;
  b->length = len;

  return bindings_call(b);
}

static int bindings_chown (const char *path, uid_t uid, gid_t gid) {
  bindings_t *b = bindings_get_context();

  b->op = OP_CHOWN;
  b->path = (char *) path;
  b->uid = uid;
  b->gid = gid;

  return bindings_call(b);
}

static int bindings_chmod (const char *path, mode_t mode) {
  bindings_t *b = bindings_get_context();

  b->op = OP_CHMOD;
  b->path = (char *) path;
  b->mode = mode;

  return bindings_call(b);
}

#ifdef __APPLE__
static int bindings_setxattr (const char *path, const char *name, const char *value, size_t size, int flags, uint32_t position) {
  bindings_t *b = bindings_get_context();

  b->op = OP_SETXATTR;
  b->path = (char *) path;
  b->name = (char *) name;
  b->data = (void *) value;
  b->length = size;
  b->offset = position;
  b->mode = flags;

  return bindings_call(b);
}

static int bindings_getxattr (const char *path, const char *name, char *value, size_t size, uint32_t position) {
  bindings_t *b = bindings_get_context();

  b->op = OP_GETXATTR;
  b->path = (char *) path;
  b->name = (char *) name;
  b->data = (void *) value;
  b->length = size;
  b->offset = position;

  return bindings_call(b);
}
#else
static int bindings_setxattr (const char *path, const char *name, const char *value, size_t size, int flags) {
  bindings_t *b = bindings_get_context();

  b->op = OP_SETXATTR;
  b->path = (char *) path;
  b->name = (char *) name;
  b->data = (void *) value;
  b->length = size;
  b->offset = 0;
  b->mode = flags;

  return bindings_call(b);
}

static int bindings_getxattr (const char *path, const char *name, char *value, size_t size) {
  bindings_t *b = bindings_get_context();

  b->op = OP_GETXATTR;
  b->path = (char *) path;
  b->name = (char *) name;
  b->data = (void *) value;
  b->length = size;
  b->offset = 0;

  return bindings_call(b);
}
#endif

static int bindings_statfs (const char *path, struct statvfs *statfs) {
  bindings_t *b = bindings_get_context();

  b->op = OP_STATFS;
  b->path = (char *) path;
  b->data = statfs;

  return bindings_call(b);
}

static int bindings_open (const char *path, struct fuse_file_info *info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_OPEN;
  b->path = (char *) path;
  b->mode = info->flags;
  b->info = info;

  return bindings_call(b);
}

static int bindings_opendir (const char *path, struct fuse_file_info *info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_OPENDIR;
  b->path = (char *) path;
  b->mode = info->flags;
  b->info = info;

  return bindings_call(b);
}

static int bindings_read (const char *path, char *buf, size_t len, FUSE_OFF_T offset, struct fuse_file_info *info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_READ;
  b->path = (char *) path;
  b->data = (void *) buf;
  b->offset = offset;
  b->length = len;
  b->info = info;

  return bindings_call(b);
}

static int bindings_write (const char *path, const char *buf, size_t len, FUSE_OFF_T offset, struct fuse_file_info * info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_WRITE;
  b->path = (char *) path;
  b->data = (void *) buf;
  b->offset = offset;
  b->length = len;
  b->info = info;

  return bindings_call(b);
}

static int bindings_release (const char *path, struct fuse_file_info *info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_RELEASE;
  b->path = (char *) path;
  b->info = info;

  return bindings_call(b);
}

static int bindings_releasedir (const char *path, struct fuse_file_info *info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_RELEASEDIR;
  b->path = (char *) path;
  b->info = info;

  return bindings_call(b);
}

static int bindings_access (const char *path, int mode) {
  bindings_t *b = bindings_get_context();

  b->op = OP_ACCESS;
  b->path = (char *) path;
  b->mode = mode;

  return bindings_call(b);
}

static int bindings_create (const char *path, mode_t mode, struct fuse_file_info *info) {
  bindings_t *b = bindings_get_context();

  b->op = OP_CREATE;
  b->path = (char *) path;
  b->mode = mode;
  b->info = info;

  return bindings_call(b);
}

static int bindings_utimens (const char *path, const struct timespec tv[2]) {
  bindings_t *b = bindings_get_context();

  b->op = OP_UTIMENS;
  b->path = (char *) path;
  b->data = (void *) tv;

  return bindings_call(b);
}

static int bindings_unlink (const char *path) {
  bindings_t *b = bindings_get_context();

  b->op = OP_UNLINK;
  b->path = (char *) path;

  return bindings_call(b);
}

static int bindings_rename (const char *src, const char *dest) {
  bindings_t *b = bindings_get_context();

  b->op = OP_RENAME;
  b->path = (char *) src;
  b->data = (void *) dest;

  return bindings_call(b);
}

static int bindings_link (const char *path, const char *dest) {
  bindings_t *b = bindings_get_context();

  b->op = OP_LINK;
  b->path = (char *) path;
  b->data = (void *) dest;

  return bindings_call(b);
}

static int bindings_symlink (const char *path, const char *dest) {
  bindings_t *b = bindings_get_context();

  b->op = OP_SYMLINK;
  b->path = (char *) path;
  b->data = (void *) dest;

  return bindings_call(b);
}

static int bindings_mkdir (const char *path, mode_t mode) {
  bindings_t *b = bindings_get_context();

  b->op = OP_MKDIR;
  b->path = (char *) path;
  b->mode = mode;

  return bindings_call(b);
}

static int bindings_rmdir (const char *path) {
  bindings_t *b = bindings_get_context();

  b->op = OP_RMDIR;
  b->path = (char *) path;

  return bindings_call(b);
}

static void* bindings_init (struct fuse_conn_info *conn) {
  bindings_t *b = bindings_get_context();

  b->op = OP_INIT;

  bindings_call(b);
  return b;
}

static void bindings_destroy (void *data) {
  bindings_t *b = bindings_get_context();

  b->op = OP_DESTROY;

  bindings_call(b);
}

static void bindings_free (bindings_t *b) {
  if (b->ops_access != NULL) delete b->ops_access;
  if (b->ops_truncate != NULL) delete b->ops_truncate;
  if (b->ops_ftruncate != NULL) delete b->ops_ftruncate;
  if (b->ops_getattr != NULL) delete b->ops_getattr;
  if (b->ops_fgetattr != NULL) delete b->ops_fgetattr;
  if (b->ops_flush != NULL) delete b->ops_flush;
  if (b->ops_fsync != NULL) delete b->ops_fsync;
  if (b->ops_fsyncdir != NULL) delete b->ops_fsyncdir;
  if (b->ops_readdir != NULL) delete b->ops_readdir;
  if (b->ops_readlink != NULL) delete b->ops_readlink;
  if (b->ops_chown != NULL) delete b->ops_chown;
  if (b->ops_chmod != NULL) delete b->ops_chmod;
  if (b->ops_mknod != NULL) delete b->ops_mknod;
  if (b->ops_setxattr != NULL) delete b->ops_setxattr;
  if (b->ops_getxattr != NULL) delete b->ops_getxattr;
  if (b->ops_statfs != NULL) delete b->ops_statfs;
  if (b->ops_open != NULL) delete b->ops_open;
  if (b->ops_opendir != NULL) delete b->ops_opendir;
  if (b->ops_read != NULL) delete b->ops_read;
  if (b->ops_write != NULL) delete b->ops_write;
  if (b->ops_release != NULL) delete b->ops_release;
  if (b->ops_releasedir != NULL) delete b->ops_releasedir;
  if (b->ops_create != NULL) delete b->ops_create;
  if (b->ops_utimens != NULL) delete b->ops_utimens;
  if (b->ops_unlink != NULL) delete b->ops_unlink;
  if (b->ops_rename != NULL) delete b->ops_rename;
  if (b->ops_link != NULL) delete b->ops_link;
  if (b->ops_symlink != NULL) delete b->ops_symlink;
  if (b->ops_mkdir != NULL) delete b->ops_mkdir;
  if (b->ops_rmdir != NULL) delete b->ops_rmdir;
  if (b->ops_init != NULL) delete b->ops_init;
  if (b->ops_destroy != NULL) delete b->ops_destroy;
  if (b->callback != NULL) delete b->callback;

  bindings_mounted[b->index] = NULL;
  while (bindings_mounted_count > 0 && bindings_mounted[bindings_mounted_count - 1] == NULL) {
    bindings_mounted_count--;
  }

  free(b);
}

static void bindings_on_close (uv_handle_t *handle) {
  mutex_lock(&mutex);
  bindings_free((bindings_t *) handle->data);
  mutex_unlock(&mutex);
}

static thread_fn_rtn_t bindings_thread (void *data) {
  bindings_t *b = (bindings_t *) data;

  struct fuse_operations ops = { };

  if (b->ops_access != NULL) ops.access = bindings_access;
  if (b->ops_truncate != NULL) ops.truncate = bindings_truncate;
  if (b->ops_ftruncate != NULL) ops.ftruncate = bindings_ftruncate;
  if (b->ops_getattr != NULL) ops.getattr = bindings_getattr;
  if (b->ops_fgetattr != NULL) ops.fgetattr = bindings_fgetattr;
  if (b->ops_flush != NULL) ops.flush = bindings_flush;
  if (b->ops_fsync != NULL) ops.fsync = bindings_fsync;
  if (b->ops_fsyncdir != NULL) ops.fsyncdir = bindings_fsyncdir;
  if (b->ops_readdir != NULL) ops.readdir = bindings_readdir;
  if (b->ops_readlink != NULL) ops.readlink = bindings_readlink;
  if (b->ops_chown != NULL) ops.chown = bindings_chown;
  if (b->ops_chmod != NULL) ops.chmod = bindings_chmod;
  if (b->ops_mknod != NULL) ops.mknod = bindings_mknod;
  if (b->ops_setxattr != NULL) ops.setxattr = bindings_setxattr;
  if (b->ops_getxattr != NULL) ops.getxattr = bindings_getxattr;
  if (b->ops_statfs != NULL) ops.statfs = bindings_statfs;
  if (b->ops_open != NULL) ops.open = bindings_open;
  if (b->ops_opendir != NULL) ops.opendir = bindings_opendir;
  if (b->ops_read != NULL) ops.read = bindings_read;
  if (b->ops_write != NULL) ops.write = bindings_write;
  if (b->ops_release != NULL) ops.release = bindings_release;
  if (b->ops_releasedir != NULL) ops.releasedir = bindings_releasedir;
  if (b->ops_create != NULL) ops.create = bindings_create;
  if (b->ops_utimens != NULL) ops.utimens = bindings_utimens;
  if (b->ops_unlink != NULL) ops.unlink = bindings_unlink;
  if (b->ops_rename != NULL) ops.rename = bindings_rename;
  if (b->ops_link != NULL) ops.link = bindings_link;
  if (b->ops_symlink != NULL) ops.symlink = bindings_symlink;
  if (b->ops_mkdir != NULL) ops.mkdir = bindings_mkdir;
  if (b->ops_rmdir != NULL) ops.rmdir = bindings_rmdir;
  if (b->ops_init != NULL) ops.init = bindings_init;
  if (b->ops_destroy != NULL) ops.destroy = bindings_destroy;

  int argc = !strcmp(b->mntopts, "-o") ? 1 : 2;
  char *argv[] = {
    (char *) "fuse_bindings_dummy",
    (char *) b->mntopts
  };

  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct fuse_chan *ch = fuse_mount(b->mnt, &args);

  if (ch == NULL) {
    b->op = OP_ERROR;
    bindings_call(b);
    uv_close((uv_handle_t*) &(b->async), &bindings_on_close);
    return NULL;
  }

  struct fuse *fuse = fuse_new(ch, &args, &ops, sizeof(struct fuse_operations), b);

  if (fuse == NULL) {
    b->op = OP_ERROR;
    bindings_call(b);
    uv_close((uv_handle_t*) &(b->async), &bindings_on_close);
    return NULL;
  }

  fuse_loop(fuse);

  fuse_unmount(b->mnt, ch);
  fuse_session_remove_chan(ch);
  fuse_destroy(fuse);

  uv_close((uv_handle_t*) &(b->async), &bindings_on_close);

  return 0;
}

#ifndef _WIN32
NAN_INLINE static Local<Date> bindings_get_date (struct timespec *out) {
  int ms = (out->tv_nsec / 1000);
  return Nan::New<Date>(out->tv_sec * 1000 + ms).ToLocalChecked();
}

NAN_INLINE static void bindings_set_date (struct timespec *out, Local<Date> date) {
  double ms = date->NumberValue();
  time_t secs = (time_t)(ms / 1000.0);
  time_t rem = ms - (1000.0 * secs);
  time_t ns = rem * 1000000.0;
  out->tv_sec = secs;
  out->tv_nsec = ns;
}
#else
NAN_INLINE static Local<Date> bindings_get_date (time_t *out) {
  return Nan::New<Date>(*out * 1000.0);
}

NAN_INLINE static void bindings_set_date (time_t *out, Local<Date> date) {
  double ms = date->NumberValue();
  time_t secs = (time_t)(ms / 1000.0);
  *out = secs;
}
#endif

NAN_INLINE static void bindings_set_stat (struct stat *stat, Local<Object> obj) {
  if (obj->Has(Nan::New<String>("dev").ToLocalChecked())) stat->st_dev = obj->Get(Nan::New<String>("dev").ToLocalChecked())->NumberValue();
  if (obj->Has(Nan::New<String>("ino").ToLocalChecked())) stat->st_ino = obj->Get(Nan::New<String>("ino").ToLocalChecked())->NumberValue();
  if (obj->Has(Nan::New<String>("mode").ToLocalChecked())) stat->st_mode = obj->Get(Nan::New<String>("mode").ToLocalChecked())->Uint32Value();
  if (obj->Has(Nan::New<String>("nlink").ToLocalChecked())) stat->st_nlink = obj->Get(Nan::New<String>("nlink").ToLocalChecked())->NumberValue();
  if (obj->Has(Nan::New<String>("uid").ToLocalChecked())) stat->st_uid = obj->Get(Nan::New<String>("uid").ToLocalChecked())->NumberValue();
  if (obj->Has(Nan::New<String>("gid").ToLocalChecked())) stat->st_gid = obj->Get(Nan::New<String>("gid").ToLocalChecked())->NumberValue();
  if (obj->Has(Nan::New<String>("rdev").ToLocalChecked())) stat->st_rdev = obj->Get(Nan::New<String>("rdev").ToLocalChecked())->NumberValue();
  if (obj->Has(Nan::New<String>("size").ToLocalChecked())) stat->st_size = obj->Get(Nan::New<String>("size").ToLocalChecked())->NumberValue();
#ifndef _WIN32
  if (obj->Has(Nan::New<String>("blocks").ToLocalChecked())) stat->st_blocks = obj->Get(Nan::New<String>("blocks").ToLocalChecked())->NumberValue();
  if (obj->Has(Nan::New<String>("blksize").ToLocalChecked())) stat->st_blksize = obj->Get(Nan::New<String>("blksize").ToLocalChecked())->NumberValue();
#endif
#ifdef __APPLE__
  if (obj->Has(Nan::New<String>("mtime").ToLocalChecked())) bindings_set_date(&stat->st_mtimespec, obj->Get(Nan::New("mtime").ToLocalChecked()).As<Date>());
  if (obj->Has(Nan::New<String>("ctime").ToLocalChecked())) bindings_set_date(&stat->st_ctimespec, obj->Get(Nan::New("ctime").ToLocalChecked()).As<Date>());
  if (obj->Has(Nan::New<String>("atime").ToLocalChecked())) bindings_set_date(&stat->st_atimespec, obj->Get(Nan::New("atime").ToLocalChecked()).As<Date>());
#elif defined(_WIN32)
  if (obj->Has(Nan::New<String>("mtime").ToLocalChecked())) bindings_set_date(&stat->st_mtime, obj->Get(Nan::New("mtime").ToLocalChecked()).As<Date>());
  if (obj->Has(Nan::New<String>("ctime").ToLocalChecked())) bindings_set_date(&stat->st_ctime, obj->Get(Nan::New("ctime").ToLocalChecked()).As<Date>());
  if (obj->Has(Nan::New<String>("atime").ToLocalChecked())) bindings_set_date(&stat->st_atime, obj->Get(Nan::New("atime").ToLocalChecked()).As<Date>());
#else
  if (obj->Has(Nan::New<String>("mtime").ToLocalChecked())) bindings_set_date(&stat->st_mtim, obj->Get(Nan::New("mtime").ToLocalChecked()).As<Date>());
  if (obj->Has(Nan::New<String>("ctime").ToLocalChecked())) bindings_set_date(&stat->st_ctim, obj->Get(Nan::New("ctime").ToLocalChecked()).As<Date>());
  if (obj->Has(Nan::New<String>("atime").ToLocalChecked())) bindings_set_date(&stat->st_atim, obj->Get(Nan::New("atime").ToLocalChecked()).As<Date>());
#endif
}

NAN_INLINE static void bindings_set_statfs (struct statvfs *statfs, Local<Object> obj) { // from http://linux.die.net/man/2/stat
  if (obj->Has(Nan::New<String>("bsize").ToLocalChecked())) statfs->f_bsize = obj->Get(Nan::New<String>("bsize").ToLocalChecked())->Uint32Value();
  if (obj->Has(Nan::New<String>("frsize").ToLocalChecked())) statfs->f_frsize = obj->Get(Nan::New<String>("frsize").ToLocalChecked())->Uint32Value();
  if (obj->Has(Nan::New<String>("blocks").ToLocalChecked())) statfs->f_blocks = obj->Get(Nan::New<String>("blocks").ToLocalChecked())->Uint32Value();
  if (obj->Has(Nan::New<String>("bfree").ToLocalChecked())) statfs->f_bfree = obj->Get(Nan::New<String>("bfree").ToLocalChecked())->Uint32Value();
  if (obj->Has(Nan::New<String>("bavail").ToLocalChecked())) statfs->f_bavail = obj->Get(Nan::New<String>("bavail").ToLocalChecked())->Uint32Value();
  if (obj->Has(Nan::New<String>("files").ToLocalChecked())) statfs->f_files = obj->Get(Nan::New<String>("files").ToLocalChecked())->Uint32Value();
  if (obj->Has(Nan::New<String>("ffree").ToLocalChecked())) statfs->f_ffree = obj->Get(Nan::New<String>("ffree").ToLocalChecked())->Uint32Value();
  if (obj->Has(Nan::New<String>("favail").ToLocalChecked())) statfs->f_favail = obj->Get(Nan::New<String>("favail").ToLocalChecked())->Uint32Value();
  if (obj->Has(Nan::New<String>("fsid").ToLocalChecked())) statfs->f_fsid = obj->Get(Nan::New<String>("fsid").ToLocalChecked())->Uint32Value();
  if (obj->Has(Nan::New<String>("flag").ToLocalChecked())) statfs->f_flag = obj->Get(Nan::New<String>("flag").ToLocalChecked())->Uint32Value();
  if (obj->Has(Nan::New<String>("namemax").ToLocalChecked())) statfs->f_namemax = obj->Get(Nan::New<String>("namemax").ToLocalChecked())->Uint32Value();
}

NAN_INLINE static void bindings_set_dirs (bindings_t *b, Local<Array> dirs) {
  Nan::HandleScope scope;
  for (uint32_t i = 0; i < dirs->Length(); i++) {
    Nan::Utf8String dir(dirs->Get(i));
    if (b->filler(b->data, *dir, &empty_stat, 0)) break;
  }
}

NAN_METHOD(OpCallback) {
  bindings_t *b = bindings_mounted[info[0]->Uint32Value()];
  b->result = (info.Length() > 1 && info[1]->IsNumber()) ? info[1]->Uint32Value() : 0;
  bindings_current = NULL;

  if (!b->result) {
    switch (b->op) {
      case OP_STATFS: {
        if (info.Length() > 2 && info[2]->IsObject()) bindings_set_statfs((struct statvfs *) b->data, info[2].As<Object>());
      }
      break;

      case OP_GETATTR:
      case OP_FGETATTR: {
        if (info.Length() > 2 && info[2]->IsObject()) bindings_set_stat((struct stat *) b->data, info[2].As<Object>());
      }
      break;

      case OP_READDIR: {
        if (info.Length() > 2 && info[2]->IsArray()) bindings_set_dirs(b, info[2].As<Array>());
      }
      break;

      case OP_CREATE:
      case OP_OPEN:
      case OP_OPENDIR: {
        if (info.Length() > 2 && info[2]->IsNumber()) {
          b->info->fh = info[2].As<Number>()->Uint32Value();
        }
      }
      break;

      case OP_READLINK: {
        if (info.Length() > 2 && info[2]->IsString()) {
          Nan::Utf8String path(info[2]);
          strcpy((char *) b->data, *path);
        }
      }
      break;

      case OP_INIT:
      case OP_ERROR:
      case OP_ACCESS:
      case OP_FLUSH:
      case OP_FSYNC:
      case OP_FSYNCDIR:
      case OP_TRUNCATE:
      case OP_FTRUNCATE:
      case OP_CHOWN:
      case OP_CHMOD:
      case OP_MKNOD:
      case OP_SETXATTR:
      case OP_GETXATTR:
      case OP_READ:
      case OP_UTIMENS:
      case OP_WRITE:
      case OP_RELEASE:
      case OP_RELEASEDIR:
      case OP_UNLINK:
      case OP_RENAME:
      case OP_LINK:
      case OP_SYMLINK:
      case OP_MKDIR:
      case OP_RMDIR:
      case OP_DESTROY:
      break;
    }
  }

  semaphore_signal(&(b->semaphore));
}

NAN_INLINE static void bindings_call_op (bindings_t *b, Nan::Callback *fn, int argc, Local<Value> *argv) {
  if (fn == NULL) semaphore_signal(&(b->semaphore));
  else fn->Call(argc, argv);
}

static void bindings_dispatch (uv_async_t* handle, int status) {
  Nan::HandleScope scope;

  bindings_t *b = bindings_current = (bindings_t *) handle->data;
  Local<Function> callback = b->callback->GetFunction();
  b->result = -1;

  switch (b->op) {
    case OP_INIT: {
      Local<Value> tmp[] = {callback};
      bindings_call_op(b, b->ops_init, 1, tmp);
    }
    return;

    case OP_ERROR: {
      Local<Value> tmp[] = {callback};
      bindings_call_op(b, b->ops_error, 1, tmp);
    }
    return;

    case OP_STATFS: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), callback};
      bindings_call_op(b, b->ops_statfs, 2, tmp);
    }
    return;

    case OP_FGETATTR: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->info->fh), callback};
      bindings_call_op(b, b->ops_fgetattr, 3, tmp);
    }
    return;

    case OP_GETATTR: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), callback};
      bindings_call_op(b, b->ops_getattr, 2, tmp);
    }
    return;

    case OP_READDIR: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), callback};
      bindings_call_op(b, b->ops_readdir, 2, tmp);
    }
    return;

    case OP_CREATE: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->mode), callback};
      bindings_call_op(b, b->ops_create, 3, tmp);
    }
    return;

    case OP_TRUNCATE: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->length), callback};
      bindings_call_op(b, b->ops_truncate, 3, tmp);
    }
    return;

    case OP_FTRUNCATE: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->info->fh), Nan::New<Number>(b->length), callback};
      bindings_call_op(b, b->ops_ftruncate, 4, tmp);
    }
    return;

    case OP_ACCESS: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->mode), callback};
      bindings_call_op(b, b->ops_access, 3, tmp);
    }
    return;

    case OP_OPEN: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->mode), callback};
      bindings_call_op(b, b->ops_open, 3, tmp);
    }
    return;

    case OP_OPENDIR: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->mode), callback};
      bindings_call_op(b, b->ops_opendir, 3, tmp);
    }
    return;

    case OP_WRITE: {
      Local<Value> tmp[] = {
        Nan::New<String>(b->path).ToLocalChecked(),
        Nan::New<Number>(b->info->fh),
        bindings_buffer((char *) b->data, b->length),
        Nan::New<Number>(b->length),
        Nan::New<Number>(b->offset),
        callback
      };
      bindings_call_op(b, b->ops_write, 6, tmp);
    }
    return;

    case OP_READ: {
      Local<Value> tmp[] = {
        Nan::New<String>(b->path).ToLocalChecked(),
        Nan::New<Number>(b->info->fh),
        bindings_buffer((char *) b->data, b->length),
        Nan::New<Number>(b->length),
        Nan::New<Number>(b->offset),
        callback
      };
      bindings_call_op(b, b->ops_read, 6, tmp);
    }
    return;

    case OP_RELEASE: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->info->fh), callback};
      bindings_call_op(b, b->ops_release, 3, tmp);
    }
    return;

    case OP_RELEASEDIR: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->info->fh), callback};
      bindings_call_op(b, b->ops_releasedir, 3, tmp);
    }
    return;

    case OP_UNLINK: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), callback};
      bindings_call_op(b, b->ops_unlink, 2, tmp);
    }
    return;

    case OP_RENAME: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<String>((char *) b->data).ToLocalChecked(), callback};
      bindings_call_op(b, b->ops_rename, 3, tmp);
    }
    return;

    case OP_LINK: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<String>((char *) b->data).ToLocalChecked(), callback};
      bindings_call_op(b, b->ops_link, 3, tmp);
    }
    return;

    case OP_SYMLINK: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<String>((char *) b->data).ToLocalChecked(), callback};
      bindings_call_op(b, b->ops_symlink, 3, tmp);
    }
    return;

    case OP_CHMOD: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->mode), callback};
      bindings_call_op(b, b->ops_chmod, 3, tmp);
    }
    return;

    case OP_MKNOD: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->mode), Nan::New<Number>(b->dev), callback};
      bindings_call_op(b, b->ops_mknod, 4, tmp);
    }
    return;

    case OP_CHOWN: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->uid), Nan::New<Number>(b->gid), callback};
      bindings_call_op(b, b->ops_chown, 4, tmp);
    }
    return;

    case OP_READLINK: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), callback};
      bindings_call_op(b, b->ops_readlink, 2, tmp);
    }
    return;

    case OP_SETXATTR: {
      Local<Value> tmp[] = {
        Nan::New<String>(b->path).ToLocalChecked(),
        Nan::New<String>(b->name).ToLocalChecked(),
        bindings_buffer((char *) b->data, b->length),
        Nan::New<Number>(b->length),
        Nan::New<Number>(b->offset),
        Nan::New<Number>(b->mode),
        callback
      };
      bindings_call_op(b, b->ops_setxattr, 7, tmp);
    }
    return;

    case OP_GETXATTR: {
      Local<Value> tmp[] = {
        Nan::New<String>(b->path).ToLocalChecked(),
        Nan::New<String>(b->name).ToLocalChecked(),
        bindings_buffer((char *) b->data, b->length),
        Nan::New<Number>(b->length),
        Nan::New<Number>(b->offset),
        callback
      };
      bindings_call_op(b, b->ops_getxattr, 6, tmp);
    }
    return;

    case OP_MKDIR: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->mode), callback};
      bindings_call_op(b, b->ops_mkdir, 3, tmp);
    }
    return;

    case OP_RMDIR: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), callback};
      bindings_call_op(b, b->ops_rmdir, 2, tmp);
    }
    return;

    case OP_DESTROY: {
      Local<Value> tmp[] = {callback};
      bindings_call_op(b, b->ops_destroy, 1, tmp);
    }
    return;

    case OP_UTIMENS: {
#ifdef _WIN32
      time_t *tv = (time_t *) b->data;
#else
      struct timespec *tv = (struct timespec *) b->data;
#endif
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), bindings_get_date(tv), bindings_get_date(tv + 1), callback};
      bindings_call_op(b, b->ops_utimens, 4, tmp);
    }
    return;

    case OP_FLUSH: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->info->fh), callback};
      bindings_call_op(b, b->ops_flush, 3, tmp);
    }
    return;

    case OP_FSYNC: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->info->fh), Nan::New<Number>(b->mode), callback};
      bindings_call_op(b, b->ops_fsync, 4, tmp);
    }
    return;

    case OP_FSYNCDIR: {
      Local<Value> tmp[] = {Nan::New<String>(b->path).ToLocalChecked(), Nan::New<Number>(b->info->fh), Nan::New<Number>(b->mode), callback};
      bindings_call_op(b, b->ops_fsyncdir, 4, tmp);
    }
    return;
  }

  semaphore_signal(&(b->semaphore));
}

static int bindings_alloc () {
  int free_index = -1;
  size_t size = sizeof(bindings_t);

  for (int i = 0; i < bindings_mounted_count; i++) {
    if (bindings_mounted[i] == NULL) {
      free_index = i;
      break;
    }
  }

  if (free_index == -1 && bindings_mounted_count < 1024) free_index = bindings_mounted_count++;
  if (free_index != -1) {
    bindings_t *b = bindings_mounted[free_index] = (bindings_t *) malloc(size);
    memset(b, 0, size);
    b->index = free_index;
  }

  return free_index;
}

NAN_METHOD(Mount) {
  if (!info[0]->IsString()) return Nan::ThrowError("mnt must be a string");

  mutex_lock(&mutex);
  int index = bindings_alloc();
  mutex_unlock(&mutex);

  if (index == -1) return Nan::ThrowError("You cannot mount more than 1024 filesystem in one process");

  mutex_lock(&mutex);
  bindings_t *b = bindings_mounted[index];
  mutex_unlock(&mutex);

  memset(&empty_stat, 0, sizeof(empty_stat));
  memset(b, 0, sizeof(bindings_t));

  Nan::Utf8String path(info[0]);
  Local<Object> ops = info[1].As<Object>();

  b->ops_init = ops->Has(Nan::New<String>("init").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("init").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_error = ops->Has(Nan::New<String>("error").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("error").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_access = ops->Has(Nan::New<String>("access").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("access").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_statfs = ops->Has(Nan::New<String>("statfs").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("statfs").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_getattr = ops->Has(Nan::New<String>("getattr").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("getattr").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_fgetattr = ops->Has(Nan::New<String>("fgetattr").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("fgetattr").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_flush = ops->Has(Nan::New<String>("flush").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("flush").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_fsync = ops->Has(Nan::New<String>("fsync").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("fsync").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_fsyncdir = ops->Has(Nan::New<String>("fsyncdir").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("fsyncdir").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_readdir = ops->Has(Nan::New<String>("readdir").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("readdir").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_truncate = ops->Has(Nan::New<String>("truncate").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("truncate").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_ftruncate = ops->Has(Nan::New<String>("ftruncate").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("ftruncate").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_readlink = ops->Has(Nan::New<String>("readlink").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("readlink").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_chown = ops->Has(Nan::New<String>("chown").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("chown").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_chmod = ops->Has(Nan::New<String>("chmod").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("chmod").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_mknod = ops->Has(Nan::New<String>("mknod").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("mknod").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_setxattr = ops->Has(Nan::New<String>("setxattr").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("setxattr").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_getxattr = ops->Has(Nan::New<String>("getxattr").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("getxattr").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_open = ops->Has(Nan::New<String>("open").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("open").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_opendir = ops->Has(Nan::New<String>("opendir").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("opendir").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_read = ops->Has(Nan::New<String>("read").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("read").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_write = ops->Has(Nan::New<String>("write").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("write").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_release = ops->Has(Nan::New<String>("release").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("release").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_releasedir = ops->Has(Nan::New<String>("releasedir").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("releasedir").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_create = ops->Has(Nan::New<String>("create").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("create").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_utimens = ops->Has(Nan::New<String>("utimens").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("utimens").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_unlink = ops->Has(Nan::New<String>("unlink").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("unlink").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_rename = ops->Has(Nan::New<String>("rename").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("rename").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_link = ops->Has(Nan::New<String>("link").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("link").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_symlink = ops->Has(Nan::New<String>("symlink").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("symlink").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_mkdir = ops->Has(Nan::New<String>("mkdir").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("mkdir").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_rmdir = ops->Has(Nan::New<String>("rmdir").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("rmdir").ToLocalChecked()).As<Function>()) : NULL;
  b->ops_destroy = ops->Has(Nan::New<String>("destroy").ToLocalChecked()) ? new Nan::Callback(ops->Get(Nan::New<String>("destroy").ToLocalChecked()).As<Function>()) : NULL;

  Local<Value> tmp[] = {Nan::New<Number>(index), Nan::New<FunctionTemplate>(OpCallback)->GetFunction()};
  b->callback = new Nan::Callback(callback_constructor->Call(2, tmp).As<Function>());

  strcpy(b->mnt, *path);
  strcpy(b->mntopts, "-o");

  Local<Array> options = ops->Get(Nan::New<String>("options").ToLocalChecked()).As<Array>();
  if (options->IsArray()) {
    for (uint32_t i = 0; i < options->Length(); i++) {
      Nan::Utf8String option(options->Get(i));
      if (strcmp(b->mntopts, "-o")) strcat(b->mntopts, ",");
      strcat(b->mntopts, *option);
    }
  }

  semaphore_init(&(b->semaphore));
  uv_async_init(uv_default_loop(), &(b->async), (uv_async_cb) bindings_dispatch);
  b->async.data = b;

  thread_create(&(b->thread), bindings_thread, b);
}

class UnmountWorker : public Nan::AsyncWorker {
 public:
  UnmountWorker(Nan::Callback *callback, char *path)
    : Nan::AsyncWorker(callback), path(path) {}
  ~UnmountWorker() {}

  void Execute () {
    bindings_unmount(path);
    free(path);
  }

  void HandleOKCallback () {
    Nan::HandleScope scope;
    callback->Call(0, NULL);
  }

 private:
  char *path;
};

NAN_METHOD(SetCallback) {
  callback_constructor = new Nan::Callback(info[0].As<Function>());
}

NAN_METHOD(SetBuffer) {
  buffer_constructor.Reset(info[0].As<Function>());
}

NAN_METHOD(PopulateContext) {
  if (bindings_current == NULL) return Nan::ThrowError("You have to call this inside a fuse operation");

  Local<Object> ctx = info[0].As<Object>();
  ctx->Set(Nan::New<String>("uid").ToLocalChecked(), Nan::New(bindings_current->context_uid));
  ctx->Set(Nan::New<String>("gid").ToLocalChecked(), Nan::New(bindings_current->context_gid));
  ctx->Set(Nan::New<String>("pid").ToLocalChecked(), Nan::New(bindings_current->context_pid));
}

NAN_METHOD(Unmount) {
  if (!info[0]->IsString()) return Nan::ThrowError("mnt must be a string");
  Nan::Utf8String path(info[0]);
  Local<Function> callback = info[1].As<Function>();

  char *path_alloc = (char *) malloc(1024);
  strcpy(path_alloc, *path);

  Nan::AsyncQueueWorker(new UnmountWorker(new Nan::Callback(callback), path_alloc));
}

void Init(Handle<Object> exports) {
  exports->Set(Nan::New("setCallback").ToLocalChecked(), Nan::New<FunctionTemplate>(SetCallback)->GetFunction());
  exports->Set(Nan::New("setBuffer").ToLocalChecked(), Nan::New<FunctionTemplate>(SetBuffer)->GetFunction());
  exports->Set(Nan::New("mount").ToLocalChecked(), Nan::New<FunctionTemplate>(Mount)->GetFunction());
  exports->Set(Nan::New("unmount").ToLocalChecked(), Nan::New<FunctionTemplate>(Unmount)->GetFunction());
  exports->Set(Nan::New("populateContext").ToLocalChecked(), Nan::New<FunctionTemplate>(PopulateContext)->GetFunction());
}

NODE_MODULE(fuse_bindings, Init)
