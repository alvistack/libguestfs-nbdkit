/* nbdkit
 * Copyright (C) 2017-2020 Red Hat Inc.
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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>

#define NBDKIT_API_VERSION 2

#include <nbdkit-plugin.h>
#include "nbd-protocol.h"
#include "protostrings.h"
#include "byte-swapping.h"
#include "cleanup.h"

/* The per-transaction details */
struct transaction {
  uint64_t cookie;
  sem_t sem;
  void *buf;
  uint64_t offset;
  uint32_t count;
  uint32_t err;
  struct nbdkit_extents *extents;
  struct transaction *next;
};

/* The per-connection handle */
struct handle {
  /* These fields are read-only once initialized */
  int fd;
  int flags;
  int64_t size;
  bool structured;
  bool extents;
  pthread_t reader;

  /* Prevents concurrent threads from interleaving writes to server */
  pthread_mutex_t write_lock;

  pthread_mutex_t trans_lock; /* Covers access to all fields below */
  struct transaction *trans;
  uint64_t unique;
  bool dead;
};

/* Connect to server via absolute name of Unix socket */
static char *sockname;

/* Connect to server via TCP socket */
static const char *hostname;
static const char *port;

/* Human-readable server description */
static char *servname;

/* Name of export on remote server, default '', ignored for oldstyle */
static const char *export;

/* Number of retries */
static unsigned retry;

/* True to share single server connection among all clients */
static bool shared;
static struct handle *shared_handle;

static struct handle *nbd_open_handle (int readonly);
static void nbd_close_handle (struct handle *h);

static void
nbd_unload (void)
{
  if (shared)
    nbd_close_handle (shared_handle);
  free (sockname);
  free (servname);
}

/* Called for each key=value passed on the command line.  This plugin
 * accepts socket=<sockname> or hostname=<hostname>/port=<port>
 * (exactly one connection required), and optional parameters
 * export=<name>, retry=<n> and shared=<bool>.
 */
static int
nbd_config (const char *key, const char *value)
{
  int r;

  if (strcmp (key, "socket") == 0) {
    /* See FILENAMES AND PATHS in nbdkit-plugin(3) */
    free (sockname);
    sockname = nbdkit_absolute_path (value);
    if (!sockname)
      return -1;
  }
  else if (strcmp (key, "hostname") == 0)
    hostname = value;
  else if (strcmp (key, "port") == 0)
    port = value;
  else if (strcmp (key, "export") == 0)
    export = value;
  else if (strcmp (key, "retry") == 0) {
    if (nbdkit_parse_unsigned ("retry", value, &retry) == -1)
      return -1;
  }
  else if (strcmp (key, "shared") == 0) {
    r = nbdkit_parse_bool (value);
    if (r == -1)
      return -1;
    shared = r;
  }
  else {
    nbdkit_error ("unknown parameter '%s'", key);
    return -1;
  }

  return 0;
}

/* Check the user passed exactly one socket description. */
static int
nbd_config_complete (void)
{
  int r;

  if (sockname) {
    struct sockaddr_un sock;

    if (hostname || port) {
      nbdkit_error ("cannot mix Unix socket and TCP hostname/port parameters");
      return -1;
    }
    if (strlen (sockname) > sizeof sock.sun_path) {
      nbdkit_error ("socket file name too large");
      return -1;
    }
    servname = strdup (sockname);
  }
  else {
    if (!hostname) {
      nbdkit_error ("must supply socket= or hostname= of external NBD server");
      return -1;
    }
    if (!port)
      port = "10809";
    if (strchr (hostname, ':'))
      r = asprintf (&servname, "[%s]:%s", hostname, port);
    else
      r = asprintf (&servname, "%s:%s", hostname, port);
    if (r < 0) {
      nbdkit_error ("asprintf: %m");
      return -1;
    }
  }

  if (!export)
    export = "";

  if (shared && (shared_handle = nbd_open_handle (false)) == NULL)
    return -1;
  return 0;
}

#define nbd_config_help \
  "socket=<SOCKNAME>      The Unix socket to connect to.\n" \
  "hostname=<HOST>        The hostname for the TCP socket to connect to.\n" \
  "port=<PORT>            TCP port or service name to use (default 10809).\n" \
  "export=<NAME>          Export name to connect to (default \"\").\n" \
  "retry=<N>              Retry connection up to N seconds (default 0).\n" \
  "shared=<BOOL>          True to share one server connection among all clients,\n" \
  "                       rather than a connection per client (default false).\n" \

#define THREAD_MODEL NBDKIT_THREAD_MODEL_PARALLEL

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

  ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&h->trans_lock);
  if (!h->dead) {
    nbdkit_debug ("permanent failure while talking to server %s: %m",
                  servname);
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

/* Find and possibly remove the transaction corresponding to cookie
   from the list. */
static struct transaction *
find_trans_by_cookie (struct handle *h, uint64_t cookie, bool remove)
{
  struct transaction **ptr;
  struct transaction *trans;

  ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&h->trans_lock);
  ptr = &h->trans;
  while ((trans = *ptr) != NULL) {
    if (cookie == trans->cookie)
      break;
    ptr = &trans->next;
  }
  if (trans && remove)
    *ptr = trans->next;
  return trans;
}

