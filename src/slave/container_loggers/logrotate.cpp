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

#include <unistd.h>

#include <new>

#include <functional>
#include <string>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/process.hpp>

#include <stout/bytes.hpp>
#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/os/pagesize.hpp>
#include <stout/os/shell.hpp>
#include <stout/os/su.hpp>
#include <stout/os/write.hpp>

// TODO(abudnik): `ENABLE_LAUNCHER_SEALING` should be replaced with
// `__linux__`, once we drop support for kernels older than 3.17, which
// do not support memfd.
#ifdef ENABLE_LAUNCHER_SEALING
#include "linux/memfd.hpp"
#endif // ENABLE_LAUNCHER_SEALING

#include "logging/logging.hpp"

#include "slave/container_loggers/logrotate.hpp"


using std::string;

using namespace process;
using namespace mesos::internal::logger::rotate;


class LogrotateLoggerProcess : public Process<LogrotateLoggerProcess>
{
public:
  static Try<LogrotateLoggerProcess*> create(const Flags& flags)
  {
    Option<int_fd> configMemFd;

    // Calculate the size of the buffer that is used for reading from
    // the `incoming` pipe.
    const size_t bufferSize = os::pagesize();

    // Populate the `logrotate` configuration file.
    // See `Flags::logrotate_options` for the format.
    //
    // NOTE: We specify a size of `--max_size - bufferSize` because `logrotate`
    // has slightly different size semantics.  `logrotate` will rotate when the
    // max size is *exceeded*.  We rotate to keep files *under* the max size.
    const string config =
      "\"" + flags.log_filename.get() + "\" {\n" +
      flags.logrotate_options.getOrElse("") + "\n" +
      "size " + stringify(flags.max_size.bytes() - bufferSize) + "\n" +
      "}";

    // TODO(abudnik): `ENABLE_LAUNCHER_SEALING` should be replaced with
    // `__linux__`, once we drop support for kernels older than 3.17, which
    // do not support memfd.
#ifdef ENABLE_LAUNCHER_SEALING
    // Create a temporary anonymous file, which can be accessed by a child
    // process by opening a `/proc/self/fd/<FD of anonymous file>`.
    // This file is automatically removed on process termination, so we don't
    // need to garbage collect it.
    // We use the `memfd` file to pass the configuration to `logrotate`.
    Try<int_fd> memFd =
      mesos::internal::memfd::create("mesos_logrotate", 0);

    if (memFd.isError()) {
      return Error(
          "Failed to create memfd file '" +
          flags.log_filename.get() + "': " + memFd.error());
    }

    Try<Nothing> write = os::write(memFd.get(), config);
    if (write.isError()) {
      os::close(memFd.get());
      return Error("Failed to write memfd file '" + flags.log_filename.get() +
                   "': " + write.error());
    }

    // `logrotate` requires configuration file to have 0644 or 0444 permissions.
    int ret = fchmod(memFd.get(), S_IRUSR | S_IRGRP | S_IROTH);
    if (ret == -1) {
      ErrnoError error("Failed to chmod memfd file '" +
                       flags.log_filename.get() + "'");
      os::close(memFd.get());
      return error;
    }

    configMemFd = memFd.get();
#else
    Try<Nothing> result = os::write(
        flags.log_filename.get() + CONF_SUFFIX, config);

    if (result.isError()) {
      return Error("Failed to write configuration file: " + result.error());
    }
#endif // ENABLE_LAUNCHER_SEALING

    return new LogrotateLoggerProcess(flags, configMemFd, bufferSize);
  }

  ~LogrotateLoggerProcess() override
  {
    if (configMemFd.isSome()) {
      os::close(configMemFd.get());
    }

    if (buffer != nullptr) {
      delete[] buffer;
      buffer = nullptr;
    }

    if (leading.isSome()) {
      os::close(leading.get());
    }
  }

  // Prepares and starts the loop which reads from stdin, writes to the
  // leading log file, and manages total log size.
  Future<Nothing> run()
  {
    // NOTE: This is a prerequisuite for `io::read`.
    Try<Nothing> async = io::prepare_async(STDIN_FILENO);
    if (async.isError()) {
      return Failure("Failed to set async pipe: " + async.error());
    }

    // NOTE: This does not block.
    loop();

    return promise.future();
  }

  // Reads from stdin and writes to the leading log file.
  void loop()
  {
    io::read(STDIN_FILENO, buffer, bufferSize)
      .then(defer(self(), [&](size_t readSize) -> Future<Nothing> {
        // Check if EOF has been reached on the input stream.
        // This indicates that the container (whose logs are being
        // piped to this process) has exited.
        if (readSize <= 0) {
          promise.set(Nothing());
          return Nothing();
        }

        // Do log rotation (if necessary) and write the bytes to the
        // leading log file.
        Try<Nothing> result = write(readSize);
        if (result.isError()) {
          promise.fail("Failed to write: " + result.error());
          return Nothing();
        }

        // Use `dispatch` to limit the size of the call stack.
        dispatch(self(), &LogrotateLoggerProcess::loop);

        return Nothing();
      }));
  }

