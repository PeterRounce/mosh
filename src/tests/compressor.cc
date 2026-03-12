#include "src/network/compressor.h"
#include "src/util/fatal_assert.h"
#include <string>

int main()
{
  Network::Compressor& c = Network::get_compressor();

  std::string original = "Hello, mosh! This is a test of zstd compression.";
  std::string compressed = c.compress_str( original );
  std::string decompressed = c.uncompress_str( compressed );
  fatal_assert( decompressed == original );

  std::string large( 100000, 'x' );
  std::string large_c = c.compress_str( large );
  std::string large_d = c.uncompress_str( large_c );
  fatal_assert( large_d == large );

  return 0;
}