/* Send a request, return 0 on success or -1 on write failure. */
static int
nbd_request_raw (struct handle *h, uint16_t flags, uint16_t type,
                 uint64_t offset, uint32_t count, uint64_t cookie,
                 const void *buf)
{
  struct nbd_request req = {
    .magic = htobe32 (NBD_REQUEST_MAGIC),
    .flags = htobe16 (flags),
    .type = htobe16 (type),
    .handle = cookie, /* Opaque to server, so endianness doesn't matter */
    .offset = htobe64 (offset),
    .count = htobe32 (count),
  };
  int r;

  ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&h->write_lock);
  nbdkit_debug ("sending request type %d (%s), flags %#x, offset %#" PRIx64
                ", count %#x, cookie %#" PRIx64, type, name_of_nbd_cmd (type),
                flags, offset, count, cookie);
  r = write_full (h->fd, &req, sizeof req);
  if (buf && !r)
    r = write_full (h->fd, buf, count);
  return r;
}

/* Perform the request half of a transaction. On success, return the
   transaction; on error return NULL. */
static struct transaction *
nbd_request_full (struct handle *h, uint16_t flags, uint16_t type,
                  uint64_t offset, uint32_t count, const void *req_buf,
                  void *rep_buf, struct nbdkit_extents *extents)
{
  int err;
  struct transaction *trans;
  uint64_t cookie;

  trans = calloc (1, sizeof *trans);
  if (!trans) {
    nbdkit_error ("unable to track transaction: %m");
    /* Still in sync with server, so don't mark connection dead */
    return NULL;
  }
  if (sem_init (&trans->sem, 0, 0)) {
    nbdkit_error ("unable to create semaphore: %m");
    /* Still in sync with server, so don't mark connection dead */
    free (trans);
    return NULL;
  }
  trans->buf = rep_buf;
  trans->count = rep_buf ? count : 0;
  trans->offset = offset;
  trans->extents = extents;
  {
    ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&h->trans_lock);
    if (h->dead)
      goto err;
    cookie = trans->cookie = h->unique++;
    trans->next = h->trans;
    h->trans = trans;
  }
  if (nbd_request_raw (h, flags, type, offset, count, cookie, req_buf) == 0)
    return trans;
  trans = find_trans_by_cookie (h, cookie, true);

 err:
  err = errno;
  if (sem_destroy (&trans->sem))
    abort ();
  free (trans);
  nbd_mark_dead (h);
  errno = err;
  return NULL;
}

/* Shorthand for nbd_request_full when no extra buffers are involved. */
static struct transaction *
nbd_request (struct handle *h, uint16_t flags, uint16_t type, uint64_t offset,
             uint32_t count)
{
  return nbd_request_full (h, flags, type, offset, count, NULL, NULL, NULL);
}

/* Read a reply, and look up the corresponding transaction.
   Return the server's non-negative answer (converted to local errno
   value) on success, or -1 on read failure. If structured replies
   were negotiated, trans_out is set to NULL if there are still more replies
   expected. */
