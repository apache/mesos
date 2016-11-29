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
  struct SubprocessInfo
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
        return IO(Type::PATH, None(), path);
      }

      static IO FD(int fd)
      {
        return IO(Type::FD, fd, None());
      }

      operator process::Subprocess::IO () const
      {
        switch (type_) {
          case Type::FD:
            // NOTE: The FD is not duplicated and will be closed (as
            // seen by the agent process) when the container is
            // spawned.  This shifts the burden of FD-lifecycle
            // management into the Containerizer.
            return process::Subprocess::FD(
                fd_.get(),
                process::Subprocess::IO::OWNED);
          case Type::PATH:
            return process::Subprocess::PATH(path_.get());
          default:
            UNREACHABLE();
        }
      }

      Type type() const { return type_; }
      Option<int> fd() const { return fd_; }
      Option<std::string> path() const { return path_; }

    private:
      IO(Type _type,
         const Option<int>& _fd,
         const Option<std::string>& _path)
        : type_(_type),
          fd_(_fd),
          path_(_path) {}

      Type type_;
      Option<int> fd_;
      Option<std::string> path_;
    };

    /**
     * How to redirect the stdout of the executable.
     * See `process::Subprocess::IO`.
     */
    IO out = SubprocessInfo::IO::FD(STDOUT_FILENO);

    /**
     * Similar to `out`, except this describes how to redirect stderr.
     */
    IO err = SubprocessInfo::IO::FD(STDERR_FILENO);
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
   * `SubprocessInfo` which tells the containerizer how to handle the stdout
   * and stderr of the subprocess.  The container logger can modify the fields
   * within the `SubprocessInfo` as much as necessary, with some exceptions;
   * see the struct `SubprocessInfo` above.
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
  virtual process::Future<SubprocessInfo> prepare(
      const ExecutorInfo& executorInfo,
      const std::string& sandboxDirectory,
      const Option<std::string>& user) = 0;
};

} // namespace slave {
} // namespace mesos {

#endif // __MESOS_SLAVE_CONTAINER_LOGGER_HPP__
