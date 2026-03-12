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

#ifndef USER_HPP
#define USER_HPP

#include <cassert>
#include <cstdint>
#include <deque>
#include <list>
#include <string>
#include <string_view>

#include "src/terminal/parseraction.h"

namespace Network {
enum UserEventType
{
  UserByteType = 0,
  ResizeType = 1
};

class UserEvent
{
public:
  UserEventType type;
  Parser::UserByte userbyte;
  Parser::Resize resize;

  UserEvent( const Parser::UserByte& s_userbyte ) : type( UserByteType ), userbyte( s_userbyte ), resize( -1, -1 )
  {}
  UserEvent( const Parser::Resize& s_resize ) : type( ResizeType ), userbyte( 0 ), resize( s_resize ) {}

private:
  UserEvent();

public:
  bool operator==( const UserEvent& x ) const
  {
    return ( type == x.type ) && ( userbyte == x.userbyte ) && ( resize == x.resize );
  }
};

class UserStream
{
private:
  std::deque<UserEvent> actions;
  mutable uint64_t user_gen_counter_ = 0;

public:
  UserStream() : actions() {}

  void push_back( const Parser::UserByte& s_userbyte ) { actions.push_back( UserEvent( s_userbyte ) ); }
  void push_back( const Parser::Resize& s_resize ) { actions.push_back( UserEvent( s_resize ) ); }

  /* Always returns a new value so diff cache never hits for UserStream.
     This is intentional: UserStream is small and always needs fresh diffs. */
  uint64_t get_fb_generation() const { return ++user_gen_counter_; }

  bool empty( void ) const { return actions.empty(); }
  size_t size( void ) const { return actions.size(); }
  const Parser::Action& get_action( unsigned int i ) const;

  /* interface for Network::Transport */
  void subtract( const UserStream* prefix );
  void diff_from( const UserStream& existing, std::string* output ) const;
  void diff_from_priority( const UserStream& existing, std::string* output, int ) const
  {
    diff_from( existing, output );
  }
  void init_diff( std::string* output ) const { diff_from( UserStream(), output ); };
  void apply_string( std::string_view diff );
  bool operator==( const UserStream& x ) const { return actions == x.actions; }

  bool compare( const UserStream& ) { return false; }
};
}

#endif
