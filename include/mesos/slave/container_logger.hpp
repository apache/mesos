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

#ifndef __MESOS_SLAVE_CONTAINER_LOGGER_HPP__
#define __MESOS_SLAVE_CONTAINER_LOGGER_HPP__

#include <map>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/subprocess.hpp>
#include <process/shared.hpp>

#include <stout/try.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/unreachable.hpp>

namespace mesos {
namespace slave {

/**
 * A containerizer component used to manage container logs.
 *
 * The `ContainerLogger` is responsible for handling the stdout/stderr of
 * containers.  The stdout/stderr of tasks launched without can executor
 * (that implicitly use the command executor) will also be handled by the
 * container logger.
 *
 * The container logger is also responsible for providing a public interface
 * for retrieving the logs.
 *
 * TODO(josephw): Provide an interface for exposing custom log-retrieval
 * endpoints via the Mesos web UI.
 */
class ContainerLogger
{
public:
  /**
   * A collection of `process::subprocess` arguments which the container logger
   * can influence.  See `ContainerLogger::prepare`.
   */
  struct ContainerIO
  {
    /**
     * Describes how the container logger redirects I/O for stdout/stderr.
     * See `process::Subprocess::IO`.
     *
     * NOTE: This wrapper prevents the container logger from redirecting I/O
     * via a `Subprocess::PIPE`.  This is restricted because logging must not
     * be affected by the status of the agent process:
     *   * A `Subprocess::PIPE` will require the agent process to regularly
     *     read and empty the pipe.  The agent does not do this.  If the pipe
     *     fills up, the write-end of the pipe may become blocked on IO.
     *   * Logging must continue even if the agent dies.
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
            return process::Subprocess::FD(*fd_->get());
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
          fd_(None()),
          path_(_path) {}

      Type type_;
      Option<process::Shared<FDWrapper>> fd_;
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

  /**
   * Create and initialize a container logger instance of the given type,
   * specified by the `container_logger` agent flag.  If the type is not
   * specified, a default container logger instance will be created.
   *
   * See `ContainerLogger::initialize`.
   */
  static Try<ContainerLogger*> create(const Option<std::string>& type);

  virtual ~ContainerLogger() {}

  /**
   * Initializes this container logger.  This method must be called before any
   * other member function is called.
   *
   * The container logger module should return an error if the particular
   * module is not supported.  For example, if the module implements log
   * rotation via the `logrotate` utility, the module can return an error if
   * the utility is not found.
   */
  virtual Try<Nothing> initialize() = 0;

  /**
   * Called before Mesos creates a container.
   *
   * The container logger is given some of the arguments which the containerizer
   * will use to launch a container.  The container logger should return a
   * `ContainerIO` which tells the containerizer how to handle the stdout
   * and stderr of the container.  The container logger can modify the fields
   * within the `ContainerIO` as much as necessary, with some exceptions;
   * see the struct `ContainerIO` above.
   *
   * NOTE: The container logger should not lose stdout/stderr if the agent
   * fails over.  Additionally, if the container logger is stateful, the logger
   * should be capable of recovering managed executors during the agent recovery
   * process.  See `ContainerLogger::recover`.
   *
   * @param executorInfo Provided for the container logger to track logs.
   * @param sandboxDirectory An absolute path to the executor's sandbox. This
   *     is provided in case the container logger needs to store files in the
   *     executor's sandbox, such as persistent state between agent failovers.
   *     NOTE: All files in the sandbox are exposed via the `/files` endpoint.
   */
  virtual process::Future<ContainerIO> prepare(
      const ExecutorInfo& executorInfo,
      const std::string& sandboxDirectory,
      const Option<std::string>& user) = 0;
};

} // namespace slave {
} // namespace mesos {

#endif // __MESOS_SLAVE_CONTAINER_LOGGER_HPP__
