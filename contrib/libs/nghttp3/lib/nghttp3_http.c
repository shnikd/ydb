/*
 * nghttp3
 *
 * Copyright (c) 2019 nghttp3 contributors
 * Copyright (c) 2015 nghttp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "nghttp3_http.h"

#include <string.h>
#include <assert.h>

#ifdef __AVX2__
#  include <immintrin.h>
#endif /* __AVX2__ */

#include "nghttp3_stream.h"
#include "nghttp3_macro.h"
#include "nghttp3_conv.h"
#include "nghttp3_unreachable.h"
#include "sfparse/sfparse.h"

static uint8_t downcase(uint8_t c) {
  return 'A' <= c && c <= 'Z' ? (uint8_t)(c - 'A' + 'a') : c;
}

/*
 * memieq returns 1 if the data pointed by |a| of length |n| equals to
 * |b| of the same length in case-insensitive manner.  The data
 * pointed by |a| must not include upper cased letters (A-Z).
 */
static int memieq(const void *a, const void *b, size_t n) {
  size_t i;
  const uint8_t *aa = a, *bb = b;

  for (i = 0; i < n; ++i) {
    if (aa[i] != downcase(bb[i])) {
      return 0;
    }
  }
  return 1;
}

#define lstrieq(A, B, N) ((sizeof((A)) - 1) == (N) && memieq((A), (B), (N)))

static int64_t parse_uint(const uint8_t *s, size_t len) {
  int64_t n = 0;
  size_t i;
  if (len == 0) {
    return -1;
  }
  for (i = 0; i < len; ++i) {
    if ('0' <= s[i] && s[i] <= '9') {
      if (n > (int64_t)NGHTTP3_MAX_VARINT / 10) {
        return -1;
      }
      n *= 10;
      if (n > (int64_t)NGHTTP3_MAX_VARINT - (s[i] - '0')) {
        return -1;
      }
      n += s[i] - '0';
      continue;
    }
    return -1;
  }
  return n;
}

static int check_pseudo_header(nghttp3_http_state *http,
                               const nghttp3_qpack_nv *nv, uint32_t flag) {
  if ((http->flags & flag) || nv->value->len == 0) {
    return 0;
  }
  http->flags |= flag;
  return 1;
}

static int expect_response_body(nghttp3_http_state *http) {
  return (http->flags & NGHTTP3_HTTP_FLAG_METH_HEAD) == 0 &&
         http->status_code / 100 != 1 && http->status_code != 304 &&
         http->status_code != 204;
}

/* For "http" or "https" URIs, OPTIONS request may have "*" in :path
   header field to represent system-wide OPTIONS request.  Otherwise,
   :path header field value must start with "/".  This function must
   be called after ":method" header field was received.  This function
   returns nonzero if path is valid.*/
static int check_path_flags(nghttp3_http_state *http) {
  return (http->flags & NGHTTP3_HTTP_FLAG_SCHEME_HTTP) == 0 ||
         ((http->flags & NGHTTP3_HTTP_FLAG_PATH_REGULAR) ||
          ((http->flags & NGHTTP3_HTTP_FLAG_METH_OPTIONS) &&
           (http->flags & NGHTTP3_HTTP_FLAG_PATH_ASTERISK)));
}

static int is_ws(uint8_t c) {
  switch (c) {
  case ' ':
  case '\t':
    return 1;
  default:
    return 0;
  }
}

int nghttp3_http_parse_priority(nghttp3_pri *dest, const uint8_t *value,
                                size_t valuelen) {
  nghttp3_pri pri = *dest;
  sfparse_parser sfp;
  sfparse_vec key;
  sfparse_value val;
  int rv;

  sfparse_parser_init(&sfp, value, valuelen);

  for (;;) {
    rv = sfparse_parser_dict(&sfp, &key, &val);
    if (rv != 0) {
      if (rv == SFPARSE_ERR_EOF) {
        break;
      }

      return NGHTTP3_ERR_INVALID_ARGUMENT;
    }

    if (key.len != 1) {
      continue;
    }

    switch (key.base[0]) {
    case 'i':
      if (val.type != SFPARSE_TYPE_BOOLEAN) {
        return NGHTTP3_ERR_INVALID_ARGUMENT;
      }

      pri.inc = (uint8_t)val.boolean;

      break;
    case 'u':
      if (val.type != SFPARSE_TYPE_INTEGER ||
          val.integer < NGHTTP3_URGENCY_HIGH ||
          NGHTTP3_URGENCY_LOW < val.integer) {
        return NGHTTP3_ERR_INVALID_ARGUMENT;
      }

      pri.urgency = (uint32_t)val.integer;

      break;
    }
  }

  *dest = pri;

  return 0;
}

int nghttp3_pri_parse_priority_versioned(int pri_version, nghttp3_pri *dest,
                                         const uint8_t *value,
                                         size_t valuelen) {
  (void)pri_version;

  return nghttp3_http_parse_priority(dest, value, valuelen);
}

