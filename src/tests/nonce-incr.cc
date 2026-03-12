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

/* Tests that the Mosh network layer seems to be using unique nonces */

#include <cstdlib>
#include <iostream>
#include <set>

#include "src/network/network.h"

int main()
{
  std::set<uint64_t> nonces;
  const unsigned int NUM_EXAMPLES = 1000000;

  for ( unsigned int i = 0; i < NUM_EXAMPLES; i++ ) {
    Network::Packet packet( Network::TO_CLIENT, 0, 0, "test" );
    nonces.insert( packet.toMessage().nonce.val() );
  }

  for ( unsigned int i = 0; i < NUM_EXAMPLES; i++ ) {
    Network::Packet packet( Network::TO_SERVER, 0, 0, "test" );
    nonces.insert( packet.toMessage().nonce.val() );
  }

  for ( unsigned int i = 0; i < NUM_EXAMPLES; i++ ) {
    {
      Network::Packet packet( Network::TO_SERVER, 0, 0, "test" );
      nonces.insert( packet.toMessage().nonce.val() );
    }

    {
      Network::Packet packet( Network::TO_CLIENT, 0, 0, "test" );
      nonces.insert( packet.toMessage().nonce.val() );
    }
  }

  if ( nonces.size() == 4 * NUM_EXAMPLES ) {
    return EXIT_SUCCESS;
  }

  return EXIT_FAILURE;
}
