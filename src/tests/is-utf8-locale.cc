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

#include <cstdio>

#include "src/util/locale_utils.h"

int main( int argc __attribute__( ( unused ) ), char** argv __attribute__( ( unused ) ) )
{
  set_native_locale();
  if ( !is_utf8_locale() ) {
    fprintf( stderr, "not a UTF-8 locale\n" );
    return 1;
  }
  return 0;
}