/* Generated by genauthroitychartbl.py */
static char VALID_AUTHORITY_CHARS[] = {
  0 /* NUL  */, 0 /* SOH  */, 0 /* STX  */, 0 /* ETX  */,
  0 /* EOT  */, 0 /* ENQ  */, 0 /* ACK  */, 0 /* BEL  */,
  0 /* BS   */, 0 /* HT   */, 0 /* LF   */, 0 /* VT   */,
  0 /* FF   */, 0 /* CR   */, 0 /* SO   */, 0 /* SI   */,
  0 /* DLE  */, 0 /* DC1  */, 0 /* DC2  */, 0 /* DC3  */,
  0 /* DC4  */, 0 /* NAK  */, 0 /* SYN  */, 0 /* ETB  */,
  0 /* CAN  */, 0 /* EM   */, 0 /* SUB  */, 0 /* ESC  */,
  0 /* FS   */, 0 /* GS   */, 0 /* RS   */, 0 /* US   */,
  0 /* SPC  */, 1 /* !    */, 0 /* "    */, 0 /* #    */,
  1 /* $    */, 1 /* %    */, 1 /* &    */, 1 /* '    */,
  1 /* (    */, 1 /* )    */, 1 /* *    */, 1 /* +    */,
  1 /* ,    */, 1 /* -    */, 1 /* .    */, 0 /* /    */,
  1 /* 0    */, 1 /* 1    */, 1 /* 2    */, 1 /* 3    */,
  1 /* 4    */, 1 /* 5    */, 1 /* 6    */, 1 /* 7    */,
  1 /* 8    */, 1 /* 9    */, 1 /* :    */, 1 /* ;    */,
  0 /* <    */, 1 /* =    */, 0 /* >    */, 0 /* ?    */,
  0 /* @    */, 1 /* A    */, 1 /* B    */, 1 /* C    */,
  1 /* D    */, 1 /* E    */, 1 /* F    */, 1 /* G    */,
  1 /* H    */, 1 /* I    */, 1 /* J    */, 1 /* K    */,
  1 /* L    */, 1 /* M    */, 1 /* N    */, 1 /* O    */,
  1 /* P    */, 1 /* Q    */, 1 /* R    */, 1 /* S    */,
  1 /* T    */, 1 /* U    */, 1 /* V    */, 1 /* W    */,
  1 /* X    */, 1 /* Y    */, 1 /* Z    */, 1 /* [    */,
  0 /* \    */, 1 /* ]    */, 0 /* ^    */, 1 /* _    */,
  0 /* `    */, 1 /* a    */, 1 /* b    */, 1 /* c    */,
  1 /* d    */, 1 /* e    */, 1 /* f    */, 1 /* g    */,
  1 /* h    */, 1 /* i    */, 1 /* j    */, 1 /* k    */,
  1 /* l    */, 1 /* m    */, 1 /* n    */, 1 /* o    */,
  1 /* p    */, 1 /* q    */, 1 /* r    */, 1 /* s    */,
  1 /* t    */, 1 /* u    */, 1 /* v    */, 1 /* w    */,
  1 /* x    */, 1 /* y    */, 1 /* z    */, 0 /* {    */,
  0 /* |    */, 0 /* }    */, 1 /* ~    */, 0 /* DEL  */,
  0 /* 0x80 */, 0 /* 0x81 */, 0 /* 0x82 */, 0 /* 0x83 */,
  0 /* 0x84 */, 0 /* 0x85 */, 0 /* 0x86 */, 0 /* 0x87 */,
  0 /* 0x88 */, 0 /* 0x89 */, 0 /* 0x8a */, 0 /* 0x8b */,
  0 /* 0x8c */, 0 /* 0x8d */, 0 /* 0x8e */, 0 /* 0x8f */,
  0 /* 0x90 */, 0 /* 0x91 */, 0 /* 0x92 */, 0 /* 0x93 */,
  0 /* 0x94 */, 0 /* 0x95 */, 0 /* 0x96 */, 0 /* 0x97 */,
  0 /* 0x98 */, 0 /* 0x99 */, 0 /* 0x9a */, 0 /* 0x9b */,
  0 /* 0x9c */, 0 /* 0x9d */, 0 /* 0x9e */, 0 /* 0x9f */,
  0 /* 0xa0 */, 0 /* 0xa1 */, 0 /* 0xa2 */, 0 /* 0xa3 */,
  0 /* 0xa4 */, 0 /* 0xa5 */, 0 /* 0xa6 */, 0 /* 0xa7 */,
  0 /* 0xa8 */, 0 /* 0xa9 */, 0 /* 0xaa */, 0 /* 0xab */,
  0 /* 0xac */, 0 /* 0xad */, 0 /* 0xae */, 0 /* 0xaf */,
  0 /* 0xb0 */, 0 /* 0xb1 */, 0 /* 0xb2 */, 0 /* 0xb3 */,
  0 /* 0xb4 */, 0 /* 0xb5 */, 0 /* 0xb6 */, 0 /* 0xb7 */,
  0 /* 0xb8 */, 0 /* 0xb9 */, 0 /* 0xba */, 0 /* 0xbb */,
  0 /* 0xbc */, 0 /* 0xbd */, 0 /* 0xbe */, 0 /* 0xbf */,
  0 /* 0xc0 */, 0 /* 0xc1 */, 0 /* 0xc2 */, 0 /* 0xc3 */,
  0 /* 0xc4 */, 0 /* 0xc5 */, 0 /* 0xc6 */, 0 /* 0xc7 */,
  0 /* 0xc8 */, 0 /* 0xc9 */, 0 /* 0xca */, 0 /* 0xcb */,
  0 /* 0xcc */, 0 /* 0xcd */, 0 /* 0xce */, 0 /* 0xcf */,
  0 /* 0xd0 */, 0 /* 0xd1 */, 0 /* 0xd2 */, 0 /* 0xd3 */,
  0 /* 0xd4 */, 0 /* 0xd5 */, 0 /* 0xd6 */, 0 /* 0xd7 */,
  0 /* 0xd8 */, 0 /* 0xd9 */, 0 /* 0xda */, 0 /* 0xdb */,
  0 /* 0xdc */, 0 /* 0xdd */, 0 /* 0xde */, 0 /* 0xdf */,
  0 /* 0xe0 */, 0 /* 0xe1 */, 0 /* 0xe2 */, 0 /* 0xe3 */,
  0 /* 0xe4 */, 0 /* 0xe5 */, 0 /* 0xe6 */, 0 /* 0xe7 */,
  0 /* 0xe8 */, 0 /* 0xe9 */, 0 /* 0xea */, 0 /* 0xeb */,
  0 /* 0xec */, 0 /* 0xed */, 0 /* 0xee */, 0 /* 0xef */,
  0 /* 0xf0 */, 0 /* 0xf1 */, 0 /* 0xf2 */, 0 /* 0xf3 */,
  0 /* 0xf4 */, 0 /* 0xf5 */, 0 /* 0xf6 */, 0 /* 0xf7 */,
  0 /* 0xf8 */, 0 /* 0xf9 */, 0 /* 0xfa */, 0 /* 0xfb */,
  0 /* 0xfc */, 0 /* 0xfd */, 0 /* 0xfe */, 0 /* 0xff */
};

static int check_authority(const uint8_t *value, size_t len) {
  const uint8_t *last;
  for (last = value + len; value != last; ++value) {
    if (!VALID_AUTHORITY_CHARS[*value]) {
      return 0;
    }
  }
  return 1;
}

static int check_scheme(const uint8_t *value, size_t len) {
  const uint8_t *last;
  if (len == 0) {
    return 0;
  }

  if (!(('A' <= *value && *value <= 'Z') || ('a' <= *value && *value <= 'z'))) {
    return 0;
  }

  last = value + len;
  ++value;

  for (; value != last; ++value) {
    if (!(('A' <= *value && *value <= 'Z') ||
          ('a' <= *value && *value <= 'z') ||
          ('0' <= *value && *value <= '9') || *value == '+' || *value == '-' ||
          *value == '.')) {
      return 0;
    }
  }
  return 1;
}

