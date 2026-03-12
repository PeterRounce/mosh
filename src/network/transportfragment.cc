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

#include <cassert>

#include "compressor.h"
#include "src/crypto/crypto.h"
#include "src/crypto/byteorder.h"
#include "src/protobufs/transportinstruction.pb.h"
#include "src/util/fatal_assert.h"
#include "transportfragment.h"

using namespace Network;
using namespace TransportBuffers;

std::string Fragment::tostring( void )
{
  assert( initialized );

  std::string ret;
  ret.resize( frag_header_len );

  uint64_t net_id = htobe64( id );
  memcpy( &ret[0], &net_id, sizeof( net_id ) );

  fatal_assert(
    !( fragment_num & 0x8000 ) ); /* effective limit on size of a terminal screen change or buffered user input */
  uint16_t combined_fragment_num = ( final << 15 ) | fragment_num;
  uint16_t net_frag = htobe16( combined_fragment_num );
  memcpy( &ret[sizeof( net_id )], &net_frag, sizeof( net_frag ) );

  assert( ret.size() == frag_header_len );

  ret += contents;

  return ret;
}

Fragment::Fragment( const std::string& x )
  : id( -1 ), fragment_num( -1 ), final( false ), initialized( true ), contents()
{
  fatal_assert( x.size() >= frag_header_len );
  contents = std::string( x.begin() + frag_header_len, x.end() );

  uint64_t data64;
  uint16_t* data16 = (uint16_t*)x.data();
  memcpy( &data64, x.data(), sizeof( data64 ) );
  id = be64toh( data64 );
  fragment_num = be16toh( data16[4] );
  final = ( fragment_num & 0x8000 ) >> 15;
  fragment_num &= 0x7FFF;
}

bool FragmentAssembly::add_fragment( Fragment& frag )
{
  /* see if this is a totally new packet */
  if ( current_id != frag.id ) {
    fragments.clear();
    fragments.resize( frag.fragment_num + 1 );
    fragments.at( frag.fragment_num ) = frag;
    fragments_arrived = 1;
    fragments_total = -1; /* unknown */
    current_id = frag.id;
  } else { /* not a new packet */
    /* see if we already have this fragment */
    if ( ( fragments.size() > frag.fragment_num ) && ( fragments.at( frag.fragment_num ).initialized ) ) {
      /* make sure new version is same as what we already have */
      assert( fragments.at( frag.fragment_num ) == frag );
    } else {
      if ( (int)fragments.size() < frag.fragment_num + 1 ) {
        fragments.resize( frag.fragment_num + 1 );
      }
      fragments.at( frag.fragment_num ) = frag;
      fragments_arrived++;
    }
  }

  if ( frag.final ) {
    fragments_total = frag.fragment_num + 1;
    assert( (int)fragments.size() <= fragments_total );
    fragments.resize( fragments_total );
  }

  if ( fragments_total != -1 ) {
    assert( fragments_arrived <= fragments_total );
  }

  /* see if we're done */
  return fragments_arrived == fragments_total;
}

Instruction FragmentAssembly::get_assembly( void )
{
  assert( fragments_arrived == fragments_total );

  std::string encoded;

  for ( int i = 0; i < fragments_total; i++ ) {
    assert( fragments.at( i ).initialized );
    encoded += fragments.at( i ).contents;
  }

  Instruction ret;
  std::string decompressed = get_compressor().uncompress_str( encoded );
  if ( !ret.ParseFromString( decompressed ) ) {
    throw Crypto::CryptoException( "Failed to parse transport instruction." );
  }

  fragments.clear();
  fragments_arrived = 0;
  fragments_total = -1;

  return ret;
}

bool Fragment::operator==( const Fragment& x ) const
{
  return ( id == x.id ) && ( fragment_num == x.fragment_num ) && ( final == x.final )
         && ( initialized == x.initialized ) && ( contents == x.contents );
}

std::vector<Fragment> Fragmenter::make_fragments( const Instruction& inst, size_t MTU )
{
  MTU -= Fragment::frag_header_len;
  if ( ( inst.old_num() != last_instruction.old_num() ) || ( inst.new_num() != last_instruction.new_num() )
       || ( inst.ack_num() != last_instruction.ack_num() )
       || ( inst.throwaway_num() != last_instruction.throwaway_num() )
       || ( inst.chaff() != last_instruction.chaff() )
       || ( inst.protocol_version() != last_instruction.protocol_version() ) || ( last_MTU != MTU ) ) {
    next_instruction_id++;
  }

  if ( ( inst.old_num() == last_instruction.old_num() ) && ( inst.new_num() == last_instruction.new_num() ) ) {
    assert( inst.diff() == last_instruction.diff() );
  }

  last_instruction = inst;
  last_MTU = MTU;

  size_t serial_size = inst.ByteSizeLong();
  std::string serial_buf;
  serial_buf.resize( serial_size );
  inst.SerializeToArray( serial_buf.data(), serial_size );
  std::string payload = get_compressor().compress_str( serial_buf );
  uint16_t fragment_num = 0;
  std::vector<Fragment> ret;

  while ( !payload.empty() ) {
    std::string this_fragment;
    bool final = false;

    if ( payload.size() > MTU ) {
      this_fragment = std::string( payload.begin(), payload.begin() + MTU );
      payload = std::string( payload.begin() + MTU, payload.end() );
    } else {
      this_fragment = payload;
      payload.clear();
      final = true;
    }

    ret.push_back( Fragment( next_instruction_id, fragment_num++, final, this_fragment ) );
  }

  return ret;
}
