// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <string>

#include <glog/logging.h>

#include <process/future.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/open.hpp>
#include <stout/os/pipe.hpp>
#include <stout/os/strerror.hpp>

using std::array;
using std::string;

namespace process {

using InputFileDescriptors = Subprocess::IO::InputFileDescriptors;
using OutputFileDescriptors = Subprocess::IO::OutputFileDescriptors;

// Opens a pair of file handles. On success, the first handle returned receives
// the 'read' handle of the pipe, while the second receives the 'write' handle.
// The pipe handles can then be passed to a child process, as shown in [1].
//
// [1] https://msdn.microsoft.com/en-us/library/windows/desktop/ms682499(v=vs.85).aspx
Subprocess::IO Subprocess::PIPE()
{
  return Subprocess::IO(
      []() -> Try<InputFileDescriptors> {
        // Create STDIN pipe and set the 'read' component to be not overlapped,
        // because we're sending it to the child process.
        const Try<array<int_fd, 2>> pipefd = os::pipe(false, true);
        if (pipefd.isError()) {
          return Error(pipefd.error());
        }

        return InputFileDescriptors{pipefd.get()[0], pipefd.get()[1]};
      },
      []() -> Try<OutputFileDescriptors> {
        // Create STDOUT pipe and set the 'write' component to be not
        // overlapped, because we're sending it to the child process.
        const Try<array<int_fd, 2>> pipefd = os::pipe(true, false);
        if (pipefd.isError()) {
          return Error(pipefd.error());
        }

        return OutputFileDescriptors{pipefd.get()[0], pipefd.get()[1]};
      });
}


Subprocess::IO Subprocess::PATH(const string& path)
{
  return Subprocess::IO(
      [path]() -> Try<InputFileDescriptors> {
        const Try<int_fd> open = os::open(path, O_RDONLY);

        if (open.isError()) {
          return Error(open.error());
        }

        return InputFileDescriptors{open.get(), None()};
      },
      [path]() -> Try<OutputFileDescriptors> {
        const Try<int_fd> open = os::open(path, O_WRONLY | O_CREAT | O_APPEND);

        if (open.isError()) {
          return Error(open.error());
        }

        return OutputFileDescriptors{None(), open.get()};
      });
}

}  // namespace process {