/* Generated by genmethodchartbl.py */
static char VALID_METHOD_CHARS[] = {
  0 /* NUL  */, 0 /* SOH  */, 0 /* STX  */, 0 /* ETX  */,
  0 /* EOT  */, 0 /* ENQ  */, 0 /* ACK  */, 0 /* BEL  */,
  0 /* BS   */, 0 /* HT   */, 0 /* LF   */, 0 /* VT   */,
  0 /* FF   */, 0 /* CR   */, 0 /* SO   */, 0 /* SI   */,
  0 /* DLE  */, 0 /* DC1  */, 0 /* DC2  */, 0 /* DC3  */,
  0 /* DC4  */, 0 /* NAK  */, 0 /* SYN  */, 0 /* ETB  */,
  0 /* CAN  */, 0 /* EM   */, 0 /* SUB  */, 0 /* ESC  */,
  0 /* FS   */, 0 /* GS   */, 0 /* RS   */, 0 /* US   */,
  0 /* SPC  */, 1 /* !    */, 0 /* "    */, 1 /* #    */,
  1 /* $    */, 1 /* %    */, 1 /* &    */, 1 /* '    */,
  0 /* (    */, 0 /* )    */, 1 /* *    */, 1 /* +    */,
  0 /* ,    */, 1 /* -    */, 1 /* .    */, 0 /* /    */,
  1 /* 0    */, 1 /* 1    */, 1 /* 2    */, 1 /* 3    */,
  1 /* 4    */, 1 /* 5    */, 1 /* 6    */, 1 /* 7    */,
  1 /* 8    */, 1 /* 9    */, 0 /* :    */, 0 /* ;    */,
  0 /* <    */, 0 /* =    */, 0 /* >    */, 0 /* ?    */,
  0 /* @    */, 1 /* A    */, 1 /* B    */, 1 /* C    */,
  1 /* D    */, 1 /* E    */, 1 /* F    */, 1 /* G    */,
  1 /* H    */, 1 /* I    */, 1 /* J    */, 1 /* K    */,
  1 /* L    */, 1 /* M    */, 1 /* N    */, 1 /* O    */,
  1 /* P    */, 1 /* Q    */, 1 /* R    */, 1 /* S    */,
  1 /* T    */, 1 /* U    */, 1 /* V    */, 1 /* W    */,
  1 /* X    */, 1 /* Y    */, 1 /* Z    */, 0 /* [    */,
  0 /* \    */, 0 /* ]    */, 1 /* ^    */, 1 /* _    */,
  1 /* `    */, 1 /* a    */, 1 /* b    */, 1 /* c    */,
  1 /* d    */, 1 /* e    */, 1 /* f    */, 1 /* g    */,
  1 /* h    */, 1 /* i    */, 1 /* j    */, 1 /* k    */,
  1 /* l    */, 1 /* m    */, 1 /* n    */, 1 /* o    */,
  1 /* p    */, 1 /* q    */, 1 /* r    */, 1 /* s    */,
  1 /* t    */, 1 /* u    */, 1 /* v    */, 1 /* w    */,
  1 /* x    */, 1 /* y    */, 1 /* z    */, 0 /* {    */,
  1 /* |    */, 0 /* }    */, 1 /* ~    */, 0 /* DEL  */,
  0 /* 0x80 */, 0 /* 0x81 */, 0 /* 0x82 */, 0 /* 0x83 */,
  0 /* 0x84 */, 0 /* 0x85 */, 0 /* 0x86 */, 0 /* 0x87 */,
  0 /* 0x88 */, 0 /* 0x89 */, 0 /* 0x8a */, 0 /* 0x8b */,
  0 /* 0x8c */, 0 /* 0x8d */, 0 /* 0x8e */, 0 /* 0x8f */,
  0 /* 0x90 */, 0 /* 0x91 */, 0 /* 0x92 */, 0 /* 0x93 */,
  0 /* 0x94 */, 0 /* 0x95 */, 0 /* 0x96 */, 0 /* 0x97 */,
  0 /* 0x98 */, 0 /* 0x99 */, 0 /* 0x9a */, 0 /* 0x9b */,
  0 /* 0x9c */, 0 /* 0x9d */, 0 /* 0x9e */, 0 /* 0x9f */,
  0 /* 0xa0 */, 0 /* 0xa1 */, 0 /* 0xa2 */, 0 /* 0xa3 */,
  0 /* 0xa4 */, 0 /* 0xa5 */, 0 /* 0xa6 */, 0 /* 0xa7 */,
  0 /* 0xa8 */, 0 /* 0xa9 */, 0 /* 0xaa */, 0 /* 0xab */,
  0 /* 0xac */, 0 /* 0xad */, 0 /* 0xae */, 0 /* 0xaf */,
  0 /* 0xb0 */, 0 /* 0xb1 */, 0 /* 0xb2 */, 0 /* 0xb3 */,
  0 /* 0xb4 */, 0 /* 0xb5 */, 0 /* 0xb6 */, 0 /* 0xb7 */,
  0 /* 0xb8 */, 0 /* 0xb9 */, 0 /* 0xba */, 0 /* 0xbb */,
  0 /* 0xbc */, 0 /* 0xbd */, 0 /* 0xbe */, 0 /* 0xbf */,
  0 /* 0xc0 */, 0 /* 0xc1 */, 0 /* 0xc2 */, 0 /* 0xc3 */,
  0 /* 0xc4 */, 0 /* 0xc5 */, 0 /* 0xc6 */, 0 /* 0xc7 */,
  0 /* 0xc8 */, 0 /* 0xc9 */, 0 /* 0xca */, 0 /* 0xcb */,
  0 /* 0xcc */, 0 /* 0xcd */, 0 /* 0xce */, 0 /* 0xcf */,
  0 /* 0xd0 */, 0 /* 0xd1 */, 0 /* 0xd2 */, 0 /* 0xd3 */,
  0 /* 0xd4 */, 0 /* 0xd5 */, 0 /* 0xd6 */, 0 /* 0xd7 */,
  0 /* 0xd8 */, 0 /* 0xd9 */, 0 /* 0xda */, 0 /* 0xdb */,
  0 /* 0xdc */, 0 /* 0xdd */, 0 /* 0xde */, 0 /* 0xdf */,
  0 /* 0xe0 */, 0 /* 0xe1 */, 0 /* 0xe2 */, 0 /* 0xe3 */,
  0 /* 0xe4 */, 0 /* 0xe5 */, 0 /* 0xe6 */, 0 /* 0xe7 */,
  0 /* 0xe8 */, 0 /* 0xe9 */, 0 /* 0xea */, 0 /* 0xeb */,
  0 /* 0xec */, 0 /* 0xed */, 0 /* 0xee */, 0 /* 0xef */,
  0 /* 0xf0 */, 0 /* 0xf1 */, 0 /* 0xf2 */, 0 /* 0xf3 */,
  0 /* 0xf4 */, 0 /* 0xf5 */, 0 /* 0xf6 */, 0 /* 0xf7 */,
  0 /* 0xf8 */, 0 /* 0xf9 */, 0 /* 0xfa */, 0 /* 0xfb */,
  0 /* 0xfc */, 0 /* 0xfd */, 0 /* 0xfe */, 0 /* 0xff */
};

static int check_method(const uint8_t *value, size_t len) {
  const uint8_t *last;
  if (len == 0) {
    return 0;
  }
  for (last = value + len; value != last; ++value) {
    if (!VALID_METHOD_CHARS[*value]) {
      return 0;
    }
  }
  return 1;
}

