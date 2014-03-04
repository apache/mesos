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
#ifndef __STOUT_OS_SHELL_HPP__
#define __STOUT_OS_SHELL_HPP__

#include <stdarg.h> // For va_list, va_start, etc.
#include <stdio.h> // For ferror, fgets, FILE, pclose, popen.

#include <ostream>
#include <string>

#include <stout/error.hpp>
#include <stout/format.hpp>
#include <stout/try.hpp>

namespace os {

// Runs a shell command formatted with varargs and return the return value
// of the command. Optionally, the output is returned via an argument.
// TODO(vinod): Pass an istream object that can provide input to the command.
inline Try<int> shell(std::ostream* os, const std::string fmt, ...)
{
  va_list args;
  va_start(args, fmt);

  const Try<std::string>& command = strings::internal::format(fmt, args);

  va_end(args);

  if (command.isError()) {
    return Error(command.error());
  }

  FILE* file;

  if ((file = popen(command.get().c_str(), "r")) == NULL) {
    return Error("Failed to run '" + command.get() + "'");
  }

  char line[1024];
  // NOTE(vinod): Ideally the if and while loops should be interchanged. But
  // we get a broken pipe error if we don't read the output and simply close.
  while (fgets(line, sizeof(line), file) != NULL) {
    if (os != NULL) {
      *os << line;
    }
  }

  if (ferror(file) != 0) {
    pclose(file); // Ignoring result since we already have an error.
    return Error("Error reading output of '" + command.get() + "'");
  }

  int status;
  if ((status = pclose(file)) == -1) {
    return Error("Failed to get status of '" + command.get() + "'");
  }

  return status;
}

} // namespace os {

#endif // __STOUT_OS_SHELL_HPP__
