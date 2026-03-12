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

/* Test suite for the base64 encode/decode and Base64Key class. */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "base64_vector.h"
#include "src/crypto/base64.h"
#include "src/crypto/crypto.h"
#include "src/crypto/prng.h"
#include "src/util/fatal_assert.h"

bool verbose = false;

static void test_base64_16byte_vectors( void )
{
  /* run through a test vector (16-byte values) */
  char encoded[25];
  uint8_t decoded[16];
  size_t b64_len = 24;
  size_t raw_len = 16;
  for ( base64_test_row* row = static_base64_vector; *row->native != '\0'; row++ ) {
    memset( encoded, '\0', sizeof encoded );
    memset( decoded, '\0', sizeof decoded );

    base64_encode( static_cast<const uint8_t*>( row->native ), raw_len, encoded, b64_len );
    fatal_assert( !memcmp( row->encoded, encoded, b64_len ) );

    size_t dec_len = 16;
    fatal_assert( base64_decode( row->encoded, b64_len, decoded, &dec_len ) );
    fatal_assert( dec_len == 16 );
    fatal_assert( !memcmp( row->native, decoded, sizeof decoded ) );
  }
  if ( verbose ) {
    printf( "16-byte validation PASSED\n" );
  }
}

static void test_base64_16byte_last_byte( void )
{
  /* try 0..255 in the last byte; make sure the final two characters are output properly */
  uint8_t source[16];
  char encoded[25];
  uint8_t decoded[16];
  size_t b64_len = 24;
  size_t raw_len = 16;

  memset( source, '\0', sizeof source );
  for ( int i = 0; i < 256; i++ ) {
    source[15] = i;
    base64_encode( source, raw_len, encoded, b64_len );

    size_t dec_len = 16;
    fatal_assert( base64_decode( encoded, b64_len, decoded, &dec_len ) );
    fatal_assert( dec_len == 16 );
    fatal_assert( !memcmp( source, decoded, sizeof decoded ) );
  }
  if ( verbose ) {
    printf( "16-byte last-byte PASSED\n" );
  }
}

static void test_base64_32byte_roundtrip( void )
{
  /* Test 32-byte (256-bit key) base64 encoding/decoding */
  PRNG prng;

  for ( int trial = 0; trial < 1024; trial++ ) {
    uint8_t source[32];
    prng.fill( source, sizeof source );

    char encoded[45]; /* 44 chars + null */
    memset( encoded, '\0', sizeof encoded );
    base64_encode( source, 32, encoded, 44 );

    uint8_t decoded[32];
    size_t dec_len = 32;
    fatal_assert( base64_decode( encoded, 44, decoded, &dec_len ) );
    fatal_assert( dec_len == 32 );
    fatal_assert( !memcmp( source, decoded, 32 ) );
  }
  if ( verbose ) {
    printf( "32-byte roundtrip PASSED\n" );
  }
}

static void test_base64_key_roundtrip( void )
{
  /* randomly try keys (32-byte Base64Key) */
  PRNG prng;
  for ( int i = 0; i < ( 1 << 17 ); i++ ) {
    Base64Key key1( prng );
    Base64Key key2( key1.printable_key() );
    fatal_assert( key1.printable_key() == key2.printable_key()
                  && !memcmp( key1.data(), key2.data(), Base64Key::KEY_LEN ) );
  }
  if ( verbose ) {
    printf( "Base64Key random PASSED\n" );
  }
}

static void test_bad_keys( void )
{
  /* test bad keys for Base64Key (43-char printable form for 32 bytes) */
  const char* bad_keys[] = {
    "",
    "AAAAAAAAAAAAAAAAAAAAAA",
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
    "~AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
    "AAAAAAAAAAAAAAAAAAAAA~AAAAAAAAAAAAAAAAAAAAA",
    NULL,
  };
  for ( const char** key = bad_keys; *key != NULL; key++ ) {
    bool got_exn = false;
    try {
      Crypto::Base64Key k { std::string( *key ) };
    } catch ( const Crypto::CryptoException& ) {
      got_exn = true;
    }
    fatal_assert( got_exn );
  }
  if ( verbose ) {
    printf( "bad-keys PASSED\n" );
  }
}

int main( int argc, char* argv[] )
{
  if ( argc >= 2 && strcmp( argv[1], "-v" ) == 0 ) {
    verbose = true;
  }

  try {
    test_base64_16byte_vectors();
    test_base64_16byte_last_byte();
    test_base64_32byte_roundtrip();
    test_base64_key_roundtrip();
    test_bad_keys();
  } catch ( const std::exception& e ) {
    fprintf( stderr, "Error: %s\r\n", e.what() );
    return 1;
  }
  return 0;
}
