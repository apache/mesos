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


#include <direct.h> // For `_mkdir`.
#include <fcntl.h>  // For file access flags like `_O_CREAT`.
#include <io.h>     // For `_read`, `_write`.

#include <BaseTsd.h> // For `SSIZE_T`.
// We include `Winsock2.h` before `Windows.h` explicitly to avoid symbold
// re-definitions. This is a known pattern in the windows community.
#include <Winsock2.h>
#include <Windows.h>


// Definitions and constants used for Windows compat.
//
// Gathers most of the Windows-compatibility definitions.  This makes it
// possible for files throughout the codebase to remain relatively free of all
// the #if's we'd need to make them work.
//
// Roughly, the things that should go in this file are definitions and
// declarations that one would not mind being:
//   * in global scope.
//   * globally available throughout both the Stout codebase, and any code
//     that uses it (such as Mesos).


// Define constants used for Windows compat. Allows a lot of code on
// Windows and POSIX systems to be the same, because we can pass the
// same constants to functions we call to do things like file I/O.
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define R_OK 0x4
#define W_OK 0x2
#define X_OK 0x0 // No such permission on Windows.
#define F_OK 0x0

#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_RDWR _O_RDWR
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define O_APPEND _O_APPEND
#define O_CLOEXEC _O_NOINHERIT

// TODO(hausdorff): (MESOS-3398) Not defined on Windows. This value is
// temporary.
#define MAXHOSTNAMELEN 64

#define PATH_MAX _MAX_PATH

// Corresponds to `mode_t` defined in sys/types.h of the POSIX spec.
// See large "permissions API" comment below for an explanation of
// why this is an int instead of unsigned short (as is common on
// POSIX).
typedef int mode_t;

// `DWORD` is expected to be the type holding PIDs throughout the Windows API,
// including functions like `OpenProcess`.
typedef DWORD pid_t;

typedef SSIZE_T ssize_t;

// Socket flags. Define behavior of a socket when it (e.g.) shuts down. We map
// the Windows versions of these flags to their POSIX equivalents so we don't
// have to change any socket code.
constexpr int SHUT_RD = SD_RECEIVE;

// File I/O function aliases.
//
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


// Filesystem function aliases.
inline auto mkdir(const char* path, mode_t mode) ->
decltype(_mkdir(path))
{
  return _mkdir(path);
}


inline auto mkstemp(char* path) ->
decltype(_mktemp_s(path, strlen(path) + 1))
{
  return _mktemp_s(path, strlen(path) + 1);
}



inline auto realpath(const char* path, char* resolved) ->
decltype(_fullpath(resolved, path, PATH_MAX))
{
  return _fullpath(resolved, path, PATH_MAX);
}


inline auto access(const char* fileName, int accessMode) ->
decltype(_access(fileName, accessMode))
{
  return _access(fileName, accessMode);
}


// Permissions API. (cf. MESOS-3176 to track ongoing permissions work.)
//
// We are currently able to emulate a subset of the POSIX permissions model
// with the Windows model:
//   [x] User write permissions.
//   [x] User read permissions.
//   [ ] User execute permissions.
//   [ ] Group permissions of any sort.
//   [ ] Other permissions of any sort.
//   [x] Flags to control "fallback" behavior (e.g., we might choose
//       to fall back to user readability when the user passes the
//       group readability flag in, since we currently do not support
//       group readability).
//
//
// Rationale:
// Windows currently implements two permissions models: (1) an extremely
// primitive permission model it largely inherited from DOS, and (2) the Access
// Control List (ACL) API. Because there is no trivial way to map the classic
// POSIX model into the ACL model, we have implemented POSIX-style permissions
// in terms of the DOS model. The result is the permissions limitations above.
//
//
// Flag implementation:
// Flags fall into the following two categories.
//   (1) Flags which exist in both permission models, but which have
//       different names (e.g., `S_IRUSR` in POSIX is called `_S_IREAD` on
//       Windows). In this case, we define the POSIX name to be the Windows
//       value (e.g., we define `S_IRUSR` to have the same value as `_S_IREAD`),
//       so that we can pass the POSIX name into Windows functions like
//       `_open`.
//   (2) Flags which exist only on POSIX (e.g., `S_IXUSR`). Here we
//       define the POSIX name to be the value given in the glibc
//       documentation[1], shifted left by 16 bits (since `mode_t`
//       is unsigned short on POSIX and `int` on Windows). We give these
//       flags glibc values to stay consistent, and so that existing
//       calls to functions like `open` do not break when they try to
//       use a flag that doesn't exist on Windows. But, of course,
//       these flags do not affect the execution of these functions.
//
//
// Flag strictness:
// Because the current implementation does not directly support setting or
// getting group or other permission bits on the Windows platform, there is a
// question of what we should fall back to when these flags are passed in to
// Stout methods.
//
// TODO(hausdorff): Investigate permissions mappings.
// We force "strictness" of the permission flag semantics:
//   * The group permissions flags will not fall back to anything, and will be
//     completely ignored.
//   * Other permissions: Same as above, but with other permissions.
//
//
// Execute permissions:
// Because DOS has no notion of "execute permissions", we define execute
// permissions to be read permissions. This is not ideal, but it is closest to
// being accurate.
//
//
// [1] http://www.delorie.com/gnu/docs/glibc/libc_288.html


// User permission flags.
const mode_t S_IRUSR = mode_t(_S_IREAD);  // Readable by user.
const mode_t S_IWUSR = mode_t(_S_IWRITE); // Writeable by user.
const mode_t S_IXUSR = S_IRUSR;           // Fallback to user read.
const mode_t S_IRWXU = S_IRUSR | S_IWUSR | S_IXUSR;


// Group permission flags. Lossy mapping to Windows permissions. See
// note above about flag strictness for explanation.
const mode_t S_IRGRP = 0x00200000;        // No-op.
const mode_t S_IWGRP = 0x00100000;        // No-op.
const mode_t S_IXGRP = 0x00080000;        // No-op.
const mode_t S_IRWXG = S_IRGRP | S_IWGRP | S_IXGRP;


// Other permission flags. Lossy mapping to Windows permissions. See
// note above about flag stictness for explanation.
const mode_t S_IROTH = 0x00040000;        // No-op.
const mode_t S_IWOTH = 0x00020000;        // No-op.
const mode_t S_IXOTH = 0x00010000;        // No-op.
const mode_t S_IRWXO = S_IROTH | S_IWOTH | S_IXOTH;


// Flags for set-ID-on-exec.
const mode_t S_ISUID = 0x08000000;        // No-op.
const mode_t S_ISGID = 0x04000000;        // No-op.
const mode_t S_ISVTX = 0x02000000;        // No-op.


#endif // __STOUT_WINDOWS_HPP__
