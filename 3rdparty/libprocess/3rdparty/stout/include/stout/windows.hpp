/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_WINDOWS_HPP__
#define __STOUT_WINDOWS_HPP__


// Definitions and constants used for Windows compat.
//
// Gathers most of the Windows-compatibility definitions. This makes it
// possible for files throughout the codebase to remain relatively free of all
// the #if's we'd need to make them work.
//
// Roughly, the things that should go in this file are definitions and
// declarations that one would not mind being:
//   * in global scope.
//   * globally available throughout both the Stout codebase, and any code
//     that uses it (such as Mesos).

// Normally defined in unistd.h.
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2


// NOTE: The use of `auto` and the trailing return type in the following
// functions are meant to make it easier for Linux developers to use and
// maintain the code. It is an explicit marker that we are using the compiler
// to guarantee that the return type is identical to whatever is in the Windows
// implementation of the standard.
inline auto write(int fd, const void* buffer, size_t count) ->
decltype(_write(fd, buffer, count))
{
  return _write(fd, buffer, count);
}


inline auto open(const char* path, int flags) ->
decltype(_open(path, flags))
{
  return _open(path, flags);
}


inline auto close(int fd) ->
decltype(_close(fd))
{
  return _close(fd);
}


// TODO(aclemmer): (MESOS-3398) Not defined on Windows. This value is temporary.
#define MAXHOSTNAMELEN 64


inline auto access(const char* fileName, int accessMode) ->
decltype(_access(fileName, accessMode))
{
  return _access(fileName, accessMode);
}
#define R_OK 0x4
#define W_OK 0x2
#define X_OK 0x0 // No such permission on Windows.
#define F_OK 0x0

// Aliases for file access modes.
#include <fcntl.h>
#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_RDWR _O_RDWR
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define O_APPEND _O_APPEND
// TODO(josephw): No equivalent for O_NONBLOCK or O_SYNC.


// Alias for mkstemp (requires io.h).
inline auto mkstemp(char* path) ->
decltype(_mktemp_s(path, strlen(path) + 1))
{
  return _mktemp_s(path, strlen(path) + 1);
}


// Alias for realpath.
#define PATH_MAX _MAX_PATH


inline auto realpath(const char* path, char* resolved) ->
decltype(_fullpath(resolved, path, PATH_MAX))
{
  return _fullpath(resolved, path, PATH_MAX);
}


// Corresponds to `mode_t` defined in sys/types.h of the POSIX spec.
// See note above for an explanation of why this is an int instead of
// unsigned short (as is common on POSIX).
typedef int mode_t;


// Alias for mkdir (requires direct.h).
inline auto mkdir(const char* path, mode_t mode) ->
decltype(_mkdir(path))
{
  return _mkdir(path);
}


#endif // __STOUT_WINDOWS_HPP__
