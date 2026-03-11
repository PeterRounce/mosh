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

#include "terminalframebuffer.h"
#include "fatal_assert.h"

int main()
{
  Terminal::Row row1( 80, 0 );
  Terminal::Row row2( 80, 0 );

  /* Identical rows should have the same hash. */
  fatal_assert( row1.hash() == row2.hash() );

  /* Reading hash again should return cached (same) value. */
  uint64_t h1 = row1.hash();
  uint64_t h1b = row1.hash();
  fatal_assert( h1 == h1b );

  /* Modifying a cell should change the hash. */
  row1.cells.at( 0 ).append( L'A' );
  row1.invalidate_hash();
  uint64_t h1c = row1.hash();
  fatal_assert( h1c != h1 );

  /* Reset should invalidate and produce blank-row hash again. */
  row1.reset( 0 );
  fatal_assert( row1.hash() == row2.hash() );

  /* Different widths should produce different hashes. */
  Terminal::Row row3( 40, 0 );
  fatal_assert( row3.hash() != row2.hash() );

  /* insert_cell should invalidate hash; put content in first cell so the
     shift is observable. */
  row2.cells.at( 0 ).append( L'X' );
  row2.invalidate_hash();
  uint64_t h2 = row2.hash();
  row2.insert_cell( 0, 0 );
  fatal_assert( row2.hash() != h2 );

  return 0;
}