static int
nbd_reply_raw (struct handle *h, struct transaction **trans_out)
{
  union {
    struct nbd_simple_reply simple;
    struct nbd_structured_reply structured;
  } rep;
  struct transaction *trans;
  void *buf = NULL;
  CLEANUP_FREE char *payload = NULL;
  uint32_t count;
  uint32_t id;
  struct nbd_block_descriptor *extents = NULL;
  size_t nextents = 0;
  int error = NBD_SUCCESS;
  bool more = false;
  uint32_t len = 0; /* 0 except for structured reads */
  uint64_t offset = 0; /* if len, absolute offset of structured read chunk */
  bool zero = false; /* if len, whether to read or memset */
  uint16_t errlen;

  *trans_out = NULL;
  /* magic and handle overlap between simple and structured replies */
  if (read_full (h->fd, &rep, sizeof rep.simple))
    return nbd_mark_dead (h);
  rep.simple.magic = be32toh (rep.simple.magic);
  switch (rep.simple.magic) {
  case NBD_SIMPLE_REPLY_MAGIC:
    nbdkit_debug ("received simple reply for cookie %#" PRIx64 ", status %s",
                  rep.simple.handle,
                  name_of_nbd_error (be32toh (rep.simple.error)));
    error = be32toh (rep.simple.error);
    break;
  case NBD_STRUCTURED_REPLY_MAGIC:
    if (!h->structured) {
      nbdkit_error ("structured response without negotiation");
      return nbd_mark_dead (h);
    }
    if (read_full (h->fd, sizeof rep.simple + (char *) &rep,
                   sizeof rep - sizeof rep.simple))
      return nbd_mark_dead (h);
    rep.structured.flags = be16toh (rep.structured.flags);
    rep.structured.type = be16toh (rep.structured.type);
    rep.structured.length = be32toh (rep.structured.length);
    nbdkit_debug ("received structured reply %s for cookie %#" PRIx64
                  ", payload length %" PRId32,
                  name_of_nbd_reply_type (rep.structured.type),
                  rep.structured.handle, rep.structured.length);
    if (rep.structured.length > 64 * 1024 * 1024) {
      nbdkit_error ("structured reply length is suspiciously large: %" PRId32,
                    rep.structured.length);
      return nbd_mark_dead (h);
    }
    if (rep.structured.length) {
      /* Special case for OFFSET_DATA in order to read tail of chunk
         directly into final buffer later on */
      len = (rep.structured.type == NBD_REPLY_TYPE_OFFSET_DATA &&
             rep.structured.length > sizeof offset) ? sizeof offset :
        rep.structured.length;
      payload = malloc (len);
      if (!payload) {
        nbdkit_error ("reading structured reply payload: %m");
        return nbd_mark_dead (h);
      }
      if (read_full (h->fd, payload, len))
        return nbd_mark_dead (h);
      len = 0;
    }
    more = !(rep.structured.flags & NBD_REPLY_FLAG_DONE);
    switch (rep.structured.type) {
    case NBD_REPLY_TYPE_NONE:
      if (rep.structured.length) {
        nbdkit_error ("NBD_REPLY_TYPE_NONE with invalid payload");
        return nbd_mark_dead (h);
      }
      if (more) {
        nbdkit_error ("NBD_REPLY_TYPE_NONE without done flag");
        return nbd_mark_dead (h);
      }
      break;
    case NBD_REPLY_TYPE_OFFSET_DATA:
      if (rep.structured.length <= sizeof offset) {
        nbdkit_error ("structured reply OFFSET_DATA too small");
        return nbd_mark_dead (h);
      }
      memcpy (&offset, payload, sizeof offset);
      offset = be64toh (offset);
      len = rep.structured.length - sizeof offset;
      break;
    case NBD_REPLY_TYPE_OFFSET_HOLE:
      if (rep.structured.length != sizeof offset + sizeof len) {
        nbdkit_error ("structured reply OFFSET_HOLE size incorrect");
        return nbd_mark_dead (h);
      }
      memcpy (&offset, payload, sizeof offset);
      offset = be64toh (offset);
      memcpy (&len, payload, sizeof len);
      len = be32toh (len);
      if (!len) {
        nbdkit_error ("structured reply OFFSET_HOLE length incorrect");
        return nbd_mark_dead (h);
      }
      zero = true;
      break;
    case NBD_REPLY_TYPE_BLOCK_STATUS:
      if (!h->extents) {
        nbdkit_error ("block status response without negotiation");
        return nbd_mark_dead (h);
      }
      if (rep.structured.length < sizeof *extents ||
          rep.structured.length % sizeof *extents != sizeof id) {
        nbdkit_error ("structured reply OFFSET_HOLE size incorrect");
        return nbd_mark_dead (h);
      }
      nextents = rep.structured.length / sizeof *extents;
      extents = (struct nbd_block_descriptor *) &payload[sizeof id];
      memcpy (&id, payload, sizeof id);
      id = be32toh (id);
      nbdkit_debug ("parsing %zu extents for context id %" PRId32,
                    nextents, id);
      break;
    default:
      if (!NBD_REPLY_TYPE_IS_ERR (rep.structured.type)) {
        nbdkit_error ("received unexpected structured reply %s",
                      name_of_nbd_reply_type (rep.structured.type));
        return nbd_mark_dead (h);
      }

      if (rep.structured.length < sizeof error + sizeof errlen) {
        nbdkit_error ("structured reply error size incorrect");
        return nbd_mark_dead (h);
      }
      memcpy (&errlen, payload + sizeof error, sizeof errlen);
      errlen = be16toh (errlen);
      if (errlen > rep.structured.length - sizeof error - sizeof errlen) {
        nbdkit_error ("structured reply error message size incorrect");
        return nbd_mark_dead (h);
      }
      memcpy (&error, payload, sizeof error);
      error = be32toh (error);
      if (errlen)
        nbdkit_debug ("received structured error %s with message: %.*s",
                      name_of_nbd_error (error), (int) errlen,
                      payload + sizeof error + sizeof errlen);
      else
        nbdkit_debug ("received structured error %s without message",
                      name_of_nbd_error (error));
    }
    break;

  default:
    nbdkit_error ("received unexpected magic in reply: %#" PRIx32,
                  rep.simple.magic);
    return nbd_mark_dead (h);
  }

  trans = find_trans_by_cookie (h, rep.simple.handle, !more);
  if (!trans) {
    nbdkit_error ("reply with unexpected cookie %#" PRIx64, rep.simple.handle);
    return nbd_mark_dead (h);
  }

  buf = trans->buf;
  count = trans->count;
  if (nextents) {
    if (!trans->extents) {
      nbdkit_error ("block status response to a non-status command");
      return nbd_mark_dead (h);
    }
    offset = trans->offset;
    for (size_t i = 0; i < nextents; i++) {
      /* We rely on the fact that NBDKIT_EXTENT_* match NBD_STATE_* */
      if (nbdkit_add_extent (trans->extents, offset,
                             be32toh (extents[i].length),
                             be32toh (extents[i].status_flags)) == -1) {
        error = errno;
        break;
      }
      offset += be32toh (extents[i].length);
    }
  }
  if (buf && h->structured && rep.simple.magic == NBD_SIMPLE_REPLY_MAGIC) {
    nbdkit_error ("simple read reply when structured was expected");
    return nbd_mark_dead (h);
  }
  if (len) {
    if (!buf) {
      nbdkit_error ("structured read response to a non-read command");
      return nbd_mark_dead (h);
    }
    if (offset < trans->offset || offset > INT64_MAX ||
        offset + len > trans->offset + count) {
      nbdkit_error ("structured read reply with unexpected offset/length");
      return nbd_mark_dead (h);
    }
    buf = (char *) buf + offset - trans->offset;
    if (zero) {
      memset (buf, 0, len);
      buf = NULL;
    }
    else
      count = len;
  }

  /* Thanks to structured replies, we must preserve an error in any
     earlier chunk for replay during the final chunk. */
  if (!more) {
    *trans_out = trans;
    if (!error)
      error = trans->err;
  }
  else if (error && !trans->err)
    trans->err = error;

  /* Convert from wire value to local errno, and perform any final read */
  switch (error) {
  case NBD_SUCCESS:
    if (buf && read_full (h->fd, buf, count))
      return nbd_mark_dead (h);
    return 0;
  case NBD_EPERM:
    return EPERM;
  case NBD_EIO:
    return EIO;
  case NBD_ENOMEM:
    return ENOMEM;
  default:
    nbdkit_debug ("unexpected error %d, squashing to EINVAL", error);
    /* fallthrough */
  case NBD_EINVAL:
    return EINVAL;
  case NBD_ENOSPC:
    return ENOSPC;
  case NBD_EOVERFLOW:
    return EOVERFLOW;
  case NBD_ESHUTDOWN:
    return ESHUTDOWN;
  }
}

