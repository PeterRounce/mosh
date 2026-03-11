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

#include "terminalframebuffer.h"
#include "fatal_assert.h"

int main()
{
  Terminal::Framebuffer fb( 80, 24 );

  uint64_t gen0 = fb.generation();

  /* Mutation should increment generation */
  fb.get_mutable_cell( 0, 0 )->reset( 0 );
  uint64_t gen1 = fb.generation();
  fatal_assert( gen1 > gen0 );

  /* Second read of generation without further mutation stays the same */
  uint64_t gen1b = fb.generation();
  fatal_assert( gen1b == gen1 );

  /* Resize should increment generation */
  fb.resize( 120, 40 );
  uint64_t gen2 = fb.generation();
  fatal_assert( gen2 > gen1 );

  /* reset() should increment generation */
  fb.reset();
  uint64_t gen3 = fb.generation();
  fatal_assert( gen3 > gen2 );

  /* scroll() should increment generation */
  uint64_t gen_before_scroll = fb.generation();
  fb.scroll( 1 );
  uint64_t gen4 = fb.generation();
  fatal_assert( gen4 > gen_before_scroll );

  /* insert_cell() should increment generation */
  uint64_t gen_before_insert = fb.generation();
  fb.insert_cell( 0, 0 );
  uint64_t gen5 = fb.generation();
  fatal_assert( gen5 > gen_before_insert );

  /* delete_cell() should increment generation */
  uint64_t gen_before_delete = fb.generation();
  fb.delete_cell( 0, 0 );
  uint64_t gen6 = fb.generation();
  fatal_assert( gen6 > gen_before_delete );

  /* copy constructor preserves generation */
  Terminal::Framebuffer fb2( fb );
  fatal_assert( fb2.generation() == fb.generation() );

  /* assignment preserves generation */
  Terminal::Framebuffer fb3( 80, 24 );
  fb3 = fb;
  fatal_assert( fb3.generation() == fb.generation() );

  return 0;
}
