// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_OS_POSIX_SHELL_HPP__
#define __STOUT_OS_POSIX_SHELL_HPP__

#include <stdarg.h> // For va_list, va_start, etc.
#include <stdio.h> // For ferror, fgets, FILE, pclose, popen.

#include <sys/wait.h> // For waitpid.

#include <ostream>
#include <string>

#include <glog/logging.h>

#include <stout/error.hpp>
#include <stout/format.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace os {

namespace Shell {

// Canonical constants used as platform-dependent args to `exec`
// calls. `name` is the command name, `arg0` is the first argument
// received by the callee, usually the command name and `arg1` is the
// second command argument received by the callee.

constexpr const char* name = "sh";
constexpr const char* arg0 = "sh";
constexpr const char* arg1 = "-c";

} // namespace Shell {


template <typename... T>
Try<std::string> shell(const std::string& fmt, const T&... t)
{
  const Try<std::string> command = strings::format(fmt, t...);
  if (command.isError()) {
    return Error(command.error());
  }

  FILE* file;
  std::ostringstream stdout;

  if ((file = popen(command->c_str(), "r")) == nullptr) {
    return Error("Failed to run '" + command.get() + "'");
  }

  char line[1024];
  // NOTE(vinod): Ideally the if and while loops should be interchanged. But
  // we get a broken pipe error if we don't read the output and simply close.
  while (fgets(line, sizeof(line), file) != nullptr) {
    stdout << line;
  }

  if (ferror(file) != 0) {
    pclose(file); // Ignoring result since we already have an error.
    return Error("Error reading output of '" + command.get() + "'");
  }

  int status;
  if ((status = pclose(file)) == -1) {
    return Error("Failed to get status of '" + command.get() + "'");
  }

  if (WIFSIGNALED(status)) {
    return Error(
        "Running '" + command.get() + "' was interrupted by signal '" +
        strsignal(WTERMSIG(status)) + "'");
  } else if ((WEXITSTATUS(status) != EXIT_SUCCESS)) {
    LOG(ERROR) << "Command '" << command.get()
               << "' failed; this is the output:\n" << stdout.str();
    return Error(
        "Failed to execute '" + command.get() + "'; the command was either "
        "not found or exited with a non-zero exit status: " +
        stringify(WEXITSTATUS(status)));
  }

  return stdout.str();
}


inline Option<int> system(const std::string& command)
{
  pid_t pid = ::fork();
  if (pid == -1) {
    return None();
  } else if (pid == 0) {
    // In child process.
    ::execlp(
        Shell::name, Shell::arg0, Shell::arg1, command.c_str(), (char*)nullptr);
    ::exit(127);
  } else {
    // In parent process.
    int status;
    while (::waitpid(pid, &status, 0) == -1) {
      if (errno != EINTR) {
        return None();
      }
    }

    return status;
  }
}

} // namespace os {

#endif // __STOUT_OS_POSIX_SHELL_HPP__