/* Reader loop. */
void *
nbd_reader (void *handle)
{
  struct handle *h = handle;
  bool done = false;
  int r;

  while (!done) {
    struct transaction *trans;

    r = nbd_reply_raw (h, &trans);
    if (r >= 0) {
      if (!trans)
        nbdkit_debug ("partial reply handled, waiting for final reply");
      else {
        trans->err = r;
        if (sem_post (&trans->sem)) {
          nbdkit_error ("failed to post semaphore: %m");
          abort ();
        }
      }
    }
    ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&h->trans_lock);
    done = h->dead;
  }

  /* Clean up any stranded in-flight requests */
  r = ESHUTDOWN;
  while (1) {
    struct transaction *trans;

    {
      ACQUIRE_LOCK_FOR_CURRENT_SCOPE (&h->trans_lock);
      trans = h->trans;
      h->trans = trans ? trans->next : NULL;
    }
    if (!trans)
      break;
    trans->err = r;
    if (sem_post (&trans->sem)) {
      nbdkit_error ("failed to post semaphore: %m");
      abort ();
    }
  }
  return NULL;
}

/* Perform the reply half of a transaction. */
static int
nbd_reply (struct handle *h, struct transaction *trans)
{
  int err;

  if (!trans) {
    assert (errno);
    return -1;
  }

  while ((err = sem_wait (&trans->sem)) == -1 && errno == EINTR)
    /* try again */;
  if (err) {
    nbdkit_debug ("failed to wait on semaphore: %m");
    err = EIO;
  }
  else
    err = trans->err;
  if (sem_destroy (&trans->sem))
    abort ();
  free (trans);
  errno = err;
  return err ? -1 : 0;
}

/* Receive response to @option into @reply, and consume any
   payload. If @payload is non-NULL, caller must free *payload. Return
   0 on success, or -1 if communication to server is no longer
   possible. */
