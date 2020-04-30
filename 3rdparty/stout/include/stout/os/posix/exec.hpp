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

#ifndef __STOUT_OS_POSIX_EXEC_HPP__
#define __STOUT_OS_POSIX_EXEC_HPP__

#include <stdlib.h> // For exit.
#include <unistd.h> // For fork, exec*.

#include <sys/wait.h> // For waitpid.

#include <string>
#include <vector>

#include <stout/none.hpp>
#include <stout/option.hpp>

#include <stout/os/raw/argv.hpp>
#include <stout/os/raw/environment.hpp>

namespace os {

inline Option<int> spawn(
    const std::string& file,
    const std::vector<std::string>& arguments)
{
  pid_t pid = ::fork();

  if (pid == -1) {
    return None();
  } else if (pid == 0) {
    // In child process.
    ::execvp(file.c_str(), os::raw::Argv(arguments));
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


inline int execvp(const char* file, char* const argv[])
{
  return ::execvp(file, argv);
}


inline int execvpe(const char* file, char** argv, char** envp)
{
  char** saved = os::raw::environment();

  *os::raw::environmentp() = envp;

  int result = execvp(file, argv);

  *os::raw::environmentp() = saved;

  return result;
}

} // namespace os {

#endif // __STOUT_OS_POSIX_EXEC_HPP__
