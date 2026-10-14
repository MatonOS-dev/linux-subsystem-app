/* SPDX-License-Identifier: Apache-2.0
 * Original userspace implementation; no kernel implementation code used.
 * Offsets, never process pointers, are stored in the shared arena. */
#define _GNU_SOURCE
#include "sysvipc.h"
#include "proxy.h"
#include "vfs.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <linux/msg.h>
#include <linux/sem.h>
#include <linux/shm.h>
#include <stdint.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include "arch/ipc.h"
#include "arch/time.h"
#define R(n, a, b, c, d, e, f) RAW(PROXY_SC_##n, a, b, c, d, e, f)
#define PAGE 4096UL
#define MAXSEM 32767
#define AGAIN (-4096L)
typedef uint64_t off;
struct head {
  _Alignas(8) uint64_t mutex;
  uint32_t seq, format;
  _Alignas(8) off size, objects, records, free_messages, fork_sequence;
  int nextid;
  unsigned transaction, poisoned;
};
struct obj {
  _Alignas(8) off next, data, messages;
  int id, kind, key, removed, index;
  struct proxy_ipc_perm perm;
  _Alignas(8) uint64_t count, size, bytes, qbytes;
  int64_t ctime, atime, dtime;
  int cpid, lpid;
};
struct cell {
  int value, pid, ncnt, zcnt;
};
/* type 1: undo; type 2: attachment; type 3: wait counter */
struct rec {
  _Alignas(8) off next;
  int type, pid, id, index;
  uint64_t birth;
  int64_t value;
  uint64_t addr, len;
};
struct msg {
  _Alignas(8) off next;
  int64_t type;
  uint64_t len, capacity;
};
_Static_assert(sizeof(struct head)==72,"shared IPC v2 header");
_Static_assert(sizeof(struct obj)==144,"shared IPC v2 object");
_Static_assert(sizeof(struct rec)==56,"shared IPC v2 record");
_Static_assert(sizeof(struct msg)==32,"shared IPC v2 message");
struct ctx {
  int fd;
  unsigned char *base;
  size_t size;
};
static struct head *shared;
static char arena_path[4096], journal_path[4096];
static int recover(void);
static long init_error;
struct local_attach {
  struct local_attach *next;
  uint64_t addr, len;
  int id;
};
static struct local_attach *attached;
static int local_pid;
struct pending_fork {
  struct ctx context;
  int owner, tid, ticket_fd;
  off ticket;
};
static struct pending_fork pending;
static void ticket_path(char *, off);
static int ticket_alive(off);

static unsigned long decimal(char *p, unsigned long n) {
  char b[24];
  unsigned k = 0, i = 0;
  do {
    b[k++] = (char)('0' + n % 10);
    n /= 10;
  } while (n);
  while (k)
    p[i++] = b[--k];
  return i;
}
static int64_t now(int clock) {
  struct proxy_timespec t;
  if (arch_clock_time(clock,&t))
    return 0;
  return t.sec * INT64_C(1000000000) + t.nsec;
}
/* /proc start time prevents a reused numeric PID from inheriting undo or
 * holding a dead writer's lock forever. Unknown proc failures are live. */
