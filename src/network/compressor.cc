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

#include "compressor.h"
#include "src/crypto/crypto.h"
#include "src/util/fatal_assert.h"

using namespace Network;

Compressor::Compressor()
  : cctx_( ZSTD_createCCtx() ),
    dctx_( ZSTD_createDCtx() ),
    compress_buf_( INITIAL_BUF_SIZE ),
    decompress_buf_( INITIAL_BUF_SIZE )
{
  fatal_assert( cctx_ != nullptr );
  fatal_assert( dctx_ != nullptr );
}

Compressor::~Compressor()
{
  ZSTD_freeCCtx( cctx_ );
  ZSTD_freeDCtx( dctx_ );
}

std::string Compressor::compress_str( std::string_view input )
{
  size_t bound = ZSTD_compressBound( input.size() );
  if ( bound > compress_buf_.size() ) {
    compress_buf_.resize( bound );
  }
  size_t result = ZSTD_compressCCtx( cctx_, compress_buf_.data(), compress_buf_.size(), input.data(), input.size(), 1 );
  fatal_assert( !ZSTD_isError( result ) );
  return std::string( compress_buf_.data(), result );
}

std::string Compressor::uncompress_str( std::string_view input )
{
  static constexpr size_t MAX_DECOMPRESS_SIZE = 16 * 1024 * 1024; /* 16 MiB safety cap */
  unsigned long long frame_size = ZSTD_getFrameContentSize( input.data(), input.size() );
  size_t out_size = INITIAL_BUF_SIZE;
  if ( frame_size != ZSTD_CONTENTSIZE_UNKNOWN && frame_size != ZSTD_CONTENTSIZE_ERROR ) {
    if ( frame_size > MAX_DECOMPRESS_SIZE ) {
      throw Crypto::CryptoException( "Decompressed frame too large." );
    }
    out_size = static_cast<size_t>( frame_size );
  }
  if ( out_size > decompress_buf_.size() ) {
    decompress_buf_.resize( out_size );
  }
  size_t result
    = ZSTD_decompressDCtx( dctx_, decompress_buf_.data(), decompress_buf_.size(), input.data(), input.size() );
  if ( ZSTD_isError( result ) ) {
    throw Crypto::CryptoException( "Decompression failed." );
  }
  return std::string( decompress_buf_.data(), result );
}

/* construct on first use */
Compressor& Network::get_compressor( void )
{
  static Compressor the_compressor;
  return the_compressor;
}