  // Writes the buffer from stdin to the leading log file.
  // When the number of written bytes exceeds `--max_size`, the leading
  // log file is rotated.  When the number of log files exceed `--max_files`,
  // the oldest log file is deleted.
  Try<Nothing> write(size_t readSize)
  {
    // Rotate the log file if it will grow beyond the `--max_size`.
    if (bytesWritten + readSize > flags.max_size.bytes()) {
      rotate();
    }

    // If the leading log file is not open, open it.
    // NOTE: We open the file in append-mode as `logrotate` may sometimes fail.
    if (leading.isNone()) {
      Try<int> open = os::open(
          flags.log_filename.get(),
          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
          S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

      if (open.isError()) {
        return Error(
            "Failed to open '" + flags.log_filename.get() +
            "': " + open.error());
      }

      leading = open.get();
    }

    // Write from stdin to `leading`.
    // NOTE: We do not exit on error here since we are prioritizing
    // clearing the STDIN pipe (which would otherwise potentially block
    // the container on write) over log fidelity.
    Try<Nothing> result =
      os::write(leading.get(), string(buffer, readSize));

    if (result.isError()) {
      std::cerr << "Failed to write: " << result.error() << std::endl;
    }

    bytesWritten += readSize;

    return Nothing();
  }

  // Calls `logrotate` on the leading log file and resets the `bytesWritten`.
  void rotate()
  {
    if (leading.isSome()) {
      os::close(leading.get());
      leading = None();
    }

    // Call `logrotate` to move around the files.
    // NOTE: If `logrotate` fails for whatever reason, we will ignore
    // the error and continue logging.  In case the leading log file
    // is not renamed, we will continue appending to the existing
    // leading log file.
    os::shell(
        flags.logrotate_path +
        " --state \"" + flags.log_filename.get() + STATE_SUFFIX + "\" \"" +
        configPath + "\"");

    // Reset the number of bytes written.
    bytesWritten = 0;
  }

private:
  explicit LogrotateLoggerProcess(
      const Flags& _flags,
      const Option<int_fd>& _configMemFd,
      size_t _bufferSize)
    : ProcessBase(process::ID::generate("logrotate-logger")),
      flags(_flags),
      configMemFd(_configMemFd),
      buffer(new char[_bufferSize]),
      bufferSize(_bufferSize),
      leading(None()),
      bytesWritten(0)
  {
    if (configMemFd.isSome()) {
      configPath = "/proc/self/fd/" + stringify(configMemFd.get());
    } else {
      configPath = flags.log_filename.get() + CONF_SUFFIX;
    }
  }

  const Flags flags;
  const Option<int_fd> configMemFd;

  string configPath;

  // For reading from stdin.
  char* buffer;
  const size_t bufferSize;

  // For writing and rotating the leading log file.
  Option<int> leading;
  size_t bytesWritten;

  // Used to capture when log rotation has completed because the
  // underlying process/input has terminated.
  Promise<Nothing> promise;
};


int main(int argc, char** argv)
{
  Flags flags;

  // Load and validate flags from the environment and command line.
  Try<flags::Warnings> load = flags.load(None(), &argc, &argv);

  if (flags.help) {
    std::cout << flags.usage() << std::endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    std::cerr << flags.usage(load.error()) << std::endl;
    return EXIT_FAILURE;
  }

  mesos::internal::logging::initialize(argv[0], false);

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  // Make sure this process is running in its own session.
  // This ensures that, if the parent process (presumably the Mesos agent)
  // terminates, this logger process will continue to run.
  if (::setsid() == -1) {
    EXIT(EXIT_FAILURE)
      << ErrnoError("Failed to put child in a new session").message;
  }

  // If the `--user` flag is set, change the UID of this process to that user.
  if (flags.user.isSome()) {
    Try<Nothing> result = os::su(flags.user.get());

    if (result.isError()) {
      EXIT(EXIT_FAILURE)
        << ErrnoError("Failed to switch user for logrotate process").message;
    }
  }

  // Asynchronously control the flow and size of logs.
  Try<LogrotateLoggerProcess*> process = LogrotateLoggerProcess::create(flags);
  if (process.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create Logrotate process: " << process.error();
  }

  spawn(process.get());

  // Wait for the logging process to finish.
  Future<Nothing> status =
    dispatch(process.get(), &LogrotateLoggerProcess::run);

  status.await();

  terminate(process.get());
  wait(process.get());

  delete process.get();

  return status.isReady() ? EXIT_SUCCESS : EXIT_FAILURE;
}
