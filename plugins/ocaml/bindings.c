/* nbdkit
 * Copyright (C) 2014-2020 Red Hat Inc.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <caml/alloc.h>
#include <caml/fail.h>
#include <caml/memory.h>
#include <caml/mlvalues.h>
#include <caml/threads.h>

#define NBDKIT_API_VERSION 2

#include <nbdkit-plugin.h>

/* Bindings for miscellaneous nbdkit_* utility functions. */

/* NB: noalloc function. */
value
ocaml_nbdkit_set_error (value nv)
{
  int err;

  switch (Int_val (nv)) {
    /* Host errno values that will map to NBD protocol values */
  case 1: err = EPERM; break;
  case 2: err = EIO; break;
  case 3: err = ENOMEM; break;
  case 4: err = EINVAL; break;
  case 5: err = ENOSPC; break;
  case 6: err = ESHUTDOWN; break;
  case 7: err = EOVERFLOW; break;
  case 8: err = EOPNOTSUPP; break;
    /* Other errno values that server/protocol.c treats specially */
  case 9: err = EROFS; break;
  case 10: err = EFBIG; break;
  default: abort ();
  }

  nbdkit_set_error (err);

  return Val_unit;
}

value
ocaml_nbdkit_parse_size (value strv)
{
  CAMLparam1 (strv);
  CAMLlocal1 (rv);
  int64_t r;

  r = nbdkit_parse_size (String_val (strv));
  if (r == -1)
    caml_invalid_argument ("nbdkit_parse_size");
  rv = caml_copy_int64 (r);

  CAMLreturn (rv);
}

value
ocaml_nbdkit_parse_bool (value strv)
{
  CAMLparam1 (strv);
  CAMLlocal1 (rv);
  int r;

  r = nbdkit_parse_bool (String_val (strv));
  if (r == -1)
    caml_invalid_argument ("nbdkit_parse_bool");
  rv = Val_bool (r);

  CAMLreturn (rv);
}

value
ocaml_nbdkit_read_password (value strv)
{
  CAMLparam1 (strv);
  CAMLlocal1 (rv);
  char *password;
  int r;

  r = nbdkit_read_password (String_val (strv), &password);
  if (r == -1)
    caml_invalid_argument ("nbdkit_read_password");
  rv = caml_copy_string (password);
  free (password);

  CAMLreturn (rv);
}

value
ocaml_nbdkit_realpath (value strv)
{
  CAMLparam1 (strv);
  CAMLlocal1 (rv);
  char *ret;

  ret = nbdkit_realpath (String_val (strv));
  if (ret == NULL)
    caml_failwith ("nbdkit_realpath");
  rv = caml_copy_string (ret);
  free (ret);

  CAMLreturn (rv);
}

value
ocaml_nbdkit_nanosleep (value secv, value nsecv)
{
  CAMLparam2 (secv, nsecv);
  int r;

  caml_enter_blocking_section ();
  r = nbdkit_nanosleep (Int_val (secv), Int_val (nsecv));
  caml_leave_blocking_section ();
  if (r == -1)
    caml_failwith ("nbdkit_nanosleep");

  CAMLreturn (Val_unit);
}

value
ocaml_nbdkit_export_name (value unitv)
{
  CAMLparam1 (unitv);
  CAMLlocal1 (rv);
  const char *ret;

  ret = nbdkit_export_name ();
  /* Note that NULL indicates error.  Default export name is [""] even
   * for oldstyle.
   */
  if (ret == NULL)
    caml_failwith ("nbdkit_export_name");
  rv = caml_copy_string (ret);

  CAMLreturn (rv);
}

/* NB: noalloc function. */
value
ocaml_nbdkit_shutdown (value unitv)
{
  CAMLparam1 (unitv);

  nbdkit_shutdown ();
  CAMLreturn (Val_unit);
}

/* NB: noalloc function. */
value
ocaml_nbdkit_debug (value strv)
{
  nbdkit_debug ("%s", String_val (strv));

  return Val_unit;
}
