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

#endif // __STOUT_WINDOWS_PREPROCESSOR_HPP__
