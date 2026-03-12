/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <cstdlib>
#include <cstring>

#include "src/crypto/base64.h"
#include "src/util/fatal_assert.h"

static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const unsigned char reverse[] = {
  // clang-format off
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3e, 0xff, 0xff, 0xff, 0x3f,
  0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
  0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
  0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  // clang-format on
};

/* Reverse maps from an ASCII char to a base64 sixbit value.  Returns > 0x3f on failure. */
static unsigned char base64_char_to_sixbit( unsigned char c )
{
  return reverse[c];
}

bool base64_decode( const char* b64, const size_t b64_len, uint8_t* raw, size_t* raw_len )
{
  /* b64_len must be a multiple of 4 */
  if ( b64_len % 4 != 0 || b64_len == 0 ) {
    return false;
  }

  /* Count padding */
  size_t pad = 0;
  if ( b64[b64_len - 1] == '=' ) {
    pad++;
  }
  if ( b64_len >= 2 && b64[b64_len - 2] == '=' ) {
    pad++;
  }

  size_t expected_raw_len = ( b64_len / 4 ) * 3 - pad;
  if ( *raw_len < expected_raw_len ) {
    return false;
  }
  *raw_len = expected_raw_len;

  size_t data_chars = b64_len - pad;
  size_t full_groups = data_chars / 4;
  size_t remaining = data_chars % 4;

  const char* src = b64;
  uint8_t* dst = raw;

  /* Process full groups of 4 chars -> 3 bytes */
  for ( size_t i = 0; i < full_groups; i++ ) {
    unsigned char a = base64_char_to_sixbit( src[0] );
    unsigned char b = base64_char_to_sixbit( src[1] );
    unsigned char c = base64_char_to_sixbit( src[2] );
    unsigned char d = base64_char_to_sixbit( src[3] );
    if ( a > 0x3f || b > 0x3f || c > 0x3f || d > 0x3f ) {
      return false;
    }
    dst[0] = ( a << 2 ) | ( b >> 4 );
    dst[1] = ( b << 4 ) | ( c >> 2 );
    dst[2] = ( c << 6 ) | d;
    src += 4;
    dst += 3;
  }

  /* Process remaining chars (partial group before padding) */
  if ( remaining == 3 ) {
    /* 3 data chars + 1 pad = 2 bytes */
    unsigned char a = base64_char_to_sixbit( src[0] );
    unsigned char b = base64_char_to_sixbit( src[1] );
    unsigned char c = base64_char_to_sixbit( src[2] );
    if ( a > 0x3f || b > 0x3f || c > 0x3f ) {
      return false;
    }
    dst[0] = ( a << 2 ) | ( b >> 4 );
    dst[1] = ( b << 4 ) | ( c >> 2 );
  } else if ( remaining == 2 ) {
    /* 2 data chars + 2 pad = 1 byte */
    unsigned char a = base64_char_to_sixbit( src[0] );
    unsigned char b = base64_char_to_sixbit( src[1] );
    if ( a > 0x3f || b > 0x3f ) {
      return false;
    }
    dst[0] = ( a << 2 ) | ( b >> 4 );
  }

  return true;
}

void base64_encode( const uint8_t* raw, const size_t raw_len, char* b64, const size_t b64_len )
{
  size_t expected_b64_len = ( ( raw_len + 2 ) / 3 ) * 4;
  fatal_assert( b64_len >= expected_b64_len );

  const uint8_t* src = raw;
  char* dst = b64;
  size_t full_groups = raw_len / 3;
  size_t remaining = raw_len % 3;

  /* Process full groups of 3 bytes -> 4 chars */
  for ( size_t i = 0; i < full_groups; i++ ) {
    uint32_t bytes = ( (uint32_t)src[0] << 16 ) | ( (uint32_t)src[1] << 8 ) | (uint32_t)src[2];
    dst[0] = table[( bytes >> 18 ) & 0x3f];
    dst[1] = table[( bytes >> 12 ) & 0x3f];
    dst[2] = table[( bytes >> 6 ) & 0x3f];
    dst[3] = table[bytes & 0x3f];
    src += 3;
    dst += 4;
  }

  /* Process remaining bytes */
  if ( remaining == 2 ) {
    uint32_t bytes = ( (uint32_t)src[0] << 16 ) | ( (uint32_t)src[1] << 8 );
    dst[0] = table[( bytes >> 18 ) & 0x3f];
    dst[1] = table[( bytes >> 12 ) & 0x3f];
    dst[2] = table[( bytes >> 6 ) & 0x3f];
    dst[3] = '=';
  } else if ( remaining == 1 ) {
    uint32_t bytes = (uint32_t)src[0] << 16;
    dst[0] = table[( bytes >> 18 ) & 0x3f];
    dst[1] = table[( bytes >> 12 ) & 0x3f];
    dst[2] = '=';
    dst[3] = '=';
  }
}
