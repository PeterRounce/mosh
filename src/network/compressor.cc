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

#include "compressor.h"
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
  unsigned long long frame_size = ZSTD_getFrameContentSize( input.data(), input.size() );
  size_t out_size = INITIAL_BUF_SIZE;
  if ( frame_size != ZSTD_CONTENTSIZE_UNKNOWN && frame_size != ZSTD_CONTENTSIZE_ERROR ) {
    out_size = static_cast<size_t>( frame_size );
  }
  if ( out_size > decompress_buf_.size() ) {
    decompress_buf_.resize( out_size );
  }
  size_t result
    = ZSTD_decompressDCtx( dctx_, decompress_buf_.data(), decompress_buf_.size(), input.data(), input.size() );
  fatal_assert( !ZSTD_isError( result ) );
  return std::string( decompress_buf_.data(), result );
}

/* construct on first use */
Compressor& Network::get_compressor( void )
{
  static Compressor the_compressor;
  return the_compressor;
}
