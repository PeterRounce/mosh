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

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

/* Test suite for the XChaCha20-Poly1305 crypto layer.

   Tests encrypt/decrypt round-trips at the Session level,
   verifies integrity check rejection of tampered ciphertexts,
   and exercises the Base64Key and Nonce classes. */

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "src/crypto/crypto.h"
#include "src/crypto/prng.h"
#include "src/util/fatal_assert.h"
#include "test_utils.h"

using namespace Crypto;

bool verbose = false;

static void test_base64_key_roundtrip( void )
{
  /* Generate a random key and verify base64 roundtrip */
  Base64Key key1;
  std::string printed = key1.printable_key();

  if ( verbose ) {
    printf( "key printable: %s (len %zu)\n", printed.c_str(), printed.length() );
  }

  fatal_assert( printed.length() == 43 );

  Base64Key key2( printed );
  fatal_assert( memcmp( key1.data(), key2.data(), Base64Key::KEY_LEN ) == 0 );

  if ( verbose ) {
    printf( "base64 key roundtrip PASSED\n\n" );
  }
}

static void test_nonce( void )
{
  /* Test nonce construction from uint64_t */
  Nonce n1( 0 );
  fatal_assert( n1.val() == 0 );

  Nonce n2( 42 );
  fatal_assert( n2.val() == 42 );

  Nonce n3( UINT64_MAX );
  fatal_assert( n3.val() == UINT64_MAX );

  /* Test nonce cc_str roundtrip */
  std::string nonce_str = n2.cc_str();
  fatal_assert( nonce_str.length() == 24 );

  Nonce n4( nonce_str.data(), nonce_str.length() );
  fatal_assert( n4.val() == 42 );

  if ( verbose ) {
    printf( "nonce PASSED\n\n" );
  }
}

static void test_encrypt_decrypt_roundtrip( void )
{
  PRNG prng;

  /* Test with various plaintext sizes */
  size_t sizes[] = { 0, 1, 15, 16, 17, 100, 256, 1024, 2000 };

  for ( size_t si = 0; si < sizeof( sizes ) / sizeof( sizes[0] ); si++ ) {
    size_t pt_len = sizes[si];

    Base64Key key;
    Session enc_session( key );
    Session dec_session( key );

    /* Generate random plaintext */
    std::string plaintext( pt_len, '\0' );
    if ( pt_len > 0 ) {
      prng.fill( &plaintext[0], pt_len );
    }

    uint64_t nonce_val = prng.uint64();
    Nonce nonce( nonce_val );

    if ( verbose ) {
      printf( "pt_len=%zu nonce=%" PRIu64 "\n", pt_len, nonce_val );
      hexdump( plaintext, "pt" );
    }

    /* Encrypt */
    std::string ciphertext = enc_session.encrypt( Message( nonce, plaintext ) );

    if ( verbose ) {
      hexdump( ciphertext, "ct" );
    }

    /* Decrypt */
    Message decrypted = dec_session.decrypt( ciphertext );

    fatal_assert( decrypted.nonce.val() == nonce_val );
    fatal_assert( decrypted.text == plaintext );

    if ( verbose ) {
      printf( "roundtrip pt_len=%zu PASSED\n\n", pt_len );
    }
  }
}

static void test_tampered_ciphertext( void )
{
  PRNG prng;
  Base64Key key;
  Session enc_session( key );
  Session dec_session( key );

  std::string plaintext = "Hello, world! This is a test message for integrity checking.";
  Nonce nonce( 12345 );

  std::string ciphertext = enc_session.encrypt( Message( nonce, plaintext ) );

  /* Try tampering with each byte position */
  for ( size_t trial = 0; trial < 64; trial++ ) {
    std::string bad_ct = ciphertext;
    size_t pos = prng.uint32() % bad_ct.size();
    bad_ct[pos] ^= ( 1 << ( prng.uint8() % 8 ) );

    bool got_exn = false;
    try {
      dec_session.decrypt( bad_ct );
    } catch ( const CryptoException& e ) {
      got_exn = true;
      fatal_assert( !e.fatal );
    }
    fatal_assert( got_exn );
  }

  if ( verbose ) {
    printf( "tampered ciphertext PASSED\n\n" );
  }
}

static void test_wrong_key( void )
{
  Base64Key key1;
  Base64Key key2;
  Session enc_session( key1 );
  Session dec_session( key2 );

  std::string plaintext = "Secret message";
  Nonce nonce( 1 );

  std::string ciphertext = enc_session.encrypt( Message( nonce, plaintext ) );

  bool got_exn = false;
  try {
    dec_session.decrypt( ciphertext );
  } catch ( const CryptoException& e ) {
    got_exn = true;
    fatal_assert( !e.fatal );
  }
  fatal_assert( got_exn );

  if ( verbose ) {
    printf( "wrong key PASSED\n\n" );
  }
}

static void test_session_copy( void )
{
  Base64Key key;
  Session original( key );

  std::string plaintext = "Test copy constructor";
  Nonce nonce( 99 );

  std::string ciphertext = original.encrypt( Message( nonce, plaintext ) );

  /* Copy should be able to decrypt */
  Session copy( original );
  Message decrypted = copy.decrypt( ciphertext );
  fatal_assert( decrypted.text == plaintext );
  fatal_assert( decrypted.nonce.val() == 99 );

  if ( verbose ) {
    printf( "session copy PASSED\n\n" );
  }
}

int main( int argc, char* argv[] )
{
  if ( argc >= 2 && strcmp( argv[1], "-v" ) == 0 ) {
    verbose = true;
  }

  try {
    test_base64_key_roundtrip();
    test_nonce();
    test_encrypt_decrypt_roundtrip();
    test_tampered_ciphertext();
    test_wrong_key();
    test_session_copy();
  } catch ( const std::exception& e ) {
    fprintf( stderr, "Error: %s\r\n", e.what() );
    return 1;
  }

  printf( "All crypto tests passed.\n" );
  return 0;
}