static int task_count(int pid) {
  char path[64] = "/proc/";
  unsigned n = 6;
  n += decimal(path + n, (unsigned)pid);
  p_copy(path + n, "/task", 6);
  long fd =
      R(openat, AT_FDCWD, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0, 0, 0);
  if (fd < 0)
    return -1;
  char buf[4096];
  int count = 0;
  long bytes;
  while ((bytes = R(getdents64, fd, buf, sizeof(buf), 0, 0, 0)) > 0) {
    for (long at = 0; at < bytes;) {
      unsigned short len = *(unsigned short *)(buf + at + 16);
      if (len < 20 || at + len > bytes) {
        count = -1;
        break;
      }
      const char *name = buf + at + 19;
      if (name[0] != '.')
        count++;
      at += len;
    }
    if (count < 0)
      break;
  }
  R(close, fd, 0, 0, 0, 0, 0);
  return count;
}
static uint64_t identity(int pid, int process) {
  char path[64] = "/proc/";
  unsigned k = 6;
  k += decimal(path + k, (unsigned)pid);
  p_copy(path + k, "/stat", 6);
  long fd = R(openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0, 0, 0);
  if (fd < 0)
    return fd == -ENOENT ? 0 : UINT64_MAX;
  char buf[4096];
  long n = R(read, fd, buf, sizeof(buf) - 1, 0, 0, 0);
  R(close, fd, 0, 0, 0, 0, 0);
  if (n <= 0)
    return UINT64_MAX;
  buf[n] = 0;
  char *p = 0;
  for (long i = n - 3; i >= 0; i--)
    if (buf[i] == ')' && buf[i + 1] == ' ') {
      p = buf + i + 2;
      break;
    }
  if (!p)
    return UINT64_MAX;
  if ((*p == 'Z' || *p == 'X') && (!process || task_count(pid) <= 1))
    return 0;
  for (int field = 3; field < 22; field++) {
    while (*p && *p != ' ')
      p++;
    while (*p == ' ')
      p++;
  }
  uint64_t value = 0;
  while (*p >= '0' && *p <= '9')
    value = value * 10 + (unsigned)(*p++ - '0');
  return value;
}
static uint64_t birth(int pid) { return identity(pid, 0); }
static int alive(int pid, uint64_t token, int process) {
  uint64_t current = identity(pid, process);
  return current &&
         (current == UINT64_MAX || token == UINT64_MAX || current == token);
}
int ipc_init(void) {
  if (shared)
    return 0;
  long error = 0;
  char *directory = vfs_temp_create(&error);
  if (!directory)
    return (int)(init_error = error);
  size_t n = p_len(directory);
  if (n + 70 >= sizeof(arena_path)) {
    vfs_free(directory);
    return (int)(init_error = -ENAMETOOLONG);
  }
  p_copy(arena_path, directory, n);
  p_copy(arena_path + n, "/sysvipc", 9);
  p_copy(journal_path, directory, n);
  p_copy(journal_path + n, "/journal", 9);
  vfs_free(directory);
  long fd = R(openat, AT_FDCWD, arena_path,
              O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600, 0, 0);
  if (fd < 0)
    return (int)(init_error = fd);
  long r = R(ftruncate, fd, PAGE, 0, 0, 0, 0);
  if (!r) {
    r = R(mmap, 0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if ((unsigned long)r < (unsigned long)-4095) {
      shared = (void *)r;
      shared->format = 2;
      shared->size = PAGE;
      shared->nextid = 1;
      local_pid = (int)R(getpid, 0, 0, 0, 0, 0, 0);
      r = 0;
    }
  }
  R(close, fd, 0, 0, 0, 0, 0);
  if (r < 0)
    init_error = r;
  return r < 0 ? (int)r : 0;
}
static void wake(void) {
  __atomic_add_fetch(&shared->seq, 1, __ATOMIC_RELEASE);
  R(futex, &shared->seq, FUTEX_WAKE, INT_MAX, 0, 0, 0);
}
static void unlock(void) {
  __atomic_store_n(&shared->mutex, 0, __ATOMIC_RELEASE);
  R(futex, &shared->mutex, FUTEX_WAKE, 1, 0, 0, 0);
}
static int lock(void) {
  unsigned tid = (unsigned)R(gettid, 0, 0, 0, 0, 0, 0);
  uint64_t token = birth((int)tid),
           mine = ((uint64_t)(unsigned)token << 32) | tid;
  for (;;) {
    uint64_t expected = 0;
    if (__atomic_compare_exchange_n(&shared->mutex, &expected, mine, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      if (shared->transaction && recover() < 0)
        shared->poisoned = 1;
      return 0;
    }
    struct timespec t = {0, 10000000};
    R(futex, &shared->mutex, FUTEX_WAIT, (unsigned)expected, &t, 0, 0);
    uint64_t current = birth((int)(unsigned)expected);
    if (!current || (current != UINT64_MAX &&
                     (unsigned)current != (unsigned)(expected >> 32)))
      __atomic_compare_exchange_n(&shared->mutex, &expected, 0, 0,
                                  __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  }
}
static void *ptr(struct ctx *c, off o) { return c->base + o; }
static long remap(struct ctx *c) {
  size_t size = shared->size;
  long p = R(mmap, 0, size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
  if ((unsigned long)p >= (unsigned long)-4095)
    return p;
  if (c->base)
    R(munmap, c->base, c->size, 0, 0, 0, 0);
  c->base = (void *)p;
  c->size = size;
  return 0;
}
static off alloc(struct ctx *c, size_t n) {
  if (n > SIZE_MAX - PAGE || shared->size > SIZE_MAX - n - PAGE)
    return 0;
  off o = shared->size;
  size_t end = (o + n + PAGE - 1) & ~(PAGE - 1);
  if (R(ftruncate, c->fd, end, 0, 0, 0, 0) < 0)
    return 0;
  shared->size = end;
  if (remap(c) < 0) {
    shared->size = o;
    R(ftruncate, c->fd, o, 0, 0, 0, 0);
    return 0;
  }
  return o;
}
/* A per-call rollback journal covers metadata, never the payload's shm pages.
 * Publish transaction=1 only after the complete before-image exists. A killed
 * writer is rolled back by the next lock owner before metadata is inspected.
 * This is process-crash recovery, not filesystem/power-loss durability. */
struct journal_entry {
  off offset, length;
};
static long full_write(int fd, const void *data, size_t n) {
  size_t at = 0;
  while (at < n) {
    long r = R(write, fd, (const char *)data + at, n - at, 0, 0, 0);
    if (r <= 0)
      return r ? r : -EIO;
    at += r;
  }
  return 0;
}
static long snapshot(int fd, off offset, const void *data, size_t len) {
  struct journal_entry e = {offset, len};
  long r = full_write(fd, &e, sizeof(e));
  return r ? r : full_write(fd, data, len);
}
static int begin(struct ctx *c) {
  if (shared->poisoned)
    return -EIO;
  int fd =
      (int)R(openat, AT_FDCWD, journal_path,
             O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600, 0, 0);
  if (fd < 0)
    return fd;
  /* Header backup excludes lock/sequence and transaction flags. */
  long r = snapshot(fd, offsetof(struct head, size),
                    (char *)shared + offsetof(struct head, size),
                    offsetof(struct head, transaction) -
                        offsetof(struct head, size));
  for (off o = shared->objects; o && !r;) {
    struct obj *p = ptr(c, o);
    r = snapshot(fd, o, p, sizeof(*p));
    if (!r && p->kind == 1 && p->removed != 2)
      r = snapshot(fd, p->data, ptr(c, p->data),
                   p->count * sizeof(struct cell));
    for (off m = p->removed == 2 ? 0 : p->messages; m && !r;) {
      struct msg *q = ptr(c, m);
      r = snapshot(fd, m, q, sizeof(*q) + q->len);
      m = q->next;
    }
    o = p->next;
  }
  for (off m = shared->free_messages; m && !r;) {
    struct msg *q = ptr(c, m);
    r = snapshot(fd, m, q, sizeof(*q) + q->capacity);
    m = q->next;
  }
  for (off o = shared->records; o && !r;) {
    struct rec *q = ptr(c, o);
    r = snapshot(fd, o, q, sizeof(*q));
    o = q->next;
  }
  R(close, fd, 0, 0, 0, 0, 0);
  if (!r)
    __atomic_store_n(&shared->transaction, 1, __ATOMIC_RELEASE);
  return (int)r;
}
static void commit(void) {
  __atomic_store_n(&shared->transaction, 0, __ATOMIC_RELEASE);
}
static int recover(void) {
  int j = (int)R(openat, AT_FDCWD, journal_path,
                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0, 0, 0);
  if (j < 0)
    return j;
  int fd = (int)R(openat, AT_FDCWD, arena_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                  0, 0, 0);
  long r = fd < 0 ? fd : 0;
  struct proxy_stat st;
  if (!r)
    r = arch_stat_fd( j,&st);
  long mapped = -1;
  if (!r) {
    mapped = R(mmap, 0, st.st_size, PROT_READ, MAP_PRIVATE, j, 0);
    if ((unsigned long)mapped >= (unsigned long)-4095)
      r = mapped;
  }
  size_t at = 0;
  while (!r && at < (size_t)st.st_size) {
    struct journal_entry *e = (void *)(mapped + at);
    if ((size_t)st.st_size - at < sizeof(*e) ||
        e->length > (size_t)st.st_size - at - sizeof(*e)) {
      r = -EIO;
      break;
    }
    at += sizeof(*e);
    size_t done = 0;
    while (done < e->length) {
      long n = R(pwrite64, fd, (void *)(mapped + at + done), e->length - done,
                 e->offset + done, 0, 0);
      if (n <= 0) {
        r = n ? n : -EIO;
        break;
      }
      done += n;
    }
    at += e->length;
  }
  if ((unsigned long)mapped < (unsigned long)-4095)
    R(munmap, mapped, st.st_size, 0, 0, 0, 0);
  if (fd >= 0)
    R(close, fd, 0, 0, 0, 0, 0);
  R(close, j, 0, 0, 0, 0, 0);
  if (!r) {
    commit();
    wake();
  }
  return (int)r;
}
static struct obj *find(struct ctx *c, int id) {
  for (off o = shared->objects; o;) {
    struct obj *p = ptr(c, o);
    if (p->id == id && p->removed != 2)
      return p;
    o = p->next;
  }
  return 0;
}
static off objoff(struct ctx *c, struct obj *p) {
  return (unsigned char *)p - c->base;
}
static struct cell *cells(struct ctx *c, struct obj *p) {
  return ptr(c, p->data);
}
static void reap(struct ctx *c, int force) {
  int pid = (int)R(getpid, 0, 0, 0, 0, 0, 0);
  for (off r = shared->records; r;) {
    struct rec *q = ptr(c, r);
    r = q->next;
    if (!q->type)
      continue;
    if (q->type == 6) {
      if (ticket_alive((off)q->value))
        continue;
    } else if (!(q->pid == pid ? force
                               : !alive(q->pid, q->birth, q->type != 3)))
      continue;
    struct obj *p = find(c, q->id);
    if (p && q->type == 1) {
      struct cell *v = cells(c, p);
      long value = v[q->index].value + q->value;
      if (value < 0)
        value = 0;
      if (value > MAXSEM)
        value = MAXSEM;
      v[q->index].value = (int)value;
      v[q->index].pid = q->pid;
      p->atime = now(CLOCK_REALTIME) / 1000000000L;
    }
    if (p && (q->type == 2 || q->type == 6)) {
      if (p->count)
        p->count--;
      p->dtime = now(CLOCK_REALTIME) / 1000000000L;
      p->lpid = q->pid;
      if (p->removed && !p->count)
        p->removed = 2;
    }
    if (p && q->type == 3) {
      struct cell *v = cells(c, p);
      if (q->value)
        v[q->index].zcnt--;
      else
        v[q->index].ncnt--;
    }
    q->type = 0;
    wake();
  }
}
static off new_record(struct ctx *c) {
  for (off r = shared->records; r;) {
    struct rec *q = ptr(c, r);
    if (!q->type) {
      off next = q->next;
      struct rec empty = {0};
      *q = empty;
      q->next = next;
      return r;
    }
    r = q->next;
  }
  off r = alloc(c, sizeof(struct rec));
  if (!r)
    return 0;
  struct rec *q = ptr(c, r);
  q->next = shared->records;
  shared->records = r;
  return r;
}
static off record(struct ctx *c, int type, int id, int index) {
  int pid = (int)RAW(type == 3 ? PROXY_SC_gettid : PROXY_SC_getpid, 0, 0, 0, 0, 0, 0);
  for (off r = shared->records; r;) {
    struct rec *q = ptr(c, r);
    if (q->type == type && q->pid == pid && q->id == id && q->index == index)
      return r;
    r = q->next;
  }
  off r = new_record(c);
  if (!r)
    return 0;
  struct rec *q = ptr(c, r);
  q->type = type;
  q->pid = pid;
  q->birth = identity(pid, type != 3);
  q->id = id;
  q->index = index;
  return r;
}
static int access_obj(struct obj *p, int mode) {
  unsigned uid = (unsigned)R(geteuid, 0, 0, 0, 0, 0, 0),
           gid = (unsigned)R(getegid, 0, 0, 0, 0, 0, 0);
  unsigned bits = p->perm.mode;
  if (uid == p->perm.uid || uid == p->perm.cuid)
    bits >>= 6;
  else if (gid == p->perm.gid || gid == p->perm.cgid)
    bits >>= 3;
  return (bits & mode) == (unsigned)mode ? 0 : -EACCES;
}
static int owner(struct obj *p) {
  unsigned uid = (unsigned)R(geteuid, 0, 0, 0, 0, 0, 0);
  return uid == p->perm.uid || uid == p->perm.cuid ? 0 : -EPERM;
}
static void segment_path(char *path, int id) {
  size_t n = p_len(arena_path) - 8;
  p_copy(path, arena_path, n);
  p_copy(path + n, "/shm-", 5);
  n += 5;
  n += decimal(path + n, (unsigned)id);
  path[n] = 0;
}
static void ticket_path(char *path, off ticket) {
  size_t n = p_len(arena_path) - 8;
  p_copy(path, arena_path, n);
  p_copy(path + n, "/fork-", 6);
  n += 6;
  n += decimal(path + n, ticket);
  path[n] = 0;
}
static int ticket_alive(off ticket) {
  char path[4096];
  ticket_path(path, ticket);
  long fd =
      R(openat, AT_FDCWD, path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0, 0, 0);
  if (fd < 0)
    return fd != -ENOENT;
  long r = R(flock, fd, LOCK_EX | LOCK_NB, 0, 0, 0, 0);
  R(close, fd, 0, 0, 0, 0, 0);
  if (!r)
    R(unlinkat, AT_FDCWD, path, 0, 0, 0, 0);
  return r != 0;
}
static void cleanup_segments(struct ctx *c) {
  for (off o = shared->objects; o;) {
    struct obj *p = ptr(c, o);
    o = p->next;
    if (p->kind == 2 && p->removed == 2) {
      char path[4096];
      segment_path(path, p->id);
      R(unlinkat, AT_FDCWD, path, 0, 0, 0, 0);
    }
  }
}
static long get(struct ctx *c, int kind, int key, unsigned long size,
                int flags) {
  int active = 0;
  unsigned char used[32000] = {0};
  unsigned long total = 0;
  off reusable = 0, reused_data = 0, reused_next = 0;
  size_t reused_capacity = 0;
  for (off o = shared->objects; o;) {
    struct obj *p = ptr(c, o);
    if (p->kind == kind && p->removed == 2 && (kind != 1 || p->size >= size)) {
      reusable = o;
      reused_data = p->data;
      reused_capacity = p->size;
      reused_next = p->next;
    }
    o = p->next;
    if (p->kind != kind || p->removed == 2)
      continue;
    active++;
    used[p->index] = 1;
    total += p->count;
    if (key && !p->removed && p->key == key) {
      if ((flags & (IPC_CREAT | IPC_EXCL)) == (IPC_CREAT | IPC_EXCL))
        return -EEXIST;
      if (kind != 3 && size > (kind == 1 ? p->count : p->size))
        return -EINVAL;
      int r = access_obj(
          p,
          ((flags & 0777) | ((flags & 0777) >> 3) | ((flags & 0777) >> 6)) & 7);
      return r ? r : p->id;
    }
  }
  if (key && !(flags & IPC_CREAT))
    return -ENOENT;
  if (kind == 1 && (!size || size > 32000))
    return -EINVAL;
  if (kind == 2 && (!size || size > ULONG_MAX - (1UL << 24)))
    return -EINVAL;
  if (active >= (kind == 2 ? 4096 : 32000) ||
      (kind == 1 && total + size > 1024000000UL))
    return -ENOSPC;
  int index = 0;
  while (index < (kind == 2 ? 4096 : 32000) && used[index])
    index++;
  int id;
  do {
    unsigned seq = (unsigned)shared->nextid;
    shared->nextid = (int)((seq + 1) & 65535);
    id = (int)((seq << 15) | (unsigned)index);
  } while (find(c, id));
  off o = reusable ? reusable : alloc(c, sizeof(struct obj));
  if (!o)
    return -ENOMEM;
  struct obj fresh = {0};
  struct obj *p = &fresh;
  p->id = id;
  p->index = index;
  p->perm.seq = (unsigned short)((unsigned)id >> 15);
  p->kind = kind;
  p->key = key;
  p->perm.key = key;
  p->perm.uid = p->perm.cuid = (unsigned)R(geteuid, 0, 0, 0, 0, 0, 0);
  p->perm.gid = p->perm.cgid = (unsigned)R(getegid, 0, 0, 0, 0, 0, 0);
  p->perm.mode = flags & 0777;
  p->ctime = now(CLOCK_REALTIME) / 1000000000L;
  p->cpid = kind == 2 ? (int)R(getpid, 0, 0, 0, 0, 0, 0) : 0;
  p->count = kind == 1 ? size : 0;
  p->size = kind == 1 && reusable ? reused_capacity : size;
  p->qbytes = 16384;
  if (kind == 2) {
    if ((uint64_t)size + 4095 > (uint64_t)INT64_MAX)
      return -ENOMEM;
    char path[4096];
    segment_path(path, p->id);
    int fd =
        (int)R(openat, AT_FDCWD, path,
               O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600, 0, 0);
    if (fd < 0)
      return fd;
    long r = R(ftruncate, fd, (size + 4095) & ~4095UL, 0, 0, 0, 0);
    R(close, fd, 0, 0, 0, 0, 0);
    if (r)
      return r;
  }
  if (kind == 1) {
    off data = reusable ? reused_data : alloc(c, size * sizeof(struct cell));
    if (!data)
      return -ENOMEM;
    p->data = data;
    struct cell *v = ptr(c, data);
    for (size_t i = 0; i < size; i++) {
      struct cell zero = {0};
      v[i] = zero;
    }
  }
  p->next = reusable ? reused_next : shared->objects;
  *(struct obj *)ptr(c, o) = *p;
  if (!reusable)
    shared->objects = o;
  return p->id;
}
/* Safe copies use kernel pipe user-copy validation, not process_vm or libc TLS.
 */
static long copy(void *dst, const void *src, size_t n) {
  int fds[2];
  long r = R(pipe2, fds, O_CLOEXEC | O_NONBLOCK, 0, 0, 0, 0);
  if (r)
    return r;
  size_t at = 0;
  while (at < n) {
    size_t step = n - at;
    if (step > 4096)
      step = 4096;
    r = R(write, fds[1], (const char *)src + at, step, 0, 0, 0);
    if (r != (long)step) {
      r = r < 0 ? r : -EFAULT;
      break;
    }
    r = R(read, fds[0], (char *)dst + at, step, 0, 0, 0);
    if (r != (long)step) {
      r = r < 0 ? r : -EFAULT;
      break;
    }
    at += step;
    r = 0;
  }
  R(close, fds[0], 0, 0, 0, 0, 0);
  R(close, fds[1], 0, 0, 0, 0, 0);
  return r;
}
static long ctl(struct ctx *c, struct obj *p, int cmd, int num,
                unsigned long arg) {
  if (cmd == IPC_RMID) {
    int r = owner(p);
    if (r)
      return r;
    if (p->kind == 3) {
      off m = p->messages;
      while (m) {
        struct msg *q = ptr(c, m);
        off next = q->next;
        q->next = shared->free_messages;
        shared->free_messages = m;
        m = next;
      }
      p->messages = 0;
      p->count = p->bytes = 0;
    }
    p->removed = p->kind == 2 && p->count ? 1 : 2;
    p->key = 0;
    p->perm.key = 0;
    wake();
    return 0;
  }
  if (cmd == IPC_SET) {
    int r = owner(p);
    if (r)
      return r;
    struct proxy_ipc_ds d;
    r=(int)arch_ipc_ds_read(p->kind,(void *)arg,&d,copy);
    struct proxy_ipc_perm perm=d.perm;
    uint64_t qbytes=d.qbytes;
    if(p->kind==3 && qbytes>16384)return -EPERM;
    if (r)
      return r;
    if (perm.uid == UINT_MAX || perm.gid == UINT_MAX)
      return -EINVAL;
    p->perm.uid = perm.uid;
    p->perm.gid = perm.gid;
    p->perm.mode = (p->perm.mode & ~0777) | (perm.mode & 0777);
    if (p->kind == 3)
      p->qbytes = qbytes;
    p->ctime = now(CLOCK_REALTIME) / 1000000000L;
    wake();
    return 0;
  }
  int indexed = (p->kind == 1 && (cmd == SEM_STAT || cmd == SEM_STAT_ANY)) ||
                (p->kind == 2 && (cmd == SHM_STAT || cmd == SHM_STAT_ANY)) ||
                (p->kind == 3 && (cmd == MSG_STAT || cmd == MSG_STAT_ANY));
  int any = (p->kind == 1 && cmd == SEM_STAT_ANY) ||
            (p->kind == 2 && cmd == SHM_STAT_ANY) ||
            (p->kind == 3 && cmd == MSG_STAT_ANY);
  if (indexed)
    cmd = IPC_STAT;
  int r = any ? 0 : access_obj(p, cmd == SETVAL || cmd == SETALL ? 2 : 4);
  if (r)
    return r;
  if (cmd == IPC_STAT) {
    struct proxy_ipc_ds d={.perm=p->perm,.count=p->count,.size=p->size,
      .bytes=p->bytes,.qbytes=p->qbytes,.ctime=p->ctime,.atime=p->atime,
      .dtime=p->dtime,.cpid=p->cpid,.lpid=p->lpid};
    if(p->kind==2 && p->removed)d.perm.mode|=01000;
    long copied=arch_ipc_ds_write(p->kind,(void *)arg,&d,copy);
    return copied?copied:indexed?p->id:0;
  }
  if (p->kind == 2 && (cmd == SHM_LOCK || cmd == SHM_UNLOCK))
    return -ENOSYS;
  if (p->kind != 1)
    return -EINVAL;
  struct cell *v = cells(c, p);
  if (cmd == GETALL || cmd == SETALL) {
    unsigned short vals[32000];
    if (cmd == SETALL) {
      r = (int)copy(vals, (void *)arg, p->count * 2);
      if (r)
        return r;
      for (size_t i = 0; i < p->count; i++)
        if (vals[i] > MAXSEM)
          return -ERANGE;
      for (size_t i = 0; i < p->count; i++) {
        v[i].value = vals[i];
        v[i].pid = (int)R(getpid, 0, 0, 0, 0, 0, 0);
      }
    } else {
      for (size_t i = 0; i < p->count; i++)
        vals[i] = (unsigned short)v[i].value;
      return copy((void *)arg, vals, p->count * 2);
    }
  } else {
    if (num < 0 || (unsigned)num >= p->count)
      return -EINVAL;
    switch (cmd) {
    case GETVAL:
      return v[num].value;
    case GETPID:
      return v[num].pid;
    case GETNCNT:
      return v[num].ncnt;
    case GETZCNT:
      return v[num].zcnt;
    case SETVAL:
      if ((int)arg < 0 || arg > MAXSEM)
        return -ERANGE;
      v[num].value = (int)arg;
      v[num].pid = (int)R(getpid, 0, 0, 0, 0, 0, 0);
      break;
    default:
      return -EINVAL;
    }
  }
  for (off o = shared->records; o;) {
    struct rec *q = ptr(c, o);
    if (q->type == 1 && q->id == p->id && (cmd == SETALL || q->index == num))
      q->value = 0;
    o = q->next;
  }
  p->ctime = now(CLOCK_REALTIME) / 1000000000L;
  wake();
  return 0;
}
static long semops(struct ctx *c, struct obj *p, proxy_arg_t *a, off *waitrec) {
  unsigned long n = a[2];
  if (!n)
    return -EINVAL;
  if (n > 500)
    return -E2BIG;
  struct sembuf ops[500];
  int trial[32000];
  long r = copy(ops, (void *)(uintptr_t)a[1], n * sizeof(*ops));
  if (r)
    return r;
  int mode = 4;
  for (size_t i = 0; i < n; i++)
    if (ops[i].sem_op)
      mode = 2;
  r = access_obj(p, mode);
  if (r)
    return r;
  struct cell *v = cells(c, p);
  for (size_t i = 0; i < p->count; i++)
    trial[i] = v[i].value;
  for (size_t i = 0; i < n; i++) {
    unsigned k = ops[i].sem_num;
    int delta = ops[i].sem_op;
    if (k >= p->count)
      return -EFBIG;
    if (trial[k] + delta > MAXSEM)
      return -ERANGE;
    if ((delta == 0 && trial[k] != 0) || trial[k] + delta < 0) {
      if (ops[i].sem_flg & IPC_NOWAIT)
        return -EAGAIN;
      int id = p->id;
      off wr = record(c, 3, id, (int)k);
      if (!wr)
        return -ENOMEM;
      p = find(c, id);
      v = cells(c, p);
      struct rec *q = ptr(c, wr);
      q->value = delta == 0;
      if (delta == 0)
        v[k].zcnt++;
      else
        v[k].ncnt++;
      *waitrec = wr;
      return AGAIN;
    }
    trial[k] += delta;
  }
  /* Allocate undo entries before committing any semaphore values. */
  int id = p->id;
  for (size_t i = 0; i < n; i++)
    if (ops[i].sem_flg & SEM_UNDO) {
      off u = record(c, 1, id, ops[i].sem_num);
      if (!u)
        return -ENOMEM;
    }
  /* Validate cumulative undo adjustment per semaphore. */
  for (size_t i = 0; i < n; i++)
    if (ops[i].sem_flg & SEM_UNDO) {
      off u = record(c, 1, id, ops[i].sem_num);
      struct rec *q = ptr(c, u);
      long adj = q->value;
      for (size_t j = 0; j < n; j++)
        if ((ops[j].sem_flg & SEM_UNDO) && ops[j].sem_num == ops[i].sem_num)
          adj -= ops[j].sem_op;
      if (adj < -MAXSEM || adj > MAXSEM)
        return -ERANGE;
    }
  for (size_t i = 0; i < n; i++)
    if (ops[i].sem_flg & SEM_UNDO) {
      off u = record(c, 1, id, ops[i].sem_num);
      ((struct rec *)ptr(c, u))->value -= ops[i].sem_op;
    }
  p = find(c, id);
  v = cells(c, p);
  for (size_t i = 0; i < p->count; i++)
    v[i].value = trial[i];
  for (size_t i = 0; i < n; i++)
    v[ops[i].sem_num].pid = (int)R(getpid, 0, 0, 0, 0, 0, 0);
  p->atime = now(CLOCK_REALTIME) / 1000000000L;
  wake();
  return 0;
}
static long info(struct ctx *c, int kind, int cmd, void *out) {
  int high = 0, used = 0;
  unsigned long total = 0, bytes = 0, pages = 0;
  for (off o = shared->objects; o;) {
    struct obj *p = ptr(c, o);
    o = p->next;
    if (p->kind == kind && p->removed != 2) {
      used++;
      if (p->index > high)
        high = p->index;
      total += p->count;
      bytes += p->bytes;
      pages += (p->size + 4095) / 4096;
    }
  }
  long r;
  if (kind == 1) {
    struct seminfo i = {0};
    i.semmap = 1024000000;
    i.semmni = 32000;
    i.semmns = 1024000000;
    i.semmnu = 1024000000;
    i.semmsl = 32000;
    i.semopm = 500;
    i.semume = 500;
    i.semusz = cmd == SEM_INFO ? used : 20;
    i.semvmx = MAXSEM;
    i.semaem = cmd == SEM_INFO ? (int)total : MAXSEM;
    r = copy(out, &i, sizeof(i));
  } else if (kind == 2) {
    if (cmd == SHM_INFO) {
      struct shm_info i = {0};
      i.used_ids = used;
      i.shm_tot = pages;
      r = copy(out, &i, sizeof(i));
    } else {
      struct shminfo64 i = {0};
      i.shmmax = ULONG_MAX - (1UL << 24);
      i.shmmin = 1;
      i.shmmni = 4096;
      i.shmall = ULONG_MAX - (1UL << 24);
      r = copy(out, &i, sizeof(i));
    }
  } else {
    struct msginfo i = {0};
    i.msgmax = 8192;
    i.msgmnb = 16384;
    i.msgmni = 32000;
    i.msgpool = cmd == MSG_INFO ? used : 512000;
    i.msgmap = cmd == MSG_INFO ? (int)total : 16384;
    i.msgtql = cmd == MSG_INFO ? (int)bytes : 16384;
    i.msgssz = 16;
    i.msgseg = 65535;
    r = copy(out, &i, sizeof(i));
  }
  return r ? r : high;
}
static long dispatch(struct ctx *c, long nr, proxy_arg_t *a, off *wr) {
  if (nr == PROXY_SC_semget)
    return get(c, 1, (int)a[0], a[1], (int)a[2]);
  if (nr == PROXY_SC_shmget) {
    if (a[2] & SHM_HUGETLB)
      return -EOPNOTSUPP;
    return get(c, 2, (int)a[0], a[1], (int)a[2]);
  }
  if (nr == PROXY_SC_msgget)
    return get(c, 3, (int)a[0], 0, (int)a[1]);
  if (nr == PROXY_SC_shmdt) {
    int pid = (int)R(getpid, 0, 0, 0, 0, 0, 0);
    for (off o = shared->records; o;) {
      struct rec *q = ptr(c, o);
      o = q->next;
      if (q->type == 2 && q->pid == pid && q->addr == (unsigned long)a[0]) {
        long r = R(munmap, a[0], q->len, 0, 0, 0, 0);
        if (r)
          return r;
        struct obj *p = find(c, q->id);
        q->type = 0;
        struct local_attach **link = &attached;
        while (*link) {
          if ((*link)->addr == (unsigned long)a[0]) {
            struct local_attach *old = *link;
            *link = old->next;
            vfs_free(old);
            break;
          }
          link = &(*link)->next;
        }
        if (p) {
          p->count--;
          p->dtime = now(CLOCK_REALTIME) / 1000000000L;
          p->lpid = pid;
          if (p->removed && !p->count)
            p->removed = 2;
        }
        return 0;
      }
    }
    return -EINVAL;
  }
  int kind = nr == PROXY_SC_semop || nr == PROXY_SC_semtimedop || nr == PROXY_SC_semctl ? 1
             : nr == PROXY_SC_shmat || nr == PROXY_SC_shmctl                       ? 2
                                                                         : 3;
  int cmd = (int)a[kind == 1 ? 2 : 1];
  if (nr == PROXY_SC_semctl || nr == PROXY_SC_shmctl || nr == PROXY_SC_msgctl) {
    if (cmd == IPC_INFO || (kind == 1 && cmd == SEM_INFO) ||
        (kind == 2 && cmd == SHM_INFO) || (kind == 3 && cmd == MSG_INFO))
      return info(c, kind, cmd, (void *)(uintptr_t)a[kind == 1 ? 3 : 2]);
  }
  struct obj *p = 0;
  int indexed =
      (nr == PROXY_SC_semctl && (cmd == SEM_STAT || cmd == SEM_STAT_ANY)) ||
      (nr == PROXY_SC_shmctl && (cmd == SHM_STAT || cmd == SHM_STAT_ANY)) ||
      (nr == PROXY_SC_msgctl && (cmd == MSG_STAT || cmd == MSG_STAT_ANY));
  if (indexed) {
    for (off o = shared->objects; o;) {
      struct obj *q = ptr(c, o);
      o = q->next;
      if (q->kind == kind && q->removed != 2 && q->index == (int)a[0]) {
        p = q;
        break;
      }
    }
  } else
    p = find(c, (int)a[0]);
  if (!p || p->kind != kind)
    return -EINVAL;
  if (nr == PROXY_SC_semop || nr == PROXY_SC_semtimedop)
    return semops(c, p, a, wr);
  if (nr == PROXY_SC_semctl || nr == PROXY_SC_shmctl || nr == PROXY_SC_msgctl)
    return ctl(c, p, cmd, (int)a[1], a[kind == 1 ? 3 : 2]);
  if (nr == PROXY_SC_shmat) {
    int flags = (int)a[2];
    long addr = a[1];
    if (flags & ~(SHM_RDONLY | SHM_RND | SHM_REMAP | SHM_EXEC))
      return -EINVAL;
    if (addr & 4095) {
      if (!(flags & SHM_RND))
        return -EINVAL;
      addr &= ~4095L;
    }
    if ((flags & SHM_REMAP) && !addr)
      return -EINVAL;
    int r = access_obj(p, ((flags & SHM_RDONLY) ? 4 : 6) |
                              ((flags & SHM_EXEC) ? 1 : 0));
    if (r)
      return r;
    int id = p->id;
    off o = objoff(c, p);
    size_t len = (p->size + 4095) & ~4095UL;
    if (flags & SHM_REMAP)
      for (struct local_attach *la = attached; la; la = la->next)
        if (la->addr < (unsigned long)addr + len &&
            la->addr + la->len > (unsigned long)addr &&
            ((unsigned long)addr > la->addr ||
             (unsigned long)addr + len < la->addr + la->len))
          return -EOPNOTSUPP;
    char path[4096];
    segment_path(path, id);
    long segment =
        R(openat, AT_FDCWD, path,
          ((flags & SHM_RDONLY) ? O_RDONLY : O_RDWR) | O_NOFOLLOW | O_CLOEXEC,
          0, 0, 0);
    if (segment < 0)
      return segment;
    long mapped =
        R(mmap, addr, len,
          PROT_READ | ((flags & SHM_RDONLY) ? 0 : PROT_WRITE) |
              ((flags & SHM_EXEC) ? PROT_EXEC : 0),
          MAP_SHARED |
              (addr ? ((flags & SHM_REMAP) ? MAP_FIXED : MAP_FIXED_NOREPLACE)
                    : 0),
          segment, 0);
    R(close, segment, 0, 0, 0, 0, 0);
    if ((unsigned long)mapped >= (unsigned long)-4095)
      return mapped;
    if (flags & SHM_REMAP) {
      struct local_attach **link = &attached;
      int pid = (int)R(getpid, 0, 0, 0, 0, 0, 0);
      while (*link) {
        struct local_attach *old = *link;
        if (old->addr < (unsigned long)mapped + len &&
            old->addr + old->len > (unsigned long)mapped) {
          for (off ro = shared->records; ro;) {
            struct rec *q = ptr(c, ro);
            ro = q->next;
            if (q->type == 2 && q->pid == pid && q->addr == old->addr) {
              struct obj *prior = find(c, q->id);
              q->type = 0;
              if (prior) {
                prior->count--;
                if (prior->removed && !prior->count)
                  prior->removed = 2;
              }
              break;
            }
          }
          *link = old->next;
          vfs_free(old);
        } else
          link = &old->next;
      }
    }
    /* Each attach gets its own record, including repeated attaches. */
    off rr = new_record(c);
    if (!rr) {
      R(munmap, mapped, len, 0, 0, 0, 0);
      return -ENOMEM;
    }
    struct rec *q = ptr(c, rr);
    q->type = 2;
    q->pid = (int)R(getpid, 0, 0, 0, 0, 0, 0);
    q->birth = identity(q->pid, 1);
    q->id = id;
    q->addr = (uintptr_t)mapped;
    q->len = len;
    p = ptr(c, o);
    p->count++;
    p->atime = now(CLOCK_REALTIME) / 1000000000L;
    p->lpid = q->pid;
    struct local_attach *la = vfs_alloc(sizeof(*la));
    if (!la) {
      q->type = 0;
      p->count--;
      R(munmap, mapped, len, 0, 0, 0, 0);
      return -ENOMEM;
    }
    la->addr = (uintptr_t)mapped;
    la->len = len;
    la->id = id;
    la->next = attached;
    attached = la;
    return mapped;
  }
  if (nr == PROXY_SC_msgsnd) {
    if ((unsigned long)a[2] > 8192)
      return -EINVAL;
    long type;
    long r = copy(&type, (void *)(uintptr_t)a[1], sizeof(type));
    if (r)
      return r;
    if (type <= 0)
      return -EINVAL;
    r = access_obj(p, 2);
    if (r)
      return r;
    if (p->bytes > p->qbytes || (unsigned long)a[2] > p->qbytes - p->bytes ||
        p->count >= p->qbytes)
      return (a[3] & IPC_NOWAIT) ? -EAGAIN : AGAIN;
    off po = objoff(c, p), m = 0;
    off *spare = &shared->free_messages;
    while (*spare) {
      struct msg *old = ptr(c, *spare);
      if (old->capacity >= (size_t)a[2]) {
        m = *spare;
        *spare = old->next;
        old->next = 0;
        break;
      }
      spare = &old->next;
    }
    int reused = m != 0;
    if (!m)
      m = alloc(c, sizeof(struct msg) + a[2]);
    if (!m)
      return -ENOMEM;
    struct msg *q = ptr(c, m);
    q->type = type;
    q->len = a[2];
    if (!reused)
      q->capacity = a[2];
    r = copy(q + 1, (char *)(uintptr_t)a[1] + sizeof(long), a[2]);
    if (r) {
      q->next = shared->free_messages;
      shared->free_messages = m;
      return r;
    }
    p = ptr(c, po);
    off *tail = &p->messages;
    while (*tail)
      tail = &((struct msg *)ptr(c, *tail))->next;
    *tail = m;
    p->count++;
    p->bytes += a[2];
    p->atime = now(CLOCK_REALTIME) / 1000000000L;
    p->cpid = (int)R(getpid, 0, 0, 0, 0, 0, 0);
    wake();
    return 0;
  }
  if (nr == PROXY_SC_msgrcv) {
    if (a[2] < 0)
      return -EINVAL;
    int r = access_obj(p, 4);
    if (r)
      return r;
    if (a[4] & 040000)
      return -ENOSYS;
    off selected = 0, *link = &p->messages, *chosen = 0;
    int64_t wanted = a[3], best = INT64_MAX;
    for (off m = *link; m;) {
      struct msg *q = ptr(c, m);
      int match = wanted == 0 ||
                  (wanted > 0 ? ((a[4] & MSG_EXCEPT) ? q->type != wanted
                                                     : q->type == wanted)
                              : ((wanted == INT64_MIN || q->type <= -wanted) &&
                                 q->type < best));
      if (match) {
        selected = m;
        chosen = link;
        best = q->type;
        if (wanted >= 0)
          break;
      }
      link = &q->next;
      m = *link;
    }
    if (!selected)
      return (a[4] & IPC_NOWAIT) ? -ENOMSG : AGAIN;
    struct msg *q = ptr(c, selected);
    if (q->len > (unsigned long)a[2] && !(a[4] & MSG_NOERROR))
      return -E2BIG;
    size_t n = q->len;
    if (n > (unsigned long)a[2])
      n = a[2];
    if(q->type>LONG_MAX)return -EOVERFLOW;
    long wire_type=q->type;
    r = (int)copy((void *)(uintptr_t)a[1], &wire_type, sizeof(wire_type));
    if (!r)
      r = (int)copy((char *)(uintptr_t)a[1] + sizeof(long), q + 1, n);
    *chosen = q->next;
    q->next = shared->free_messages;
    shared->free_messages = selected;
    p->count--;
    p->bytes -= q->len;
    p->dtime = now(CLOCK_REALTIME) / 1000000000L;
    p->lpid = (int)R(getpid, 0, 0, 0, 0, 0, 0);
    wake();
    return r ? r : (long)n;
  }
  return -ENOSYS;
}
int ipc_ours(long n) {
  switch (n) {
  case PROXY_SC_semget:
  case PROXY_SC_semop:
  case PROXY_SC_semtimedop:
  case PROXY_SC_semtimedop_time64:
  case PROXY_SC_semctl:
  case PROXY_SC_shmget:
  case PROXY_SC_shmat:
  case PROXY_SC_shmdt:
  case PROXY_SC_shmctl:
  case PROXY_SC_msgget:
  case PROXY_SC_msgsnd:
  case PROXY_SC_msgrcv:
  case PROXY_SC_msgctl:
    return 1;
  default:
    return 0;
  }
}
long ipc_call(long nr, proxy_arg_t *input) {
  int time64=nr==PROXY_SC_semtimedop_time64;
  if(time64)nr=PROXY_SC_semtimedop;
  proxy_arg_t args[6];
  p_copy(args, input, sizeof(args));
  proxy_arg_t *a = args;
  struct sembuf operations[500];
  unsigned char message[sizeof(long) + 8192];
  if (nr == PROXY_SC_semop || nr == PROXY_SC_semtimedop) {
    if (a[2] <= 0)
      return a[2] ? -E2BIG : -EINVAL;
    if (a[2] > 500)
      return -E2BIG;
    long r = copy(operations, (void *)(uintptr_t)a[1], a[2] * sizeof(*operations));
    if (r)
      return r;
    a[1] = (long)operations;
  }
  if (nr == PROXY_SC_msgsnd) {
    if ((unsigned long)a[2] > 8192)
      return -EINVAL;
    long r = copy(message, (void *)(uintptr_t)a[1], sizeof(long) + a[2]);
    if (r)
      return r;
    a[1] = (long)message;
  }
  ipc_fork();
  if (!shared)
    return init_error ? init_error : -ENOSYS;
  int64_t deadline = 0;
  if (nr == PROXY_SC_semtimedop && a[3]) {
    struct proxy_timespec t;
    long r = arch_timeout_read((void *)(uintptr_t)a[3],time64,&t,copy);
    if (r)
      return r;
    if (t.sec < 0 || t.nsec < 0 || t.nsec >= 1000000000)
      return -EINVAL;
    int64_t start = now(CLOCK_MONOTONIC);
    deadline = t.sec > (INT64_MAX - start - t.nsec) / 1000000000L
                   ? INT64_MAX
                   : start + t.sec * 1000000000L + t.nsec;
  }
  int waited = 0;
  for (;;) {
    lock();
    struct ctx c = {.fd = (int)R(openat, AT_FDCWD, arena_path,
                                 O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0, 0, 0)};
    long result = c.fd < 0 ? c.fd : remap(&c);
    off wr = 0;
    unsigned seq = shared->seq;
    if (result >= 0) {
      result = begin(&c);
      if (!result) {
        reap(&c, 0);
        if (waited && !find(&c, (int)a[0]))
          result = -EIDRM;
        else
          result = dispatch(&c, nr, a, &wr);
        seq = shared->seq;
        commit();
        cleanup_segments(&c);
      }
    }
    if (c.base)
      R(munmap, c.base, c.size, 0, 0, 0, 0);
    if (c.fd >= 0)
      R(close, c.fd, 0, 0, 0, 0, 0);
    unlock();
    if (result != AGAIN)
      return result;
    waited = 1;
    int64_t remaining = deadline ? deadline - now(CLOCK_MONOTONIC) : 10000000L;
    long waitresult = 0;
    if (remaining > 0) {
      if (remaining > 10000000)
        remaining = 10000000;
      struct timespec t = {0, remaining};
      waitresult = R(futex, &shared->seq, FUTEX_WAIT, seq, &t, 0, 0);
    }
    if (wr) {
      lock();
      struct ctx d = {.fd = (int)R(openat, AT_FDCWD, arena_path,
                                   O_RDWR | O_CLOEXEC, 0, 0, 0)};
      if (d.fd >= 0 && remap(&d) >= 0 && begin(&d) >= 0) {
        struct rec *q = ptr(&d, wr);
        struct obj *p = find(&d, q->id);
        if (p && q->type == 3) {
          struct cell *v = cells(&d, p);
          if (q->value)
            v[q->index].zcnt--;
          else
            v[q->index].ncnt--;
        }
        q->type = 0;
        commit();
      }
      if (d.base)
        R(munmap, d.base, d.size, 0, 0, 0, 0);
      if (d.fd >= 0)
        R(close, d.fd, 0, 0, 0, 0, 0);
      unlock();
    }
    if (waitresult == -EINTR)
      return -EINTR;
    if (deadline && now(CLOCK_MONOTONIC) >= deadline)
      return -EAGAIN;
  }
}
void ipc_exit(void) {
  ipc_fork();
  if (!shared)
    return;
  lock();
  struct ctx c = {
      .fd = (int)R(openat, AT_FDCWD, arena_path, O_RDWR | O_CLOEXEC, 0, 0, 0)};
  if (c.fd >= 0 && remap(&c) >= 0 && begin(&c) >= 0) {
    reap(&c, 1);
    commit();
  }
  if (c.base)
    R(munmap, c.base, c.size, 0, 0, 0, 0);
  if (c.fd >= 0)
    R(close, c.fd, 0, 0, 0, 0, 0);
  unlock();
}
/* Fork/exec lifecycle integration is intentionally separate from dispatch. */
void ipc_fork(void) {
  if (pending.owner && (int)R(getpid, 0, 0, 0, 0, 0, 0) != pending.owner) {
    ipc_fork_result(0);
    return;
  }
  int pid = (int)R(getpid, 0, 0, 0, 0, 0, 0);
  if (!shared || pid == local_pid)
    return;
  local_pid = pid;
  lock();
  struct ctx c = {
      .fd = (int)R(openat, AT_FDCWD, arena_path, O_RDWR | O_CLOEXEC, 0, 0, 0)};
  if (c.fd >= 0 && remap(&c) >= 0 && begin(&c) >= 0) {
    for (struct local_attach *a = attached; a; a = a->next) {
      off o = new_record(&c);
      if (!o)
        break;
      struct rec *q = ptr(&c, o);
      q->type = 2;
      q->pid = pid;
      q->birth = identity(pid, 1);
      q->id = a->id;
      q->addr = a->addr;
      q->len = a->len;
      struct obj *p = find(&c, a->id);
      if (p)
        p->count++;
    }
    commit();
  }
  if (c.base)
    R(munmap, c.base, c.size, 0, 0, 0, 0);
  if (c.fd >= 0)
    R(close, c.fd, 0, 0, 0, 0, 0);
  unlock();
}
void ipc_exec(void) {
  ipc_fork();
  while (attached) {
    proxy_arg_t a[6] = {(long)attached->addr, 0, 0, 0, 0, 0};
    if (ipc_call(PROXY_SC_shmdt, a) < 0)
      break;
  }
}

void ipc_thread_exit(void) {
  if (shared && task_count((int)R(getpid, 0, 0, 0, 0, 0, 0)) == 1)
    ipc_exit();
}

/* Reserve inherited attachments before the kernel copies the address space.
 * The short-held mutex serializes mapping changes through clone. The ticket's
 * flock is inherited by the child, so a dead parent cannot make an unscheduled
 * child's reservation disappear. An unclaimed ticket with no holders is reaped.
 */
int ipc_fork_prepare(void) {
  if (!shared)
    return 0;
  lock();
  if (!attached) {
    unlock();
    return 0;
  }
  struct ctx c = {
      .fd = (int)R(openat, AT_FDCWD, arena_path, O_RDWR | O_CLOEXEC, 0, 0, 0)};
  long r = c.fd < 0 ? c.fd : remap(&c);
  int ticketfd = -1;
  off ticket = 0;
  if (!r)
    r = begin(&c);
  if (!r) {
    ticket = ++shared->fork_sequence;
    char path[4096];
    ticket_path(path, ticket);
    ticketfd =
        (int)R(openat, AT_FDCWD, path,
               O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600, 0, 0);
    if (ticketfd < 0)
      r = ticketfd;
    else
      r = R(flock, ticketfd, LOCK_EX | LOCK_NB, 0, 0, 0, 0);
    if (!r)
      for (struct local_attach *a = attached; a; a = a->next) {
        off o = new_record(&c);
        if (!o) {
          r = -ENOMEM;
          break;
        }
        struct rec *q = ptr(&c, o);
        q->type = 6;
        q->pid = (int)R(getpid, 0, 0, 0, 0, 0, 0);
        q->birth = identity(q->pid, 1);
        q->id = a->id;
        q->addr = a->addr;
        q->len = a->len;
        q->value = (long)ticket;
        struct obj *p = find(&c, a->id);
        if (p)
          p->count++;
      }
    if (r) {
      if (recover() < 0)
        shared->poisoned = 1;
    } else
      commit();
  }
  if (!r) {
    pending.context = c;
    pending.owner = (int)R(getpid, 0, 0, 0, 0, 0, 0);
    pending.tid = (int)R(gettid, 0, 0, 0, 0, 0, 0);
    pending.ticket_fd = ticketfd;
    pending.ticket = ticket;
    return 0;
  }
  if (ticketfd >= 0) {
    R(close, ticketfd, 0, 0, 0, 0, 0);
    char path[4096];
    ticket_path(path, ticket);
    R(unlinkat, AT_FDCWD, path, 0, 0, 0, 0);
  }
  if (c.base)
    R(munmap, c.base, c.size, 0, 0, 0, 0);
  if (c.fd >= 0)
    R(close, c.fd, 0, 0, 0, 0, 0);
  unlock();
  return (int)r;
}
void ipc_fork_result(long result) {
  int pid = (int)R(getpid, 0, 0, 0, 0, 0, 0),
      tid = (int)R(gettid, 0, 0, 0, 0, 0, 0);
  if (!pending.owner)
    return;
  int child = pid != pending.owner;
  if (!child && (tid != pending.tid || !result))
    return;
  struct ctx c = pending.context;
  off ticket = pending.ticket;
  int ticketfd = pending.ticket_fd;
  if (child)
    lock();
  long error = remap(&c);
  if (!error)
    error = begin(&c);
  if (!error) {
    for (off o = shared->records; o;) {
      struct rec *q = ptr(&c, o);
      o = q->next;
      if (q->type != 6 || (off)q->value != ticket)
        continue;
      if (result < 0) {
        struct obj *p = find(&c, q->id);
        if (p && p->count)
          p->count--;
        q->type = 0;
      } else {
        q->type = 2;
        q->pid = child ? pid : (int)result;
        q->birth = identity(q->pid, 1);
      }
    }
    commit();
    cleanup_segments(&c);
  } else
    shared->poisoned = 1;
  if (child)
    local_pid = pid;
  pending.owner = 0;
  R(close, ticketfd, 0, 0, 0, 0, 0);
  char path[4096];
  ticket_path(path, ticket);
  R(unlinkat, AT_FDCWD, path, 0, 0, 0, 0);
  if (c.base)
    R(munmap, c.base, c.size, 0, 0, 0, 0);
  if (c.fd >= 0)
    R(close, c.fd, 0, 0, 0, 0, 0);
  unlock();
}
int ipc_private_fd(int fd) {
  return pending.owner && (fd == pending.ticket_fd || fd == pending.context.fd);
}

#include "state.h"
void ipc_state(struct state *s){
 uint64_t present=state_word(s,shared!=NULL);if(!present)return;
 state_bytes(s,arena_path,sizeof(arena_path));state_bytes(s,journal_path,sizeof(journal_path));
 if(arena_path[sizeof(arena_path)-1]||journal_path[sizeof(journal_path)-1]){s->error=-EINVAL;return;}
 if(!s->reading)return;
 long fd=R(openat,AT_FDCWD,arena_path,O_RDWR|O_NOFOLLOW|O_CLOEXEC,0,0,0);
 if(fd<0){s->error=fd;return;}
 long m=R(mmap,0,PAGE,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);R(close,fd,0,0,0,0,0);
 if((unsigned long)m>=(unsigned long)-4095){s->error=m;return;}
 shared=(void *)m;if(shared->format!=2){s->error=-EPROTO;return;}
 /* The kernel PID/birth identity retains SEM_UNDO records. Kernel exec
  * unmapped attachments; import below removes their accounting records. */
 local_pid=R(getpid,0,0,0,0,0,0);
 /* Kernel exec already unmapped attachments. Remove accounting records without
  * munmap of old addresses (which could now belong to the new loader). */
 lock();
 struct ctx c={.fd=(int)R(openat,AT_FDCWD,arena_path,O_RDWR|O_CLOEXEC|O_NOFOLLOW,0,0,0)};
 long result=c.fd<0?c.fd:remap(&c);
 if(result>=0 && (result=begin(&c))>=0){
  uint64_t token=identity(local_pid,1);
  for(off o=shared->records;o;){struct rec *q=ptr(&c,o);o=q->next;
   if(q->type==2 && q->pid==local_pid && q->birth==token){struct obj *p=find(&c,q->id);if(p){
    if(p->count)p->count--;
    p->dtime=now(CLOCK_REALTIME)/1000000000L;p->lpid=local_pid;
    if(p->removed && !p->count)p->removed=2;
   }q->type=0;}
  }
  commit();cleanup_segments(&c);
 }
 if(c.base)R(munmap,c.base,c.size,0,0,0,0);
 if(c.fd>=0)R(close,c.fd,0,0,0,0,0);
 unlock();if(result<0)s->error=result;
}
