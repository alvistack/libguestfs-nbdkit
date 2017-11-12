/* nbdkit
 * Copyright (C) 2017 Red Hat Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of Red Hat nor the names of its contributors may be
 * used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY RED HAT AND CONTRIBUTORS ''AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL RED HAT OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <config.h>

#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <assert.h>

#include <nbdkit-plugin.h>
#include "protocol.h"

static char *sockname = NULL;
static char *export = NULL;

static void
nbd_unload (void)
{
  free (sockname);
  free (export);
}

/* Called for each key=value passed on the command line.  This plugin
 * accepts socket=<sockname> (required for now) and export=<name> (optional).
 */
static int
nbd_config (const char *key, const char *value)
{
  if (strcmp (key, "socket") == 0) {
    /* See FILENAMES AND PATHS in nbdkit-plugin(3) */
    free (sockname);
    sockname = nbdkit_absolute_path (value);
    if (!sockname)
      return -1;
  }
  else if (strcmp (key, "export") == 0) {
    free (export);
    export = strdup (value);
    if (!export) {
      nbdkit_error ("memory failure: %m");
      return -1;
    }
  }
  else {
    nbdkit_error ("unknown parameter '%s'", key);
    return -1;
  }

  return 0;
}

/* Check the user did pass a socket=<SOCKNAME> parameter. */
static int
nbd_config_complete (void)
{
  struct sockaddr_un sock;

  if (sockname == NULL) {
    nbdkit_error ("you must supply the socket=<SOCKNAME> parameter after the plugin name on the command line");
    return -1;
  }
  if (strlen (sockname) >= sizeof sock.sun_path) {
    nbdkit_error ("socket file name too large");
    return -1;
  }
  if (!export)
    export = strdup ("");
  if (!export) {
    nbdkit_error ("memory failure: %m");
    return -1;
  }
  return 0;
}

#define nbd_config_help \
  "socket=<SOCKNAME>   (required) The Unix socket to connect to.\n" \
  "export=<NAME>                  Export name to connect to (default \"\").\n" \

/* TODO Allow more parallelism than one request at a time */
#define THREAD_MODEL NBDKIT_THREAD_MODEL_SERIALIZE_REQUESTS

/* The per-transaction details */
struct transaction {
  /* TODO: the protocol uses a 64-bit handle, but until we allow
     interleaved transactions, 31 bits with wraparound is plenty */
  int cookie;
  void *buf;
  uint32_t count;
};

/* The per-connection handle */
struct handle {
  int fd;
  int flags;
  int64_t size;
  /* Our choice of THREAD_MODEL means at most one outstanding transaction */
  struct transaction trans;
  bool dead;
};

/* Read an entire buffer, returning 0 on success or -1 with errno set. */
static int
read_full (int fd, void *buf, size_t len)
{
  ssize_t r;

  while (len) {
    r = read (fd, buf, len);
    if (r < 0) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      return -1;
    }
    if (!r) {
      /* Unexpected EOF */
      errno = EBADMSG;
      return -1;
    }
    buf += r;
    len -= r;
  }
  return 0;
}

/* Write an entire buffer, returning 0 on success or -1 with errno set. */
static int
write_full (int fd, const void *buf, size_t len)
{
  ssize_t r;

  while (len) {
    r = write (fd, buf, len);
    if (r < 0) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      return -1;
    }
    buf += r;
    len -= r;
  }
  return 0;
}

/* Called during transmission phases when there is no hope of
 * resynchronizing with the server, and all further requests from the
 * client will fail.  Returns -1 for convenience. */
static int
nbd_mark_dead (struct handle *h)
{
  int err = errno;

  if (!h->dead) {
    nbdkit_debug ("permanent failure while talking to server %s: %m",
                  sockname);
    h->dead = true;
  }
  else if (!err)
    errno = ESHUTDOWN;
  /* NBD only accepts a limited set of errno values over the wire, and
     nbdkit converts all other values to EINVAL. If we died due to an
     errno value that cannot transmit over the wire, translate it to
     ESHUTDOWN instead.  */
  if (err == EPIPE || err == EBADMSG)
    nbdkit_set_error (ESHUTDOWN);
  return -1;
}

/* Send a request, return 0 on success or -1 on write failure. */
static int
nbd_request_raw (struct handle *h, uint32_t type, uint64_t offset,
                 uint32_t count, uint64_t cookie)
{
  struct request req = {
    .magic = htobe32 (NBD_REQUEST_MAGIC),
    /* TODO nbdkit should have a way to pass flags, separate from cmd type */
    .type = htobe32 (type),
    .handle = cookie, /* Opaque to server, so endianness doesn't matter */
    .offset = htobe64 (offset),
    .count = htobe32 (count),
  };

  nbdkit_debug ("sending request with type %d and cookie %#" PRIx64, type,
                cookie);
  return write_full (h->fd, &req, sizeof req);
}

/* Perform the request half of a transaction. On success, return the
   non-negative cookie to match to the reply; on error return -1. */