/* Generated by genpathchartbl.py */
static char VALID_PATH_CHARS[] = {
  0 /* NUL  */, 0 /* SOH  */, 0 /* STX  */, 0 /* ETX  */,
  0 /* EOT  */, 0 /* ENQ  */, 0 /* ACK  */, 0 /* BEL  */,
  0 /* BS   */, 0 /* HT   */, 0 /* LF   */, 0 /* VT   */,
  0 /* FF   */, 0 /* CR   */, 0 /* SO   */, 0 /* SI   */,
  0 /* DLE  */, 0 /* DC1  */, 0 /* DC2  */, 0 /* DC3  */,
  0 /* DC4  */, 0 /* NAK  */, 0 /* SYN  */, 0 /* ETB  */,
  0 /* CAN  */, 0 /* EM   */, 0 /* SUB  */, 0 /* ESC  */,
  0 /* FS   */, 0 /* GS   */, 0 /* RS   */, 0 /* US   */,
  0 /* SPC  */, 1 /* !    */, 1 /* "    */, 1 /* #    */,
  1 /* $    */, 1 /* %    */, 1 /* &    */, 1 /* '    */,
  1 /* (    */, 1 /* )    */, 1 /* *    */, 1 /* +    */,
  1 /* ,    */, 1 /* -    */, 1 /* .    */, 1 /* /    */,
  1 /* 0    */, 1 /* 1    */, 1 /* 2    */, 1 /* 3    */,
  1 /* 4    */, 1 /* 5    */, 1 /* 6    */, 1 /* 7    */,
  1 /* 8    */, 1 /* 9    */, 1 /* :    */, 1 /* ;    */,
  1 /* <    */, 1 /* =    */, 1 /* >    */, 1 /* ?    */,
  1 /* @    */, 1 /* A    */, 1 /* B    */, 1 /* C    */,
  1 /* D    */, 1 /* E    */, 1 /* F    */, 1 /* G    */,
  1 /* H    */, 1 /* I    */, 1 /* J    */, 1 /* K    */,
  1 /* L    */, 1 /* M    */, 1 /* N    */, 1 /* O    */,
  1 /* P    */, 1 /* Q    */, 1 /* R    */, 1 /* S    */,
  1 /* T    */, 1 /* U    */, 1 /* V    */, 1 /* W    */,
  1 /* X    */, 1 /* Y    */, 1 /* Z    */, 1 /* [    */,
  1 /* \    */, 1 /* ]    */, 1 /* ^    */, 1 /* _    */,
  1 /* `    */, 1 /* a    */, 1 /* b    */, 1 /* c    */,
  1 /* d    */, 1 /* e    */, 1 /* f    */, 1 /* g    */,
  1 /* h    */, 1 /* i    */, 1 /* j    */, 1 /* k    */,
  1 /* l    */, 1 /* m    */, 1 /* n    */, 1 /* o    */,
  1 /* p    */, 1 /* q    */, 1 /* r    */, 1 /* s    */,
  1 /* t    */, 1 /* u    */, 1 /* v    */, 1 /* w    */,
  1 /* x    */, 1 /* y    */, 1 /* z    */, 1 /* {    */,
  1 /* |    */, 1 /* }    */, 1 /* ~    */, 0 /* DEL  */,
  1 /* 0x80 */, 1 /* 0x81 */, 1 /* 0x82 */, 1 /* 0x83 */,
  1 /* 0x84 */, 1 /* 0x85 */, 1 /* 0x86 */, 1 /* 0x87 */,
  1 /* 0x88 */, 1 /* 0x89 */, 1 /* 0x8a */, 1 /* 0x8b */,
  1 /* 0x8c */, 1 /* 0x8d */, 1 /* 0x8e */, 1 /* 0x8f */,
  1 /* 0x90 */, 1 /* 0x91 */, 1 /* 0x92 */, 1 /* 0x93 */,
  1 /* 0x94 */, 1 /* 0x95 */, 1 /* 0x96 */, 1 /* 0x97 */,
  1 /* 0x98 */, 1 /* 0x99 */, 1 /* 0x9a */, 1 /* 0x9b */,
  1 /* 0x9c */, 1 /* 0x9d */, 1 /* 0x9e */, 1 /* 0x9f */,
  1 /* 0xa0 */, 1 /* 0xa1 */, 1 /* 0xa2 */, 1 /* 0xa3 */,
  1 /* 0xa4 */, 1 /* 0xa5 */, 1 /* 0xa6 */, 1 /* 0xa7 */,
  1 /* 0xa8 */, 1 /* 0xa9 */, 1 /* 0xaa */, 1 /* 0xab */,
  1 /* 0xac */, 1 /* 0xad */, 1 /* 0xae */, 1 /* 0xaf */,
  1 /* 0xb0 */, 1 /* 0xb1 */, 1 /* 0xb2 */, 1 /* 0xb3 */,
  1 /* 0xb4 */, 1 /* 0xb5 */, 1 /* 0xb6 */, 1 /* 0xb7 */,
  1 /* 0xb8 */, 1 /* 0xb9 */, 1 /* 0xba */, 1 /* 0xbb */,
  1 /* 0xbc */, 1 /* 0xbd */, 1 /* 0xbe */, 1 /* 0xbf */,
  1 /* 0xc0 */, 1 /* 0xc1 */, 1 /* 0xc2 */, 1 /* 0xc3 */,
  1 /* 0xc4 */, 1 /* 0xc5 */, 1 /* 0xc6 */, 1 /* 0xc7 */,
  1 /* 0xc8 */, 1 /* 0xc9 */, 1 /* 0xca */, 1 /* 0xcb */,
  1 /* 0xcc */, 1 /* 0xcd */, 1 /* 0xce */, 1 /* 0xcf */,
  1 /* 0xd0 */, 1 /* 0xd1 */, 1 /* 0xd2 */, 1 /* 0xd3 */,
  1 /* 0xd4 */, 1 /* 0xd5 */, 1 /* 0xd6 */, 1 /* 0xd7 */,
  1 /* 0xd8 */, 1 /* 0xd9 */, 1 /* 0xda */, 1 /* 0xdb */,
  1 /* 0xdc */, 1 /* 0xdd */, 1 /* 0xde */, 1 /* 0xdf */,
  1 /* 0xe0 */, 1 /* 0xe1 */, 1 /* 0xe2 */, 1 /* 0xe3 */,
  1 /* 0xe4 */, 1 /* 0xe5 */, 1 /* 0xe6 */, 1 /* 0xe7 */,
  1 /* 0xe8 */, 1 /* 0xe9 */, 1 /* 0xea */, 1 /* 0xeb */,
  1 /* 0xec */, 1 /* 0xed */, 1 /* 0xee */, 1 /* 0xef */,
  1 /* 0xf0 */, 1 /* 0xf1 */, 1 /* 0xf2 */, 1 /* 0xf3 */,
  1 /* 0xf4 */, 1 /* 0xf5 */, 1 /* 0xf6 */, 1 /* 0xf7 */,
  1 /* 0xf8 */, 1 /* 0xf9 */, 1 /* 0xfa */, 1 /* 0xfb */,
  1 /* 0xfc */, 1 /* 0xfd */, 1 /* 0xfe */, 1 /* 0xff */
};

static int check_path(const uint8_t *value, size_t len) {
  const uint8_t *last;
  for (last = value + len; value != last; ++value) {
    if (!VALID_PATH_CHARS[*value]) {
      return 0;
    }
  }
  return 1;
}

