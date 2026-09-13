#include "sandbox/host.h"
#include "sandbox_backend/backend.h"
#include "sandbox_backend/virtio_9p.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void init_dev(ant_hvf_9p_device_t *dev, const char *root) {
  memset(dev, 0, sizeof(*dev));
  dev->root = root;
}

static void cleanup_dev(ant_hvf_9p_device_t *dev) {
  ant_hvf_9p_stat_cache_clear(dev);
  ant_hvf_9p_file_cache_clear(dev);
  free(dev->fids);
}

#if defined(__aarch64__) && defined(__APPLE__)

static void test_relative_path_policy(void) {
  char root_template[] = "/tmp/ant-9p-policy.XXXXXX";
  char *root_tmp = mkdtemp(root_template);
  assert(root_tmp);

  char root[4096];
  assert(realpath(root_tmp, root));

  ant_hvf_9p_device_t dev;
  init_dev(&dev, root);

  char host[4096];
  assert(ant_hvf_9p_existing_path(&dev, "", host, sizeof(host)) == 0);
  assert(strcmp(host, root) == 0);
  assert(ant_hvf_9p_host_path(&dev, "../x", host, sizeof(host)) == -ENOENT);
  assert(ant_hvf_9p_host_path(&dev, "/x", host, sizeof(host)) == -ENOENT);
  assert(ant_hvf_9p_host_path(&dev, "a//b", host, sizeof(host)) == -ENOENT);
  assert(ant_hvf_9p_host_path(&dev, "a/./b", host, sizeof(host)) == -ENOENT);
  assert(ant_hvf_9p_host_path(&dev, "a/../b", host, sizeof(host)) == -ENOENT);

  rmdir(root);
}

static void test_symlink_escape_policy(void) {
  char root_template[] = "/tmp/ant-9p-root.XXXXXX";
  char external_template[] = "/tmp/ant-9p-external.XXXXXX";
  char *root_tmp = mkdtemp(root_template);
  char *external_tmp = mkdtemp(external_template);
  assert(root_tmp);
  assert(external_tmp);

  char root[4096];
  char external[4096];
  assert(realpath(root_tmp, root));
  assert(realpath(external_tmp, external));

  char link_path[4096];
  int written = snprintf(link_path, sizeof(link_path), "%s/escape", root);
  assert(written > 0 && (size_t)written < sizeof(link_path));
  assert(symlink(external, link_path) == 0);

  ant_hvf_9p_device_t dev;
  init_dev(&dev, root);

  char host[4096];
  assert(ant_hvf_9p_existing_path(&dev, "escape", host, sizeof(host)) == -EPERM);
  assert(ant_hvf_9p_child_path(&dev, "escape", "new-file", host, sizeof(host)) == -EPERM);

  assert(ant_hvf_9p_symlink_target_bad(""));
  assert(ant_hvf_9p_symlink_target_bad("/abs"));
  assert(ant_hvf_9p_symlink_target_bad("../x"));
  assert(ant_hvf_9p_symlink_target_bad("a/../b"));
  assert(ant_hvf_9p_symlink_target_bad("a//b"));
  assert(!ant_hvf_9p_symlink_target_bad("target"));
  assert(!ant_hvf_9p_symlink_target_bad("dir/file"));

  unlink(link_path);
  rmdir(root);
  rmdir(external);
}

#else

static void test_relative_path_policy(void) {}
static void test_symlink_escape_policy(void) {}

#endif

