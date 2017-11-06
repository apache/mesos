// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __MESOS_SLAVE_CONTAINERIZER_HPP__
#define __MESOS_SLAVE_CONTAINERIZER_HPP__

#include <string>

#include <process/shared.hpp>
#include <process/subprocess.hpp>

#include <stout/option.hpp>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include <mesos/slave/containerizer.pb.h>

namespace mesos {
namespace slave {

/**
 * An abstraction around the IO classes used to redirect
 * stdin/stdout/stderr to/from a container by the containerizer.
 */
struct ContainerIO
{
  /**
   * Describes how the containerizer redirects I/O for
   * stdin/stdout/stderr of a container.
   * See `process::Subprocess::IO`.
   *
   * NOTE: This wrapper prevents the containerizer from redirecting I/O
   * via a `Subprocess::PIPE`. This is restricted because the IO of a
   * container must not be affected by the status of the agent process:
   *   * A `Subprocess::PIPE` will require the agent process to regularly
   *     read and empty the pipe. The agent does not do this. If the pipe
   *     fills up, the write-end of the pipe may become blocked on IO.
   *   * Redirection must continue even if the agent dies.
   */
  class IO
  {
  public:
    enum class Type
    {
      FD,
      PATH
    };

    static IO PATH(const std::string& path)
    {
      return IO(Type::PATH, path);
    }

    static IO FD(int_fd fd, bool closeOnDestruction = true)
    {
      return IO(Type::FD, fd, closeOnDestruction);
    }

    operator process::Subprocess::IO () const
    {
      switch (type_) {
        case Type::FD:
          return process::Subprocess::FD(*fd_);
        case Type::PATH:
          return process::Subprocess::PATH(path_.get());
        default:
          UNREACHABLE();
      }
    }

  private:
    // A simple abstraction to wrap an FD and (optionally) close it
    // on destruction. We know that we never copy instances of this
    // class once they are instantiated, so it's OK to call
    // `close()` in the destructor since only one reference will
    // ever exist to it.
    class FDWrapper
    {
    public:
      FDWrapper(int_fd _fd, bool _closeOnDestruction)
        : fd(_fd), closeOnDestruction(_closeOnDestruction) {}

      ~FDWrapper() {
        CHECK(fd >= 0);
        if (closeOnDestruction) {
          close(fd);
        }
      }

      operator int_fd() const { return fd; }

    private:
      FDWrapper(const FDWrapper& fd) = delete;

      int_fd fd;
      bool closeOnDestruction;
    };

    IO(Type _type, int_fd _fd, bool closeOnDestruction)
      : type_(_type),
        fd_(new FDWrapper(_fd, closeOnDestruction)),
        path_(None()) {}

    IO(Type _type, const std::string& _path)
      : type_(_type),
        fd_(),
        path_(_path) {}

    Type type_;
    process::Shared<FDWrapper> fd_;
    Option<std::string> path_;
  };

  /**
   * How to redirect the stdin of the executable.
   * See `process::Subprocess::IO`.
   */
  IO in = IO::FD(STDIN_FILENO, false);

  /**
   * How to redirect the stdout of the executable.
   * See `process::Subprocess::IO`.
   */
  IO out = IO::FD(STDOUT_FILENO, false);

  /**
   * Similar to `out`, except this describes how to redirect stderr.
   */
  IO err = IO::FD(STDERR_FILENO, false);
};

} // namespace slave {
} // namespace mesos {

#endif // __MESOS_SLAVE_CONTAINERIZER_HPP__