static int http_request_on_header(nghttp3_http_state *http,
                                  nghttp3_qpack_nv *nv, int trailers,
                                  int connect_protocol) {
  nghttp3_pri pri;

  switch (nv->token) {
  case NGHTTP3_QPACK_TOKEN__AUTHORITY:
    if (!check_authority(nv->value->base, nv->value->len) ||
        !check_pseudo_header(http, nv, NGHTTP3_HTTP_FLAG__AUTHORITY)) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    break;
  case NGHTTP3_QPACK_TOKEN__METHOD:
    if (!check_method(nv->value->base, nv->value->len) ||
        !check_pseudo_header(http, nv, NGHTTP3_HTTP_FLAG__METHOD)) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    switch (nv->value->len) {
    case 4:
      if (lstreq("HEAD", nv->value->base, nv->value->len)) {
        http->flags |= NGHTTP3_HTTP_FLAG_METH_HEAD;
      }
      break;
    case 7:
      switch (nv->value->base[6]) {
      case 'T':
        if (lstreq("CONNECT", nv->value->base, nv->value->len)) {
          http->flags |= NGHTTP3_HTTP_FLAG_METH_CONNECT;
        }
        break;
      case 'S':
        if (lstreq("OPTIONS", nv->value->base, nv->value->len)) {
          http->flags |= NGHTTP3_HTTP_FLAG_METH_OPTIONS;
        }
        break;
      }
      break;
    }
    break;
  case NGHTTP3_QPACK_TOKEN__PATH:
    if (!check_path(nv->value->base, nv->value->len) ||
        !check_pseudo_header(http, nv, NGHTTP3_HTTP_FLAG__PATH)) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    if (nv->value->base[0] == '/') {
      http->flags |= NGHTTP3_HTTP_FLAG_PATH_REGULAR;
    } else if (nv->value->len == 1 && nv->value->base[0] == '*') {
      http->flags |= NGHTTP3_HTTP_FLAG_PATH_ASTERISK;
    }
    break;
  case NGHTTP3_QPACK_TOKEN__SCHEME:
    if (!check_scheme(nv->value->base, nv->value->len) ||
        !check_pseudo_header(http, nv, NGHTTP3_HTTP_FLAG__SCHEME)) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    /* scheme is case-insensitive:
       https://datatracker.ietf.org/doc/html/rfc3986#section-3.1 */
    if (lstrieq("http", nv->value->base, nv->value->len) ||
        lstrieq("https", nv->value->base, nv->value->len)) {
      http->flags |= NGHTTP3_HTTP_FLAG_SCHEME_HTTP;
    }
    break;
  case NGHTTP3_QPACK_TOKEN__PROTOCOL:
    if (!connect_protocol ||
        !nghttp3_check_header_value(nv->value->base, nv->value->len) ||
        !check_pseudo_header(http, nv, NGHTTP3_HTTP_FLAG__PROTOCOL)) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    break;
  case NGHTTP3_QPACK_TOKEN_HOST:
    if (!check_authority(nv->value->base, nv->value->len)) {
      return NGHTTP3_ERR_REMOVE_HTTP_HEADER;
    }
    if (!check_pseudo_header(http, nv, NGHTTP3_HTTP_FLAG_HOST)) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    break;
  case NGHTTP3_QPACK_TOKEN_CONTENT_LENGTH: {
    /* https://tools.ietf.org/html/rfc7230#section-4.1.2: A sender
       MUST NOT generate a trailer that contains a field necessary for
       message framing (e.g., Transfer-Encoding and Content-Length),
       ... */
    if (trailers) {
      return NGHTTP3_ERR_REMOVE_HTTP_HEADER;
    }
    if (http->content_length != -1) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    http->content_length = parse_uint(nv->value->base, nv->value->len);
    if (http->content_length == -1) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    break;
  }
  /* disallowed header fields */
  case NGHTTP3_QPACK_TOKEN_CONNECTION:
  case NGHTTP3_QPACK_TOKEN_KEEP_ALIVE:
  case NGHTTP3_QPACK_TOKEN_PROXY_CONNECTION:
  case NGHTTP3_QPACK_TOKEN_TRANSFER_ENCODING:
  case NGHTTP3_QPACK_TOKEN_UPGRADE:
    return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
  case NGHTTP3_QPACK_TOKEN_TE:
    if (!lstrieq("trailers", nv->value->base, nv->value->len)) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    break;
  case NGHTTP3_QPACK_TOKEN_PRIORITY:
    if (!nghttp3_check_header_value(nv->value->base, nv->value->len)) {
      http->flags &= ~NGHTTP3_HTTP_FLAG_PRIORITY;
      http->flags |= NGHTTP3_HTTP_FLAG_BAD_PRIORITY;

      return NGHTTP3_ERR_REMOVE_HTTP_HEADER;
    }

    if (trailers || (http->flags & NGHTTP3_HTTP_FLAG_BAD_PRIORITY)) {
      break;
    }

    pri = http->pri;

    if (nghttp3_http_parse_priority(&pri, nv->value->base, nv->value->len) ==
        0) {
      http->pri = pri;
      http->flags |= NGHTTP3_HTTP_FLAG_PRIORITY;
      break;
    }

    http->flags &= ~NGHTTP3_HTTP_FLAG_PRIORITY;
    http->flags |= NGHTTP3_HTTP_FLAG_BAD_PRIORITY;

    break;
  default:
    if (nv->name->base[0] == ':') {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }

    if (!nghttp3_check_header_value(nv->value->base, nv->value->len)) {
      return NGHTTP3_ERR_REMOVE_HTTP_HEADER;
    }
  }

  return 0;
}

static int http_response_on_header(nghttp3_http_state *http,
                                   nghttp3_qpack_nv *nv, int trailers) {
  switch (nv->token) {
  case NGHTTP3_QPACK_TOKEN__STATUS: {
    if (!check_pseudo_header(http, nv, NGHTTP3_HTTP_FLAG__STATUS) ||
        nv->value->len != 3) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    http->status_code = (int16_t)parse_uint(nv->value->base, nv->value->len);
    if (http->status_code < 100 || http->status_code == 101) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    break;
  }
  case NGHTTP3_QPACK_TOKEN_CONTENT_LENGTH: {
    /* https://tools.ietf.org/html/rfc7230#section-4.1.2: A sender
       MUST NOT generate a trailer that contains a field necessary for
       message framing (e.g., Transfer-Encoding and Content-Length),
       ... */
    if (trailers) {
      return NGHTTP3_ERR_REMOVE_HTTP_HEADER;
    }
    if (http->status_code == 204) {
      /* content-length header field in 204 response is prohibited by
         RFC 7230.  But some widely used servers send content-length:
         0.  Until they get fixed, we ignore it. */
      if (/* Found multiple content-length field */
          http->content_length != -1 ||
          !lstrieq("0", nv->value->base, nv->value->len)) {
        return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
      }
      http->content_length = 0;
      return NGHTTP3_ERR_REMOVE_HTTP_HEADER;
    }
    if (http->status_code / 100 == 1 ||
        /* https://tools.ietf.org/html/rfc7230#section-3.3.3 */
        (http->status_code / 100 == 2 &&
         (http->flags & NGHTTP3_HTTP_FLAG_METH_CONNECT))) {
      return NGHTTP3_ERR_REMOVE_HTTP_HEADER;
    }
    if (http->content_length != -1) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    http->content_length = parse_uint(nv->value->base, nv->value->len);
    if (http->content_length == -1) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    break;
  }
  /* disallowed header fields */
  case NGHTTP3_QPACK_TOKEN_CONNECTION:
  case NGHTTP3_QPACK_TOKEN_KEEP_ALIVE:
  case NGHTTP3_QPACK_TOKEN_PROXY_CONNECTION:
  case NGHTTP3_QPACK_TOKEN_TRANSFER_ENCODING:
  case NGHTTP3_QPACK_TOKEN_UPGRADE:
    return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
  case NGHTTP3_QPACK_TOKEN_TE:
    if (!lstrieq("trailers", nv->value->base, nv->value->len)) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    break;
  default:
    if (nv->name->base[0] == ':') {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }

    if (!nghttp3_check_header_value(nv->value->base, nv->value->len)) {
      return NGHTTP3_ERR_REMOVE_HTTP_HEADER;
    }
  }

  return 0;
}

static int http_check_nonempty_header_name(const uint8_t *name, size_t len);