static void test_tread_count_is_bounded(bool readonly) {
  enum { guard_size = 32 };
  char root_template[] = "/tmp/ant-9p-read.XXXXXX";
  char *root_tmp = mkdtemp(root_template);
  assert(root_tmp);

  char root[4096];
  assert(realpath(root_tmp, root));

  char file[4096];
  int written = snprintf(file, sizeof(file), "%s/payload", root);
  assert(written > 0 && (size_t)written < sizeof(file));
  int fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  assert(fd >= 0);
  assert(ftruncate(fd, ANT_HVF_9P_MAX_MSIZE + 13) == 0);
  assert(close(fd) == 0);

  ant_hvf_9p_device_t dev;
  init_dev(&dev, root);
  dev.readonly = readonly;
  ant_hvf_9p_fid_t *fid = ant_hvf_9p_fid(&dev, 42, true);
  assert(fid);
  fid->active = true;
  snprintf(fid->path, sizeof(fid->path), "payload");

  unsigned char req[23] = {0};
  ant_hvf_store32(req, sizeof(req));
  req[4] = P9_TREAD;
  ant_hvf_store16(req + 5, 7);
  ant_hvf_store32(req + 7, fid->fid);
  ant_hvf_store64(req + 11, 0);
  ant_hvf_store32(req + 19, UINT32_MAX);

  unsigned char *resp = malloc(ANT_HVF_9P_MAX_MSIZE + guard_size);
  assert(resp);
  memset(resp, 0xa5, ANT_HVF_9P_MAX_MSIZE + guard_size);

  uint32_t resp_len = ant_hvf_9p_handle(&dev, req, sizeof(req), resp,
                                        ANT_HVF_9P_MAX_MSIZE);
  assert(resp_len == ANT_HVF_9P_MAX_MSIZE);
  assert(ant_hvf_load32(resp) == ANT_HVF_9P_MAX_MSIZE);
  assert(resp[4] == P9_RREAD);
  assert(ant_hvf_load32(resp + 7) == ANT_HVF_9P_MAX_MSIZE - 11u);
  for (size_t i = ANT_HVF_9P_MAX_MSIZE;
       i < ANT_HVF_9P_MAX_MSIZE + guard_size; i++) {
    assert(resp[i] == 0xa5);
  }

  free(resp);
  cleanup_dev(&dev);
  assert(unlink(file) == 0);
  assert(rmdir(root) == 0);
}

static void test_twrite_count_overflow_is_rejected(void) {
  char root_template[] = "/tmp/ant-9p-write.XXXXXX";
  char *root_tmp = mkdtemp(root_template);
  assert(root_tmp);

  char root[4096];
  assert(realpath(root_tmp, root));

  char file[4096];
  int written = snprintf(file, sizeof(file), "%s/payload", root);
  assert(written > 0 && (size_t)written < sizeof(file));
  int fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  assert(fd >= 0);
  assert(close(fd) == 0);

  ant_hvf_9p_device_t dev;
  init_dev(&dev, root);
  ant_hvf_9p_fid_t *fid = ant_hvf_9p_fid(&dev, 42, true);
  assert(fid);
  fid->active = true;
  snprintf(fid->path, sizeof(fid->path), "payload");

  unsigned char req[23] = {0};
  ant_hvf_store32(req, sizeof(req));
  req[4] = P9_TWRITE;
  ant_hvf_store16(req + 5, 7);
  ant_hvf_store32(req + 7, fid->fid);
  ant_hvf_store64(req + 11, 0);
  ant_hvf_store32(req + 19, UINT32_MAX);

  unsigned char resp[ANT_HVF_9P_MIN_MSIZE] = {0};
  uint32_t resp_len = ant_hvf_9p_handle(&dev, req, sizeof(req), resp,
                                        sizeof(resp));
  assert(resp_len == 11);
  assert(resp[4] == P9_RLERROR);
  assert(ant_hvf_load32(resp + 7) == EINVAL);

  struct stat st;
  assert(stat(file, &st) == 0);
  assert(st.st_size == 0);

  cleanup_dev(&dev);
  assert(unlink(file) == 0);
  assert(rmdir(root) == 0);
}

static void test_temp_write_cleanup(void) {
  ant_sandbox_launch_options_t opts;
  ant_sandbox_launch_options_init(&opts);

  char err[512] = {0};
  assert(ant_sandbox_launch_add_mount(&opts, "tmp:/tmp", false, err, sizeof(err)) == 0);
  assert(opts.temp_dir_count == 1);
  assert(opts.mount_count == 1);
  assert(!opts.mounts[0].readonly);

  char temp_dir[4096];
  snprintf(temp_dir, sizeof(temp_dir), "%s", opts.temp_dirs[0]);
  assert(access(temp_dir, F_OK) == 0);

  char nested[4096];
  int written = snprintf(nested, sizeof(nested), "%s/nested", temp_dir);
  assert(written > 0 && (size_t)written < sizeof(nested));
  assert(mkdir(nested, 0700) == 0);

  char file[4096];
  written = snprintf(file, sizeof(file), "%s/file", nested);
  assert(written > 0 && (size_t)written < sizeof(file));
  FILE *fp = fopen(file, "w");
  assert(fp);
  fputs("ok", fp);
  fclose(fp);

  ant_sandbox_launch_options_cleanup(&opts);
  assert(opts.temp_dir_count == 0);
  assert(access(temp_dir, F_OK) != 0);
  assert(errno == ENOENT);
}

int main(void) {
  test_relative_path_policy();
  test_symlink_escape_policy();
  test_tread_count_is_bounded(true);
  test_tread_count_is_bounded(false);
  test_twrite_count_overflow_is_rejected();
  test_temp_write_cleanup();
  return 0;
}
