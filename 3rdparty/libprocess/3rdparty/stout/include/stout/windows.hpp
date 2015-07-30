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
#ifndef __STOUT_WINDOWS_PREPROCESSOR_HPP__
#define __STOUT_WINDOWS_PREPROCESSOR_HPP__

// Provides aliases to Windows-specific nuances.

// Normally defined in unistd.h.
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// Alias for method in stdio.h.
#define write(fd, buf, count) _write(fd, buf, count)

// Aliases for 'inet_pton' and 'inet_ntop'.
#define inet_pton(af, cp, buf) InetPton(af, cp, buf)
#define inet_ntop(af, cp, buf, len) InetNtop(af, cp, buf, len)

// TODO(aclemmer): Not defined on Windows.  This value is temporary.
#define MAXHOSTNAMELEN 64

// Macros associated with ::access, usually defined in unistd.h.
#define access(path, how) _access(path, how)
#define R_OK 0x4
#define W_OK 0x2
#define X_OK 0x0 // No such permission on Windows
#define F_OK 0x0

// Aliases for file access modes (defined in fcntl.h).
#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_RDWR _O_RDWR
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define O_APPEND _O_APPEND
// TODO(josephw): No equivalent for O_NONBLOCK or O_SYNC

// Alias for mkstemp (requires io.h).
#define mkstemp(path) _mktemp_s(path, strlen(path) + 1)

// Alias for realpath.
#define PATH_MAX MAX_PATH
#define realpath(path, resolved) _fullpath(resolved, path, PATH_MAX)

// Alias for mkdir (requires direct.h).
#define mkdir(path, mode) _mkdir(path)


#endif // __STOUT_WINDOWS_PREPROCESSOR_HPP__