static int
nbd_request_full (struct handle *h, uint32_t type, uint64_t offset,
                  uint32_t count, const void *req_buf, void *rep_buf)
{
  if (h->dead)
    return nbd_mark_dead (h);
  h->trans.buf = rep_buf;
  h->trans.count = rep_buf ? count : 0;
  if (++h->trans.cookie > INT_MAX)
    h->trans.cookie = 1;
  if (nbd_request_raw (h, type, offset, count, h->trans.cookie) < 0)
    return nbd_mark_dead (h);
  if (req_buf && write_full (h->fd, req_buf, count) < 0)
    return nbd_mark_dead (h);
  return h->trans.cookie;
}

/* Shorthand for nbd_request_full when no extra buffers are involved. */
static int
nbd_request (struct handle *h, uint32_t type, uint64_t offset, uint32_t count)
{
  return nbd_request_full (h, type, offset, count, NULL, NULL);
}

/* Read a reply, and look up the corresponding transaction.  Return
   the server's non-negative answer (converted to local errno value)
   on success, or -1 on read failure. */
static int
nbd_reply_raw (struct handle *h, struct transaction *trans)
{
  struct reply rep;

  if (read_full (h->fd, &rep, sizeof rep) < 0)
    return nbd_mark_dead (h);
  *trans = h->trans;
  nbdkit_debug ("received reply for cookie %#" PRIx64, rep.handle);
  if (be32toh (rep.magic) != NBD_REPLY_MAGIC || rep.handle != trans->cookie)
    return nbd_mark_dead (h);
  switch (be32toh (rep.error)) {
  case NBD_SUCCESS:
    if (trans->buf && read_full (h->fd, trans->buf, trans->count) < 0)
      return nbd_mark_dead (h);
    return 0;
  case NBD_EPERM:
    return EPERM;
  case NBD_EIO:
    return EIO;
  case NBD_ENOMEM:
    return ENOMEM;
  default:
    nbdkit_debug ("unexpected error %d, squashing to EINVAL",
                  be32toh (rep.error));
    /* fallthrough */
  case NBD_EINVAL:
    return EINVAL;
  case NBD_ENOSPC:
    return ENOSPC;
  case NBD_ESHUTDOWN:
    /* The server wants us to initiate soft-disconnect.  Because our
       THREAD_MODEL does not permit interleaved requests, we know that
       there are no other pending outstanding messages, so we can
       attempt that immediately.

       TODO: Once we allow interleaved requests, handling
       soft-disconnect properly will be trickier */
    nbd_request_raw (h, NBD_CMD_DISC, 0, 0, 0);
    errno = ESHUTDOWN;
    return nbd_mark_dead (h);
  }
}

/* Perform the reply half of a transaction. */
static int
nbd_reply (struct handle *h, int cookie)
{
  int err;
  struct transaction trans;

  err = nbd_reply_raw (h, &trans);
  assert (err < 0 || cookie == trans.cookie);
  if (err > 0)
    errno = err;
  return err ? -1 : 0;
}

/* Create the per-connection handle. */
static void *
nbd_open (int readonly)
{
  struct handle *h;
  struct sockaddr_un sock = { .sun_family = AF_UNIX };
  struct old_handshake old;
  uint64_t version;

  h = calloc (1, sizeof *h);
  if (h == NULL) {
    nbdkit_error ("malloc: %m");
    return NULL;
  }
  h->fd = socket (AF_UNIX, SOCK_STREAM, 0);
  if (h->fd < 0) {
    nbdkit_error ("socket: %m");
    return NULL;
  }
  strncpy (sock.sun_path, sockname, sizeof (sock.sun_path));
  if (connect (h->fd, (const struct sockaddr *) &sock, sizeof sock) < 0) {
    nbdkit_error ("connect: %m");
    goto err;
  }

  /* old and new handshake share same meaning of first 16 bytes */
  if (read_full (h->fd, &old, offsetof (struct old_handshake, exportsize))) {
    nbdkit_error ("unable to read magic: %m");
    goto err;
  }
  if (strncmp(old.nbdmagic, "NBDMAGIC", sizeof old.nbdmagic)) {
    nbdkit_error ("wrong magic, %s is not an NBD server", sockname);
    goto err;
  }
  version = be64toh (old.version);
  if (version == OLD_VERSION) {
    if (read_full (h->fd,
                   (char *) &old + offsetof (struct old_handshake, exportsize),
                   sizeof old - offsetof (struct old_handshake, exportsize))) {
      nbdkit_error ("unable to read old handshake: %m");
      goto err;
    }
    h->size = be64toh (old.exportsize);
    h->flags = be16toh (old.eflags);
  }
  else if (version == NEW_VERSION) {
    uint16_t gflags;
    uint32_t cflags;
    struct new_option opt;
    struct new_handshake_finish finish;
    size_t expect;

    if (read_full (h->fd, &gflags, sizeof gflags)) {
      nbdkit_error ("unable to read global flags: %m");
      goto err;
    }
    cflags = htobe32(gflags & (NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES));
    if (write_full (h->fd, &cflags, sizeof cflags)) {
      nbdkit_error ("unable to return global flags: %m");
      goto err;
    }

    /* For now, we don't do any option haggling, but go straight into
       transmission phase */
    opt.version = htobe64 (NEW_VERSION);
    opt.option = htobe32 (NBD_OPT_EXPORT_NAME);
    opt.optlen = htobe32 (strlen (export));
    if (write_full (h->fd, &opt, sizeof opt) ||
        write_full (h->fd, export, strlen (export))) {
      nbdkit_error ("unable to request export '%s': %m", export);
      goto err;
    }
    expect = sizeof finish;
    if (gflags & NBD_FLAG_NO_ZEROES)
      expect -= sizeof finish.zeroes;
    if (read_full (h->fd, &finish, expect)) {
      nbdkit_error ("unable to read new handshake: %m");
      goto err;
    }
    h->size = be64toh (finish.exportsize);
    h->flags = be16toh (finish.eflags);
  }
  else {
    nbdkit_error ("unexpected version %#" PRIx64, version);
    goto err;
  }

  return h;

 err:
  close (h->fd);
  return NULL;
}

