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

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <sys/resource.h>

#include "src/crypto/base64.h"
#include "src/crypto/byteorder.h"
#include "src/crypto/crypto.h"
#include "src/crypto/prng.h"
#include "src/util/fatal_assert.h"

using namespace Crypto;

long int myatoi( const char* str )
{
  char* end;

  errno = 0;
  long int ret = strtol( str, &end, 10 );

  if ( ( errno != 0 ) || ( end != str + strlen( str ) ) ) {
    throw CryptoException( "Bad integer." );
  }

  return ret;
}

uint64_t Crypto::unique( void )
{
  static uint64_t counter = 0;
  uint64_t rv = counter++;
  if ( counter == 0 ) {
    throw CryptoException( "Counter wrapped", true );
  }
  return rv;
}

Base64Key::Base64Key( std::string printable_key )
{
  /* 32 bytes base64-encoded = 44 characters with padding, 43 without trailing '=' */
  if ( printable_key.length() != 43 ) {
    throw CryptoException( "Key must be 43 letters long." );
  }

  std::string base64 = printable_key + "=";

  size_t len = 32;
  if ( !base64_decode( base64.data(), 44, key, &len ) ) {
    throw CryptoException( "Key must be well-formed base64." );
  }

  if ( len != 32 ) {
    throw CryptoException( "Key must represent 32 octets." );
  }

  /* to catch changes after the first 256 bits */
  if ( printable_key != this->printable_key() ) {
    throw CryptoException( "Base64 key was not encoded 256-bit key." );
  }
}

Base64Key::Base64Key()
{
  PRNG().fill( key, sizeof( key ) );
}

Base64Key::Base64Key( PRNG& prng )
{
  prng.fill( key, sizeof( key ) );
}

std::string Base64Key::printable_key( void ) const
{
  char base64[45];

  base64_encode( key, 32, base64, 44 );

  if ( base64[43] != '=' ) {
    throw CryptoException( std::string( "Unexpected output from base64_encode: " ) + std::string( base64, 44 ) );
  }

  base64[43] = 0;
  return std::string( base64 );
}

static void ensure_sodium_init( void )
{
  static bool initialized = false;
  if ( !initialized ) {
    if ( sodium_init() < 0 ) {
      throw CryptoException( "Could not initialize libsodium.", true );
    }
    initialized = true;
  }
}

Session::Session( Base64Key s_key ) : blocks_encrypted( 0 )
{
  ensure_sodium_init();

  static_assert( sizeof( key ) == Base64Key::KEY_LEN, "Key size mismatch" );
  memcpy( key, s_key.data(), sizeof( key ) );
  sodium_mlock( key, sizeof( key ) );
}

Session::~Session()
{
  sodium_memzero( key, sizeof( key ) );
  sodium_munlock( key, sizeof( key ) );
}

Session::Session( const Session& other ) : blocks_encrypted( other.blocks_encrypted )
{
  memcpy( key, other.key, sizeof( key ) );
  sodium_mlock( key, sizeof( key ) );
}

Session& Session::operator=( const Session& other )
{
  if ( this != &other ) {
    sodium_memzero( key, sizeof( key ) );
    sodium_munlock( key, sizeof( key ) );
    memcpy( key, other.key, sizeof( key ) );
    sodium_mlock( key, sizeof( key ) );
    blocks_encrypted = other.blocks_encrypted;
  }
  return *this;
}

Nonce::Nonce( uint64_t val )
{
  uint64_t val_net = htobe64( val );

  memset( bytes, 0, 16 );
  memcpy( bytes + 16, &val_net, 8 );
}

uint64_t Nonce::val( void ) const
{
  uint64_t ret;
  memcpy( &ret, bytes + 16, 8 );
  return be64toh( ret );
}

Nonce::Nonce( const char* s_bytes, size_t len )
{
  if ( len != NONCE_LEN ) {
    throw CryptoException( "Nonce representation must be 24 octets long." );
  }

  memcpy( bytes, s_bytes, NONCE_LEN );
}