static int
nbd_newstyle_recv_option_reply (struct handle *h, uint32_t option,
                                struct nbd_fixed_new_option_reply *reply,
                                void **payload)
{
  CLEANUP_FREE char *buffer = NULL;

  if (payload)
    *payload = NULL;
  if (read_full (h->fd, reply, sizeof *reply)) {
    nbdkit_error ("unable to read option reply: %m");
    return -1;
  }
  reply->magic = be64toh (reply->magic);
  reply->option = be32toh (reply->option);
  reply->reply = be32toh (reply->reply);
  reply->replylen = be32toh (reply->replylen);
  if (reply->magic != NBD_REP_MAGIC || reply->option != option) {
    nbdkit_error ("unexpected option reply");
    return -1;
  }
  if (reply->replylen) {
    if (reply->reply == NBD_REP_ACK) {
      nbdkit_error ("NBD_REP_ACK should not have replylen %" PRId32,
                    reply->replylen);
      return -1;
    }
    if (reply->replylen > 16 * 1024 * 1024) {
      nbdkit_error ("option reply length is suspiciously large: %" PRId32,
                    reply->replylen);
      return -1;
    }
    /* buffer is a string for NBD_REP_ERR_*; adding a NUL terminator
       makes that string easier to use, without hurting other reply
       types where buffer is not a string */
    buffer = malloc (reply->replylen + 1);
    if (!buffer) {
      nbdkit_error ("malloc: %m");
      return -1;
    }
    if (read_full (h->fd, buffer, reply->replylen)) {
      nbdkit_error ("unable to read option reply payload: %m");
      return -1;
    }
    buffer[reply->replylen] = '\0';
    if (!payload)
      nbdkit_debug ("ignoring option reply payload");
    else {
      *payload = buffer;
      buffer = NULL;
    }
  }
  return 0;
}

/* Attempt to negotiate structured reads, block status, and NBD_OPT_GO.
   Return 1 if haggling completed, 0 if haggling failed but
   NBD_OPT_EXPORT_NAME is still viable, or -1 on inability to connect. */
static int
nbd_newstyle_haggle (struct handle *h)
{
  const char *const query = "base:allocation";
  struct nbd_new_option opt;
  uint32_t exportnamelen = htobe32 (strlen (export));
  uint32_t nrqueries = htobe32 (1);
  uint32_t querylen = htobe32 (strlen (query));
  /* For now, we make no NBD_INFO_* requests, relying on the server to
     send its defaults. TODO: nbdkit should let plugins report block
     sizes, at which point we should request NBD_INFO_BLOCK_SIZE and
     obey any sizes set by server. */
  uint16_t nrinfos = htobe16 (0);
  struct nbd_fixed_new_option_reply reply;

  nbdkit_debug ("trying NBD_OPT_STRUCTURED_REPLY");
  opt.version = htobe64 (NBD_NEW_VERSION);
  opt.option = htobe32 (NBD_OPT_STRUCTURED_REPLY);
  opt.optlen = htobe32 (0);
  if (write_full (h->fd, &opt, sizeof opt)) {
    nbdkit_error ("unable to request NBD_OPT_STRUCTURED_REPLY: %m");
    return -1;
  }
  if (nbd_newstyle_recv_option_reply (h, NBD_OPT_STRUCTURED_REPLY, &reply,
                                      NULL) < 0)
    return -1;
  if (reply.reply == NBD_REP_ACK) {
    nbdkit_debug ("structured replies enabled, trying NBD_OPT_SET_META_CONTEXT");
    h->structured = true;

    opt.version = htobe64 (NBD_NEW_VERSION);
    opt.option = htobe32 (NBD_OPT_SET_META_CONTEXT);
    opt.optlen = htobe32 (sizeof exportnamelen + strlen (export) +
                          sizeof nrqueries + sizeof querylen + strlen (query));
    if (write_full (h->fd, &opt, sizeof opt) ||
        write_full (h->fd, &exportnamelen, sizeof exportnamelen) ||
        write_full (h->fd, export, strlen (export)) ||
        write_full (h->fd, &nrqueries, sizeof nrqueries) ||
        write_full (h->fd, &querylen, sizeof querylen) ||
        write_full (h->fd, query, strlen (query))) {
      nbdkit_error ("unable to request NBD_OPT_SET_META_CONTEXT: %m");
      return -1;
    }
    if (nbd_newstyle_recv_option_reply (h, NBD_OPT_SET_META_CONTEXT, &reply,
                                        NULL) < 0)
      return -1;
    if (reply.reply == NBD_REP_META_CONTEXT) {
      /* Cheat: we asked for exactly one context. We could double
         check that the server is replying with exactly the
         "base:allocation" context, and then remember the id it tells
         us to later confirm that responses to NBD_CMD_BLOCK_STATUS
         match up; but in the absence of multiple contexts, it's
         easier to just assume the server is compliant, and will reuse
         the same id, without bothering to check further. */
      nbdkit_debug ("extents enabled");
      h->extents = true;
      if (nbd_newstyle_recv_option_reply (h, NBD_OPT_SET_META_CONTEXT, &reply,
                                          NULL) < 0)
        return -1;
    }
    if (reply.reply != NBD_REP_ACK) {
      if (h->extents) {
        nbdkit_error ("unexpected response to set meta context");
        return -1;
      }
      nbdkit_debug ("ignoring meta context response %s",
                    name_of_nbd_rep (reply.reply));
    }
  }
  else {
    nbdkit_debug ("structured replies disabled");
  }

  /* Try NBD_OPT_GO */
  nbdkit_debug ("trying NBD_OPT_GO");
  opt.version = htobe64 (NBD_NEW_VERSION);
  opt.option = htobe32 (NBD_OPT_GO);
  opt.optlen = htobe32 (sizeof exportnamelen + strlen (export) +
                        sizeof nrinfos);
  if (write_full (h->fd, &opt, sizeof opt) ||
      write_full (h->fd, &exportnamelen, sizeof exportnamelen) ||
      write_full (h->fd, export, strlen (export)) ||
      write_full (h->fd, &nrinfos, sizeof nrinfos)) {
    nbdkit_error ("unable to request NBD_OPT_GO: %m");
    return -1;
  }
  while (1) {
    CLEANUP_FREE void *buffer;
    struct nbd_fixed_new_option_reply_info_export *reply_export;
    uint16_t info;

    if (nbd_newstyle_recv_option_reply (h, NBD_OPT_GO, &reply, &buffer) < 0)
      return -1;
    switch (reply.reply) {
    case NBD_REP_INFO:
      /* Parse payload, but ignore all except NBD_INFO_EXPORT */
      if (reply.replylen < 2) {
        nbdkit_error ("NBD_REP_INFO reply too short");
        return -1;
      }
      memcpy (&info, buffer, sizeof info);
      info = be16toh (info);
      switch (info) {
      case NBD_INFO_EXPORT:
        if (reply.replylen != sizeof *reply_export) {
          nbdkit_error ("NBD_INFO_EXPORT reply wrong size");
          return -1;
        }
        reply_export = buffer;
        h->size = be64toh (reply_export->exportsize);
        h->flags = be16toh (reply_export->eflags);
        break;
      default:
        nbdkit_debug ("ignoring server info %d", info);
      }
      break;
    case NBD_REP_ACK:
      /* End of replies, valid if server already sent NBD_INFO_EXPORT,
         observable since h->flags must contain NBD_FLAG_HAS_FLAGS */
      assert (!buffer);
      if (!h->flags) {
        nbdkit_error ("server omitted NBD_INFO_EXPORT reply to NBD_OPT_GO");
        return -1;
      }
      nbdkit_debug ("NBD_OPT_GO complete");
      return 1;
    case NBD_REP_ERR_UNSUP:
      /* Special case this failure to fall back to NBD_OPT_EXPORT_NAME */
      nbdkit_debug ("server lacks NBD_OPT_GO support");
      return 0;
    default:
      /* Unexpected. Either the server sent a legitimate error or an
         unexpected reply, but either way, we can't connect. */
      if (NBD_REP_IS_ERR (reply.reply))
        if (reply.replylen)
          nbdkit_error ("server rejected NBD_OPT_GO with %s: %s",
                        name_of_nbd_rep (reply.reply), (char *) buffer);
        else
          nbdkit_error ("server rejected NBD_OPT_GO with %s",
                        name_of_nbd_rep (reply.reply));
      else
        nbdkit_error ("server used unexpected reply %s to NBD_OPT_GO",
                      name_of_nbd_rep (reply.reply));
      return -1;
    }
  }
}

