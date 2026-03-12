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

#ifndef PTY_COMPAT_HPP
#define PTY_COMPAT_HPP

#include "src/include/config.h"

#ifndef HAVE_FORKPTY
#define forkpty my_forkpty
#endif
#ifndef HAVE_CFMAKERAW
#define cfmakeraw my_cfmakeraw
#endif

pid_t my_forkpty( int* amaster, char* name, const struct termios* termp, const struct winsize* winp );

void my_cfmakeraw( struct termios* termios_p );

#endif