/* Free up the per-connection handle. */
static void
nbd_close (void *handle)
{
  struct handle *h = handle;

  if (!h->dead)
    nbd_request_raw (h, NBD_CMD_DISC, 0, 0, 0);
  close (h->fd);
  free (h);
}

/* Get the file size. */
static int64_t
nbd_get_size (void *handle)
{
  struct handle *h = handle;

  return h->size;
}

static int
nbd_can_write (void *handle)
{
  struct handle *h = handle;

  return !(h->flags & NBD_FLAG_READ_ONLY);
}

static int
nbd_can_flush (void *handle)
{
  struct handle *h = handle;

  return h->flags & NBD_FLAG_SEND_FLUSH;
}

static int
nbd_is_rotational (void *handle)
{
  struct handle *h = handle;

  return h->flags & NBD_FLAG_ROTATIONAL;
}

static int
nbd_can_trim (void *handle)
{
  struct handle *h = handle;

  return h->flags & NBD_FLAG_SEND_TRIM;
}

/* Read data from the file. */
static int
nbd_pread (void *handle, void *buf, uint32_t count, uint64_t offset)
{
  struct handle *h = handle;
  int c;

  /* TODO Auto-fragment this if the client has a larger max transfer
     limit than the server */
  c = nbd_request_full (h, NBD_CMD_READ, offset, count, NULL, buf);
  return c < 0 ? c : nbd_reply (h, c);
}

/* Write data to the file. */
static int
nbd_pwrite (void *handle, const void *buf, uint32_t count, uint64_t offset)
{
  struct handle *h = handle;
  int c;

  /* TODO Auto-fragment this if the client has a larger max transfer
     limit than the server */
  c = nbd_request_full (h, NBD_CMD_WRITE, offset, count, buf, NULL);
  return c < 0 ? c : nbd_reply (h, c);
}

/* Write zeroes to the file. */
static int
nbd_zero (void *handle, uint32_t count, uint64_t offset, int may_trim)
{
  struct handle *h = handle;
  uint32_t cmd = NBD_CMD_WRITE_ZEROES;
  int c;

  if (!(h->flags & NBD_FLAG_SEND_WRITE_ZEROES)) {
    /* Trigger a fall back to regular writing */
    errno = EOPNOTSUPP;
    return -1;
  }

  if (!may_trim)
    cmd |= NBD_CMD_FLAG_NO_HOLE;
  c = nbd_request (h, cmd, offset, count);
  return c < 0 ? c : nbd_reply (h, c);
}

/* Trim a portion of the file. */
static int
nbd_trim (void *handle, uint32_t count, uint64_t offset)
{
  struct handle *h = handle;
  int c;

  c = nbd_request (h, NBD_CMD_TRIM, offset, count);
  return c < 0 ? c : nbd_reply (h, c);
}

/* Flush the file to disk. */
static int
nbd_flush (void *handle)
{
  struct handle *h = handle;
  int c;

  c = nbd_request (h, NBD_CMD_FLUSH, 0, 0);
  return c < 0 ? c : nbd_reply (h, c);
}

static struct nbdkit_plugin plugin = {
  .name               = "nbd",
  .longname           = "nbdkit nbd plugin",
  .version            = PACKAGE_VERSION,
  .unload             = nbd_unload,
  .config             = nbd_config,
  .config_complete    = nbd_config_complete,
  .config_help        = nbd_config_help,
  .open               = nbd_open,
  .close              = nbd_close,
  .get_size           = nbd_get_size,
  .can_write          = nbd_can_write,
  .can_flush          = nbd_can_flush,
  .is_rotational      = nbd_is_rotational,
  .can_trim           = nbd_can_trim,
  .pread              = nbd_pread,
  .pwrite             = nbd_pwrite,
  .zero               = nbd_zero,
  .flush              = nbd_flush,
  .trim               = nbd_trim,
  .errno_is_preserved = 1,
};

NBDKIT_REGISTER_PLUGIN(plugin)