int nghttp3_http_on_header(nghttp3_http_state *http, nghttp3_qpack_nv *nv,
                           int request, int trailers, int connect_protocol) {
  if (nv->name->len == 0) {
    http->flags |= NGHTTP3_HTTP_FLAG_PSEUDO_HEADER_DISALLOWED;

    return NGHTTP3_ERR_REMOVE_HTTP_HEADER;
  }

  if (nv->name->base[0] == ':') {
    /* pseudo header must have a valid token. */
    if (nv->token == -1 || trailers ||
        (http->flags & NGHTTP3_HTTP_FLAG_PSEUDO_HEADER_DISALLOWED)) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
  } else {
    http->flags |= NGHTTP3_HTTP_FLAG_PSEUDO_HEADER_DISALLOWED;

    switch (http_check_nonempty_header_name(nv->name->base, nv->name->len)) {
    case 0:
      return NGHTTP3_ERR_REMOVE_HTTP_HEADER;
    case -1:
      /* header field name must be lower-cased without exception */
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
  }

  assert(nv->name->len > 0);

  if (request) {
    return http_request_on_header(http, nv, trailers, connect_protocol);
  }

  return http_response_on_header(http, nv, trailers);
}

int nghttp3_http_on_request_headers(nghttp3_http_state *http) {
  if (!(http->flags & NGHTTP3_HTTP_FLAG__PROTOCOL) &&
      (http->flags & NGHTTP3_HTTP_FLAG_METH_CONNECT)) {
    if ((http->flags & (NGHTTP3_HTTP_FLAG__SCHEME | NGHTTP3_HTTP_FLAG__PATH)) ||
        (http->flags & NGHTTP3_HTTP_FLAG__AUTHORITY) == 0) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    http->content_length = -1;
  } else {
    if ((http->flags & NGHTTP3_HTTP_FLAG_REQ_HEADERS) !=
          NGHTTP3_HTTP_FLAG_REQ_HEADERS ||
        (http->flags &
         (NGHTTP3_HTTP_FLAG__AUTHORITY | NGHTTP3_HTTP_FLAG_HOST)) == 0) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    if ((http->flags & NGHTTP3_HTTP_FLAG__PROTOCOL) &&
        ((http->flags & NGHTTP3_HTTP_FLAG_METH_CONNECT) == 0 ||
         (http->flags & NGHTTP3_HTTP_FLAG__AUTHORITY) == 0)) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
    if (!check_path_flags(http)) {
      return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
    }
  }

  return 0;
}

int nghttp3_http_on_response_headers(nghttp3_http_state *http) {
  if ((http->flags & NGHTTP3_HTTP_FLAG__STATUS) == 0) {
    return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
  }

  if (http->status_code / 100 == 1) {
    /* non-final response */
    http->flags = (http->flags & NGHTTP3_HTTP_FLAG_METH_ALL) |
                  NGHTTP3_HTTP_FLAG_EXPECT_FINAL_RESPONSE;
    http->content_length = -1;
    http->status_code = -1;
    return 0;
  }

  http->flags &= ~NGHTTP3_HTTP_FLAG_EXPECT_FINAL_RESPONSE;

  if (!expect_response_body(http)) {
    http->content_length = 0;
  } else if (http->flags & NGHTTP3_HTTP_FLAG_METH_CONNECT) {
    http->content_length = -1;
  }

  return 0;
}

int nghttp3_http_on_remote_end_stream(nghttp3_stream *stream) {
  if ((stream->rx.http.flags & NGHTTP3_HTTP_FLAG_EXPECT_FINAL_RESPONSE) ||
      (stream->rx.http.content_length != -1 &&
       stream->rx.http.content_length != stream->rx.http.recv_content_length)) {
    return NGHTTP3_ERR_MALFORMED_HTTP_MESSAGING;
  }

  return 0;
}

int nghttp3_http_on_data_chunk(nghttp3_stream *stream, size_t n) {
  stream->rx.http.recv_content_length += (int64_t)n;

  if ((stream->rx.http.flags & NGHTTP3_HTTP_FLAG_EXPECT_FINAL_RESPONSE) ||
      (stream->rx.http.content_length != -1 &&
       stream->rx.http.recv_content_length > stream->rx.http.content_length)) {
    return NGHTTP3_ERR_MALFORMED_HTTP_MESSAGING;
  }

  return 0;
}

void nghttp3_http_record_request_method(nghttp3_stream *stream,
                                        const nghttp3_nv *nva, size_t nvlen) {
  size_t i;
  const nghttp3_nv *nv;

  /* TODO we should do this strictly. */
  for (i = 0; i < nvlen; ++i) {
    nv = &nva[i];
    if (!(nv->namelen == 7 && nv->name[6] == 'd' &&
          memcmp(":metho", nv->name, nv->namelen - 1) == 0)) {
      continue;
    }
    if (lstreq("CONNECT", nv->value, nv->valuelen)) {
      stream->rx.http.flags |= NGHTTP3_HTTP_FLAG_METH_CONNECT;
      return;
    }
    if (lstreq("HEAD", nv->value, nv->valuelen)) {
      stream->rx.http.flags |= NGHTTP3_HTTP_FLAG_METH_HEAD;
      return;
    }
    return;
  }
}

/* Generated by gennmchartbl.py */
static const int VALID_HD_NAME_CHARS[] = {
  0 /* NUL  */, 0 /* SOH  */, 0 /* STX  */, 0 /* ETX  */, 0 /* EOT  */,
  0 /* ENQ  */, 0 /* ACK  */, 0 /* BEL  */, 0 /* BS   */, 0 /* HT   */,
  0 /* LF   */, 0 /* VT   */, 0 /* FF   */, 0 /* CR   */, 0 /* SO   */,
  0 /* SI   */, 0 /* DLE  */, 0 /* DC1  */, 0 /* DC2  */, 0 /* DC3  */,
  0 /* DC4  */, 0 /* NAK  */, 0 /* SYN  */, 0 /* ETB  */, 0 /* CAN  */,
  0 /* EM   */, 0 /* SUB  */, 0 /* ESC  */, 0 /* FS   */, 0 /* GS   */,
  0 /* RS   */, 0 /* US   */, 0 /* SPC  */, 1 /* !    */, 0 /* "    */,
  1 /* #    */, 1 /* $    */, 1 /* %    */, 1 /* &    */, 1 /* '    */,
  0 /* (    */, 0 /* )    */, 1 /* *    */, 1 /* +    */, 0 /* ,    */,
  1 /* -    */, 1 /* .    */, 0 /* /    */, 1 /* 0    */, 1 /* 1    */,
  1 /* 2    */, 1 /* 3    */, 1 /* 4    */, 1 /* 5    */, 1 /* 6    */,
  1 /* 7    */, 1 /* 8    */, 1 /* 9    */, 0 /* :    */, 0 /* ;    */,
  0 /* <    */, 0 /* =    */, 0 /* >    */, 0 /* ?    */, 0 /* @    */,
  -1 /* A   */, -1 /* B   */, -1 /* C   */, -1 /* D   */, -1 /* E   */,
  -1 /* F   */, -1 /* G   */, -1 /* H   */, -1 /* I   */, -1 /* J   */,
  -1 /* K   */, -1 /* L   */, -1 /* M   */, -1 /* N   */, -1 /* O   */,
  -1 /* P   */, -1 /* Q   */, -1 /* R   */, -1 /* S   */, -1 /* T   */,
  -1 /* U   */, -1 /* V   */, -1 /* W   */, -1 /* X   */, -1 /* Y   */,
  -1 /* Z   */, 0 /* [    */, 0 /* \    */, 0 /* ]    */, 1 /* ^    */,
  1 /* _    */, 1 /* `    */, 1 /* a    */, 1 /* b    */, 1 /* c    */,
  1 /* d    */, 1 /* e    */, 1 /* f    */, 1 /* g    */, 1 /* h    */,
  1 /* i    */, 1 /* j    */, 1 /* k    */, 1 /* l    */, 1 /* m    */,
  1 /* n    */, 1 /* o    */, 1 /* p    */, 1 /* q    */, 1 /* r    */,
  1 /* s    */, 1 /* t    */, 1 /* u    */, 1 /* v    */, 1 /* w    */,
  1 /* x    */, 1 /* y    */, 1 /* z    */, 0 /* {    */, 1 /* |    */,
  0 /* }    */, 1 /* ~    */, 0 /* DEL  */, 0 /* 0x80 */, 0 /* 0x81 */,
  0 /* 0x82 */, 0 /* 0x83 */, 0 /* 0x84 */, 0 /* 0x85 */, 0 /* 0x86 */,
  0 /* 0x87 */, 0 /* 0x88 */, 0 /* 0x89 */, 0 /* 0x8a */, 0 /* 0x8b */,
  0 /* 0x8c */, 0 /* 0x8d */, 0 /* 0x8e */, 0 /* 0x8f */, 0 /* 0x90 */,
  0 /* 0x91 */, 0 /* 0x92 */, 0 /* 0x93 */, 0 /* 0x94 */, 0 /* 0x95 */,
  0 /* 0x96 */, 0 /* 0x97 */, 0 /* 0x98 */, 0 /* 0x99 */, 0 /* 0x9a */,
  0 /* 0x9b */, 0 /* 0x9c */, 0 /* 0x9d */, 0 /* 0x9e */, 0 /* 0x9f */,
  0 /* 0xa0 */, 0 /* 0xa1 */, 0 /* 0xa2 */, 0 /* 0xa3 */, 0 /* 0xa4 */,
  0 /* 0xa5 */, 0 /* 0xa6 */, 0 /* 0xa7 */, 0 /* 0xa8 */, 0 /* 0xa9 */,
  0 /* 0xaa */, 0 /* 0xab */, 0 /* 0xac */, 0 /* 0xad */, 0 /* 0xae */,
  0 /* 0xaf */, 0 /* 0xb0 */, 0 /* 0xb1 */, 0 /* 0xb2 */, 0 /* 0xb3 */,
  0 /* 0xb4 */, 0 /* 0xb5 */, 0 /* 0xb6 */, 0 /* 0xb7 */, 0 /* 0xb8 */,
  0 /* 0xb9 */, 0 /* 0xba */, 0 /* 0xbb */, 0 /* 0xbc */, 0 /* 0xbd */,
  0 /* 0xbe */, 0 /* 0xbf */, 0 /* 0xc0 */, 0 /* 0xc1 */, 0 /* 0xc2 */,
  0 /* 0xc3 */, 0 /* 0xc4 */, 0 /* 0xc5 */, 0 /* 0xc6 */, 0 /* 0xc7 */,
  0 /* 0xc8 */, 0 /* 0xc9 */, 0 /* 0xca */, 0 /* 0xcb */, 0 /* 0xcc */,
  0 /* 0xcd */, 0 /* 0xce */, 0 /* 0xcf */, 0 /* 0xd0 */, 0 /* 0xd1 */,
  0 /* 0xd2 */, 0 /* 0xd3 */, 0 /* 0xd4 */, 0 /* 0xd5 */, 0 /* 0xd6 */,
  0 /* 0xd7 */, 0 /* 0xd8 */, 0 /* 0xd9 */, 0 /* 0xda */, 0 /* 0xdb */,
  0 /* 0xdc */, 0 /* 0xdd */, 0 /* 0xde */, 0 /* 0xdf */, 0 /* 0xe0 */,
  0 /* 0xe1 */, 0 /* 0xe2 */, 0 /* 0xe3 */, 0 /* 0xe4 */, 0 /* 0xe5 */,
  0 /* 0xe6 */, 0 /* 0xe7 */, 0 /* 0xe8 */, 0 /* 0xe9 */, 0 /* 0xea */,
  0 /* 0xeb */, 0 /* 0xec */, 0 /* 0xed */, 0 /* 0xee */, 0 /* 0xef */,
  0 /* 0xf0 */, 0 /* 0xf1 */, 0 /* 0xf2 */, 0 /* 0xf3 */, 0 /* 0xf4 */,
  0 /* 0xf5 */, 0 /* 0xf6 */, 0 /* 0xf7 */, 0 /* 0xf8 */, 0 /* 0xf9 */,
  0 /* 0xfa */, 0 /* 0xfb */, 0 /* 0xfc */, 0 /* 0xfd */, 0 /* 0xfe */,
  0 /* 0xff */,
};

int nghttp3_check_header_name(const uint8_t *name, size_t len) {
  const uint8_t *last;
  if (len == 0) {
    return 0;
  }
  if (*name == ':') {
    if (len == 1) {
      return 0;
    }
    ++name;
    --len;
  }
  for (last = name + len; name != last; ++name) {
    if (!VALID_HD_NAME_CHARS[*name]) {
      return 0;
    }
  }
  return 1;
}

/* http_check_nonempty_header_name validates regular header name
   pointed by |name| of length |len|.  |len| must be greater than
   zero.  This function returns 1 if it succeeds, or -1 if the name
   contains a character in [A-Z], otherwise 0. */
static int http_check_nonempty_header_name(const uint8_t *name, size_t len) {
  const uint8_t *last;
  int rv;

  for (last = name + len; name != last; ++name) {
    rv = VALID_HD_NAME_CHARS[*name];
    if (rv != 1) {
      return rv;
    }
  }

  return 1;
}

/* Generated by genvchartbl.py */
static const int VALID_HD_VALUE_CHARS[] = {
  0 /* NUL  */, 0 /* SOH  */, 0 /* STX  */, 0 /* ETX  */,
  0 /* EOT  */, 0 /* ENQ  */, 0 /* ACK  */, 0 /* BEL  */,
  0 /* BS   */, 1 /* HT   */, 0 /* LF   */, 0 /* VT   */,
  0 /* FF   */, 0 /* CR   */, 0 /* SO   */, 0 /* SI   */,
  0 /* DLE  */, 0 /* DC1  */, 0 /* DC2  */, 0 /* DC3  */,
  0 /* DC4  */, 0 /* NAK  */, 0 /* SYN  */, 0 /* ETB  */,
  0 /* CAN  */, 0 /* EM   */, 0 /* SUB  */, 0 /* ESC  */,
  0 /* FS   */, 0 /* GS   */, 0 /* RS   */, 0 /* US   */,
  1 /* SPC  */, 1 /* !    */, 1 /* "    */, 1 /* #    */,
  1 /* $    */, 1 /* %    */, 1 /* &    */, 1 /* '    */,
  1 /* (    */, 1 /* )    */, 1 /* *    */, 1 /* +    */,
  1 /* ,    */, 1 /* -    */, 1 /* .    */, 1 /* /    */,
  1 /* 0    */, 1 /* 1    */, 1 /* 2    */, 1 /* 3    */,
  1 /* 4    */, 1 /* 5    */, 1 /* 6    */, 1 /* 7    */,
  1 /* 8    */, 1 /* 9    */, 1 /* :    */, 1 /* ;    */,
  1 /* <    */, 1 /* =    */, 1 /* >    */, 1 /* ?    */,
  1 /* @    */, 1 /* A    */, 1 /* B    */, 1 /* C    */,
  1 /* D    */, 1 /* E    */, 1 /* F    */, 1 /* G    */,
  1 /* H    */, 1 /* I    */, 1 /* J    */, 1 /* K    */,
  1 /* L    */, 1 /* M    */, 1 /* N    */, 1 /* O    */,
  1 /* P    */, 1 /* Q    */, 1 /* R    */, 1 /* S    */,
  1 /* T    */, 1 /* U    */, 1 /* V    */, 1 /* W    */,
  1 /* X    */, 1 /* Y    */, 1 /* Z    */, 1 /* [    */,
  1 /* \    */, 1 /* ]    */, 1 /* ^    */, 1 /* _    */,
  1 /* `    */, 1 /* a    */, 1 /* b    */, 1 /* c    */,
  1 /* d    */, 1 /* e    */, 1 /* f    */, 1 /* g    */,
  1 /* h    */, 1 /* i    */, 1 /* j    */, 1 /* k    */,
  1 /* l    */, 1 /* m    */, 1 /* n    */, 1 /* o    */,
  1 /* p    */, 1 /* q    */, 1 /* r    */, 1 /* s    */,
  1 /* t    */, 1 /* u    */, 1 /* v    */, 1 /* w    */,
  1 /* x    */, 1 /* y    */, 1 /* z    */, 1 /* {    */,
  1 /* |    */, 1 /* }    */, 1 /* ~    */, 0 /* DEL  */,
  1 /* 0x80 */, 1 /* 0x81 */, 1 /* 0x82 */, 1 /* 0x83 */,
  1 /* 0x84 */, 1 /* 0x85 */, 1 /* 0x86 */, 1 /* 0x87 */,
  1 /* 0x88 */, 1 /* 0x89 */, 1 /* 0x8a */, 1 /* 0x8b */,
  1 /* 0x8c */, 1 /* 0x8d */, 1 /* 0x8e */, 1 /* 0x8f */,
  1 /* 0x90 */, 1 /* 0x91 */, 1 /* 0x92 */, 1 /* 0x93 */,
  1 /* 0x94 */, 1 /* 0x95 */, 1 /* 0x96 */, 1 /* 0x97 */,
  1 /* 0x98 */, 1 /* 0x99 */, 1 /* 0x9a */, 1 /* 0x9b */,
  1 /* 0x9c */, 1 /* 0x9d */, 1 /* 0x9e */, 1 /* 0x9f */,
  1 /* 0xa0 */, 1 /* 0xa1 */, 1 /* 0xa2 */, 1 /* 0xa3 */,
  1 /* 0xa4 */, 1 /* 0xa5 */, 1 /* 0xa6 */, 1 /* 0xa7 */,
  1 /* 0xa8 */, 1 /* 0xa9 */, 1 /* 0xaa */, 1 /* 0xab */,
  1 /* 0xac */, 1 /* 0xad */, 1 /* 0xae */, 1 /* 0xaf */,
  1 /* 0xb0 */, 1 /* 0xb1 */, 1 /* 0xb2 */, 1 /* 0xb3 */,
  1 /* 0xb4 */, 1 /* 0xb5 */, 1 /* 0xb6 */, 1 /* 0xb7 */,
  1 /* 0xb8 */, 1 /* 0xb9 */, 1 /* 0xba */, 1 /* 0xbb */,
  1 /* 0xbc */, 1 /* 0xbd */, 1 /* 0xbe */, 1 /* 0xbf */,
  1 /* 0xc0 */, 1 /* 0xc1 */, 1 /* 0xc2 */, 1 /* 0xc3 */,
  1 /* 0xc4 */, 1 /* 0xc5 */, 1 /* 0xc6 */, 1 /* 0xc7 */,
  1 /* 0xc8 */, 1 /* 0xc9 */, 1 /* 0xca */, 1 /* 0xcb */,
  1 /* 0xcc */, 1 /* 0xcd */, 1 /* 0xce */, 1 /* 0xcf */,
  1 /* 0xd0 */, 1 /* 0xd1 */, 1 /* 0xd2 */, 1 /* 0xd3 */,
  1 /* 0xd4 */, 1 /* 0xd5 */, 1 /* 0xd6 */, 1 /* 0xd7 */,
  1 /* 0xd8 */, 1 /* 0xd9 */, 1 /* 0xda */, 1 /* 0xdb */,
  1 /* 0xdc */, 1 /* 0xdd */, 1 /* 0xde */, 1 /* 0xdf */,
  1 /* 0xe0 */, 1 /* 0xe1 */, 1 /* 0xe2 */, 1 /* 0xe3 */,
  1 /* 0xe4 */, 1 /* 0xe5 */, 1 /* 0xe6 */, 1 /* 0xe7 */,
  1 /* 0xe8 */, 1 /* 0xe9 */, 1 /* 0xea */, 1 /* 0xeb */,
  1 /* 0xec */, 1 /* 0xed */, 1 /* 0xee */, 1 /* 0xef */,
  1 /* 0xf0 */, 1 /* 0xf1 */, 1 /* 0xf2 */, 1 /* 0xf3 */,
  1 /* 0xf4 */, 1 /* 0xf5 */, 1 /* 0xf6 */, 1 /* 0xf7 */,
  1 /* 0xf8 */, 1 /* 0xf9 */, 1 /* 0xfa */, 1 /* 0xfb */,
  1 /* 0xfc */, 1 /* 0xfd */, 1 /* 0xfe */, 1 /* 0xff */
};

#ifdef __AVX2__
static int contains_bad_header_value_char_avx2(const uint8_t *first,
                                               const uint8_t *last) {
  const __m256i ctll = _mm256_set1_epi8(0x00 - 1);
  const __m256i ctlr = _mm256_set1_epi8(0x1f + 1);
  const __m256i ht = _mm256_set1_epi8('\t');
  const __m256i del = _mm256_set1_epi8(0x7f);
  __m256i s, x;
  uint32_t m;

  for (; first != last; first += 32) {
    s = _mm256_loadu_si256((void *)first);

    x = _mm256_andnot_si256(
      _mm256_cmpeq_epi8(s, ht),
      _mm256_and_si256(_mm256_cmpgt_epi8(s, ctll), _mm256_cmpgt_epi8(ctlr, s)));
    x = _mm256_or_si256(_mm256_cmpeq_epi8(s, del), x);

    m = (uint32_t)_mm256_movemask_epi8(x);
    if (m) {
      return 1;
    }
  }

  return 0;
}
#endif /* __AVX2__ */

int nghttp3_check_header_value(const uint8_t *value, size_t len) {
  const uint8_t *last;
#ifdef __AVX2__
  const uint8_t *last32;
#endif /* __AVX2__ */

  switch (len) {
  case 0:
    return 1;
  case 1:
    if (is_ws(*value)) {
      return 0;
    }

    break;
  default:
    if (is_ws(*value) || is_ws(*(value + len - 1))) {
      return 0;
    }
  }

  last = value + len;

#ifdef __AVX2__
  if (len >= 32) {
    last32 = value + (len & ~0x1fu);
    if (contains_bad_header_value_char_avx2(value, last32)) {
      return 0;
    }

    value = last32;
  }
#endif /* __AVX2__ */

  for (; value != last; ++value) {
    if (!VALID_HD_VALUE_CHARS[*value]) {
      return 0;
    }
  }
  return 1;
}

int nghttp3_pri_eq(const nghttp3_pri *a, const nghttp3_pri *b) {
  return a->urgency == b->urgency && a->inc == b->inc;
}