const std::string Session::encrypt( const Message& plaintext )
{
  const size_t pt_len = plaintext.text.size();
  const size_t ciphertext_len = pt_len + crypto_aead_xchacha20poly1305_ietf_ABYTES;

  std::string ciphertext( ciphertext_len, '\0' );
  unsigned long long actual_ciphertext_len = 0;

  if ( crypto_aead_xchacha20poly1305_ietf_encrypt(
         reinterpret_cast<unsigned char*>( &ciphertext[0] ),
         &actual_ciphertext_len,
         reinterpret_cast<const unsigned char*>( plaintext.text.data() ),
         pt_len,
         NULL, /* no additional data */
         0,    /* ad_len */
         NULL, /* nsec (unused) */
         reinterpret_cast<const unsigned char*>( plaintext.nonce.data() ),
         key )
       != 0 ) {
    throw CryptoException( "crypto_aead_xchacha20poly1305_ietf_encrypt() returned error." );
  }

  fatal_assert( actual_ciphertext_len == ciphertext_len );

  blocks_encrypted += pt_len >> 4;
  if ( pt_len & 0xF ) {
    blocks_encrypted++;
  }

  /* XChaCha20-Poly1305 has a much larger security margin than OCB,
     but we keep the same conservative limit as a safety measure.
     With 256-bit keys and 192-bit nonces, the actual limit is far higher. */
  if ( blocks_encrypted >> 47 ) {
    throw CryptoException( "Encrypted 2^47 blocks.", true );
  }

  return plaintext.nonce.cc_str() + ciphertext;
}

const Message Session::decrypt( const char* str, size_t len )
{
  if ( len < Nonce::NONCE_LEN + crypto_aead_xchacha20poly1305_ietf_ABYTES ) {
    throw CryptoException( "Ciphertext must contain nonce and tag." );
  }

  size_t body_len = len - Nonce::NONCE_LEN;
  size_t pt_len = body_len - crypto_aead_xchacha20poly1305_ietf_ABYTES;

  Nonce nonce( str, Nonce::NONCE_LEN );

  std::string plaintext( pt_len, '\0' );
  unsigned long long actual_pt_len = 0;

  if ( crypto_aead_xchacha20poly1305_ietf_decrypt(
         reinterpret_cast<unsigned char*>( &plaintext[0] ),
         &actual_pt_len,
         NULL, /* nsec (unused) */
         reinterpret_cast<const unsigned char*>( str + Nonce::NONCE_LEN ),
         body_len,
         NULL, /* no additional data */
         0,    /* ad_len */
         reinterpret_cast<const unsigned char*>( nonce.data() ),
         key )
       != 0 ) {
    throw CryptoException( "Packet failed integrity check." );
  }

  fatal_assert( actual_pt_len == pt_len );

  return Message( nonce, plaintext );
}

static rlim_t saved_core_rlimit;

/* Disable dumping core, as a precaution to avoid saving sensitive data
   to disk. */
void Crypto::disable_dumping_core( void )
{
  struct rlimit limit;
  if ( 0 != getrlimit( RLIMIT_CORE, &limit ) ) {
    /* We don't throw CryptoException because this is called very early
       in main(), outside of 'try'. */
    perror( "getrlimit(RLIMIT_CORE)" );
    exit( 1 );
  }

  saved_core_rlimit = limit.rlim_cur;
  limit.rlim_cur = 0;
  if ( 0 != setrlimit( RLIMIT_CORE, &limit ) ) {
    perror( "setrlimit(RLIMIT_CORE)" );
    exit( 1 );
  }
}

void Crypto::reenable_dumping_core( void )
{
  /* Silent failure is safe. */
  struct rlimit limit;
  if ( 0 == getrlimit( RLIMIT_CORE, &limit ) ) {
    limit.rlim_cur = saved_core_rlimit;
    setrlimit( RLIMIT_CORE, &limit );
  }
}