/* Connect to a Unix socket, returning the fd on success */
static int
nbd_connect_unix (void)
{
  struct sockaddr_un sock = { .sun_family = AF_UNIX };
  int fd;

  nbdkit_debug ("connecting to Unix socket name=%s", sockname);
  fd = socket (AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    nbdkit_error ("socket: %m");
    return -1;
  }

  /* We already validated length during nbd_config_complete */
  assert (strlen (sockname) <= sizeof sock.sun_path);
  memcpy (sock.sun_path, sockname, strlen (sockname));
  if (connect (fd, (const struct sockaddr *) &sock, sizeof sock) < 0) {
    nbdkit_error ("connect: %m");
    return -1;
  }
  return fd;
}

/* Connect to a TCP socket, returning the fd on success */
static int
nbd_connect_tcp (void)
{
  struct addrinfo hints = { .ai_family = AF_UNSPEC,
                            .ai_socktype = SOCK_STREAM, };
  struct addrinfo *result, *rp;
  int r;
  const int optval = 1;
  int fd;

  nbdkit_debug ("connecting to TCP socket host=%s port=%s", hostname, port);
  r = getaddrinfo (hostname, port, &hints, &result);
  if (r != 0) {
    nbdkit_error ("getaddrinfo: %s", gai_strerror (r));
    return -1;
  }

  assert (result != NULL);

  for (rp = result; rp; rp = rp->ai_next) {
    fd = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1)
      continue;
    if (connect (fd, rp->ai_addr, rp->ai_addrlen) != -1)
      break;
    close (fd);
  }
  freeaddrinfo (result);
  if (rp == NULL) {
    nbdkit_error ("connect: %m");
    close (fd);
    return -1;
  }

  if (setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &optval,
                  sizeof (int)) == -1) {
    nbdkit_error ("cannot set TCP_NODELAY option: %m");
    close (fd);
    return -1;
  }
  return fd;
}

