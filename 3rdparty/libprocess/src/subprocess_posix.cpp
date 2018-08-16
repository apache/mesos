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
#include <unistd.h>

#include <string>

#include <glog/logging.h>

#include <process/future.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/os/pipe.hpp>
#include <stout/os/strerror.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

using std::array;
using std::map;
using std::string;
using std::vector;

namespace process {

using InputFileDescriptors = Subprocess::IO::InputFileDescriptors;
using OutputFileDescriptors = Subprocess::IO::OutputFileDescriptors;


Subprocess::IO Subprocess::PIPE()
{
  return Subprocess::IO(
      []() -> Try<InputFileDescriptors> {
        Try<array<int, 2>> pipefd = os::pipe();
        if (pipefd.isError()) {
          return Error(pipefd.error());
        }

        InputFileDescriptors fds;
        fds.read = pipefd->at(0);
        fds.write = pipefd->at(1);
        return fds;
      },
      []() -> Try<OutputFileDescriptors> {
        Try<array<int, 2>> pipefd = os::pipe();
        if (pipefd.isError()) {
          return Error(pipefd.error());
        }

        OutputFileDescriptors fds;
        fds.read = pipefd->at(0);
        fds.write = pipefd->at(1);
        return fds;
      });
}


Subprocess::IO Subprocess::PATH(const string& path)
{
  return Subprocess::IO(
      [path]() -> Try<InputFileDescriptors> {
        Try<int> open = os::open(path, O_RDONLY | O_CLOEXEC);
        if (open.isError()) {
          return Error("Failed to open '" + path + "': " + open.error());
        }

        InputFileDescriptors fds;
        fds.read = open.get();
        return fds;
      },
      [path]() -> Try<OutputFileDescriptors> {
        Try<int> open = os::open(
            path,
            O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

        if (open.isError()) {
          return Error("Failed to open '" + path + "': " + open.error());
        }

        OutputFileDescriptors fds;
        fds.write = open.get();
        return fds;
      });
}

}  // namespace process {