/* Create the shared or per-connection handle. */
static struct handle *
nbd_open_handle (int readonly)
{
  struct handle *h;
  struct nbd_old_handshake old;
  uint64_t version;
  unsigned long retries = retry;

  h = calloc (1, sizeof *h);
  if (h == NULL) {
    nbdkit_error ("malloc: %m");
    return NULL;
  }

 retry:
  if (sockname)
    h->fd = nbd_connect_unix ();
  else
    h->fd = nbd_connect_tcp ();
  if (h->fd == -1) {
    if (retries--) {
      sleep (1);
      goto retry;
    }
    goto err;
  }

  /* old and new handshake share same meaning of first 16 bytes */
  if (read_full (h->fd, &old,
                 offsetof (struct nbd_old_handshake, exportsize))) {
    nbdkit_error ("unable to read magic: %m");
    goto err;
  }
  if (be64toh (old.nbdmagic) != NBD_MAGIC) {
    nbdkit_error ("wrong magic, %s is not an NBD server", servname);
    goto err;
  }
  version = be64toh (old.version);
  if (version == NBD_OLD_VERSION) {
    nbdkit_debug ("trying oldstyle connection");
    if (read_full (h->fd,
                   (char *) &old + offsetof (struct nbd_old_handshake, exportsize),
                   sizeof old - offsetof (struct nbd_old_handshake, exportsize))) {
      nbdkit_error ("unable to read old handshake: %m");
      goto err;
    }
    h->size = be64toh (old.exportsize);
    h->flags = be16toh (old.eflags);
  }
  else if (version == NBD_NEW_VERSION) {
    uint16_t gflags;
    uint32_t cflags;
    struct nbd_new_option opt;
    struct nbd_export_name_option_reply finish;
    size_t expect;

    nbdkit_debug ("trying newstyle connection");
    if (read_full (h->fd, &gflags, sizeof gflags)) {
      nbdkit_error ("unable to read global flags: %m");
      goto err;
    }
    gflags = be16toh (gflags);
    cflags = htobe32 (gflags & (NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES));
    if (write_full (h->fd, &cflags, sizeof cflags)) {
      nbdkit_error ("unable to return global flags: %m");
      goto err;
    }

    /* Prefer NBD_OPT_GO if possible */
    if (gflags & NBD_FLAG_FIXED_NEWSTYLE) {
      int rc = nbd_newstyle_haggle (h);
      if (rc < 0)
        goto err;
      if (!rc)
        goto export_name;
    }
    else {
    export_name:
      /* Option haggling untried or failed, use older NBD_OPT_EXPORT_NAME */
      nbdkit_debug ("trying NBD_OPT_EXPORT_NAME");
      opt.version = htobe64 (NBD_NEW_VERSION);
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
  }
  else {
    nbdkit_error ("unexpected version %#" PRIx64, version);
    goto err;
  }
  if (readonly)
    h->flags |= NBD_FLAG_READ_ONLY;

  /* Spawn a dedicated reader thread */
  if ((errno = pthread_mutex_init (&h->write_lock, NULL))) {
    nbdkit_error ("failed to initialize write mutex: %m");
    goto err;
  }
  if ((errno = pthread_mutex_init (&h->trans_lock, NULL))) {
    nbdkit_error ("failed to initialize transaction mutex: %m");
    pthread_mutex_destroy (&h->write_lock);
    goto err;
  }
  if ((errno = pthread_create (&h->reader, NULL, nbd_reader, h))) {
    nbdkit_error ("failed to initialize reader thread: %m");
    pthread_mutex_destroy (&h->write_lock);
    pthread_mutex_destroy (&h->trans_lock);
    goto err;
  }

  return h;

 err:
  if (h->fd >= 0)
    close (h->fd);
  free (h);
  return NULL;
}

/* Create the per-connection handle. */
static void *
nbd_open (int readonly)
{
  if (shared)
    return shared_handle;
  return nbd_open_handle (readonly);
}

/* Free up the shared or per-connection handle. */
static void
nbd_close_handle (struct handle *h)
{
  if (!h->dead) {
    nbd_request_raw (h, 0, NBD_CMD_DISC, 0, 0, 0, NULL);
    shutdown (h->fd, SHUT_WR);
  }
  if ((errno = pthread_join (h->reader, NULL)))
    nbdkit_debug ("failed to join reader thread: %m");
  close (h->fd);
  pthread_mutex_destroy (&h->write_lock);
  pthread_mutex_destroy (&h->trans_lock);
  free (h);
}

/* Free up the per-connection handle. */
static void
nbd_close (void *handle)
{
  struct handle *h = handle;

  if (!shared)
    nbd_close_handle (h);
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

  return !!(h->flags & NBD_FLAG_SEND_FLUSH);
}

static int
nbd_is_rotational (void *handle)
{
  struct handle *h = handle;

  return !!(h->flags & NBD_FLAG_ROTATIONAL);
}

static int
nbd_can_trim (void *handle)
{
  struct handle *h = handle;

  return !!(h->flags & NBD_FLAG_SEND_TRIM);
}

static int
nbd_can_zero (void *handle)
{
  struct handle *h = handle;

  return !!(h->flags & NBD_FLAG_SEND_WRITE_ZEROES);
}

static int
nbd_can_fua (void *handle)
{
  struct handle *h = handle;

  return h->flags & NBD_FLAG_SEND_FUA ? NBDKIT_FUA_NATIVE : NBDKIT_FUA_NONE;
}

static int
nbd_can_multi_conn (void *handle)
{
  struct handle *h = handle;

  return !!(h->flags & NBD_FLAG_CAN_MULTI_CONN);
}

static int
nbd_can_cache (void *handle)
{
  struct handle *h = handle;

  if (h->flags & NBD_FLAG_SEND_CACHE)
    return NBDKIT_CACHE_NATIVE;
  return NBDKIT_CACHE_NONE;
}

static int
nbd_can_extents (void *handle)
{
  struct handle *h = handle;

  return h->extents;
}

/* Read data from the file. */
static int
nbd_pread (void *handle, void *buf, uint32_t count, uint64_t offset,
           uint32_t flags)
{
  struct handle *h = handle;
  struct transaction *s;

  assert (!flags);
  s = nbd_request_full (h, 0, NBD_CMD_READ, offset, count, NULL, buf, NULL);
  return nbd_reply (h, s);
}

/* Write data to the file. */
static int
nbd_pwrite (void *handle, const void *buf, uint32_t count, uint64_t offset,
            uint32_t flags)
{
  struct handle *h = handle;
  struct transaction *s;

  assert (!(flags & ~NBDKIT_FLAG_FUA));
  s = nbd_request_full (h, flags & NBDKIT_FLAG_FUA ? NBD_CMD_FLAG_FUA : 0,
                        NBD_CMD_WRITE, offset, count, buf, NULL, NULL);
  return nbd_reply (h, s);
}

/* Write zeroes to the file. */
static int
nbd_zero (void *handle, uint32_t count, uint64_t offset, uint32_t flags)
{
  struct handle *h = handle;
  struct transaction *s;
  int f = 0;

  assert (!(flags & ~(NBDKIT_FLAG_FUA | NBDKIT_FLAG_MAY_TRIM)));
  assert (h->flags & NBD_FLAG_SEND_WRITE_ZEROES);

  if (!(flags & NBDKIT_FLAG_MAY_TRIM))
    f |= NBD_CMD_FLAG_NO_HOLE;
  if (flags & NBDKIT_FLAG_FUA)
    f |= NBD_CMD_FLAG_FUA;
  s = nbd_request (h, f, NBD_CMD_WRITE_ZEROES, offset, count);
  return nbd_reply (h, s);
}

/* Trim a portion of the file. */
static int
nbd_trim (void *handle, uint32_t count, uint64_t offset, uint32_t flags)
{
  struct handle *h = handle;
  struct transaction *s;

  assert (!(flags & ~NBDKIT_FLAG_FUA));
  s = nbd_request (h, flags & NBDKIT_FLAG_FUA ? NBD_CMD_FLAG_FUA : 0,
                   NBD_CMD_TRIM, offset, count);
  return nbd_reply (h, s);
}

/* Flush the file to disk. */
static int
nbd_flush (void *handle, uint32_t flags)
{
  struct handle *h = handle;
  struct transaction *s;

  assert (!flags);
  s = nbd_request (h, 0, NBD_CMD_FLUSH, 0, 0);
  return nbd_reply (h, s);
}

/* Read extents of the file. */
static int
nbd_extents (void *handle, uint32_t count, uint64_t offset,
             uint32_t flags, struct nbdkit_extents *extents)
{
  struct handle *h = handle;
  struct transaction *s;

  assert (!(flags & ~NBDKIT_FLAG_REQ_ONE) && h->extents);
  s = nbd_request_full (h, flags & NBDKIT_FLAG_REQ_ONE ? NBD_CMD_FLAG_REQ_ONE : 0,
                        NBD_CMD_BLOCK_STATUS, offset, count, NULL, NULL,
                        extents);
  return nbd_reply (h, s);
}

/* Cache a portion of the file. */
static int
nbd_cache (void *handle, uint32_t count, uint64_t offset, uint32_t flags)
{
  struct handle *h = handle;
  struct transaction *s;

  assert (!flags);
  s = nbd_request (h, 0, NBD_CMD_CACHE, offset, count);
  return nbd_reply (h, s);
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
  .can_zero           = nbd_can_zero,
  .can_fua            = nbd_can_fua,
  .can_multi_conn     = nbd_can_multi_conn,
  .can_extents        = nbd_can_extents,
  .can_cache          = nbd_can_cache,
  .pread              = nbd_pread,
  .pwrite             = nbd_pwrite,
  .zero               = nbd_zero,
  .flush              = nbd_flush,
  .trim               = nbd_trim,
  .extents            = nbd_extents,
  .cache              = nbd_cache,
  .errno_is_preserved = 1,
};

NBDKIT_REGISTER_PLUGIN (plugin)
