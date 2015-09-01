/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <list>
#include <ostream>
#include <tuple>
#include <vector>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include <stout/strings.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/signals.hpp>

#include "common/status_utils.hpp"

#include "linux/perf.hpp"

using namespace process;

using process::await;

using std::list;
using std::ostringstream;
using std::set;
using std::string;
using std::tuple;
using std::vector;

namespace perf {

// Delimiter for fields in perf stat output.
static const char PERF_DELIMITER[] = ",";

namespace internal {

// Normalize a perf event name. After normalization the event name
// should match an event field in the PerfStatistics protobuf.
inline string normalize(const string& s)
{
  string lower = strings::lower(s);
  return strings::replace(lower, "-", "_");
}


// Executes the 'perf' command using the supplied arguments, and
// returns stdout as the value of the future or a failure if calling
// the command fails or the command returns a non-zero exit code.
//
// TODO(bmahler): Add a process::os::shell to generalize this.
class Perf : public Process<Perf>
{
public:
  Perf(const vector<string>& _argv) : argv(_argv)
  {
    // The first argument should be 'perf'. Note that this is
    // a bit hacky because this class is specialized to only
    // execute the 'perf' binary. Ultimately, this should be
    // generalized to something like process::os::shell.
    if (argv.empty() || argv.front() != "perf") {
      argv.insert(argv.begin(), "perf");
    }
  }

  virtual ~Perf() {}

  Future<string> future()
  {
    return promise.future();
  }

protected:
  virtual void initialize()
  {
    // Stop when no one cares.
    promise.future().onDiscard(lambda::bind(
        static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));

    execute();
  }

  virtual void finalize()
  {
    // Kill the perf process (if it's still running) by sending
    // SIGTERM to the signal handler which will then SIGKILL the
    // perf process group created by setupChild.
    if (perf.isSome() && perf->status().isPending()) {
      kill(perf->pid(), SIGTERM);
    }

    promise.discard();
  }

private:
  static void signalHandler(int signal)
  {
    // Send SIGKILL to every process in the process group of the
    // calling process. This will terminate both the perf process
    // (including its children) and the bookkeeping process.
    kill(0, SIGKILL);
    abort();
  }

  // This function is invoked right before each 'perf' is exec'ed.
  // Note that this function needs to be async signal safe. In fact,
  // all the library functions we used in this function are async
  // signal safe.
  static int setupChild()
  {
    // Send SIGTERM to the current process if the parent (i.e., the
    // slave) exits. Note that this function should always succeed
    // because we are passing in a valid signal.
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    // Put the current process into a separate process group so that
    // we can kill it and all its children easily.
    if (setpgid(0, 0) != 0) {
      abort();
    }

    // Install a SIGTERM handler which will kill the current process
    // group. Since we already setup the death signal above, the
    // signal handler will be triggered when the parent (i.e., the
    // slave) exits.
    if (os::signals::install(SIGTERM, &signalHandler) != 0) {
      abort();
    }

    pid_t pid = fork();
    if (pid == -1) {
      abort();
    } else if (pid == 0) {
      // Child. This is the process that is going to exec the perf
      // process if zero is returned.

      // We setup death signal for the perf process as well in case
      // someone, though unlikely, accidentally kill the parent of
      // this process (the bookkeeping process).
      prctl(PR_SET_PDEATHSIG, SIGKILL);

      // NOTE: We don't need to clear the signal handler explicitly
      // because the subsequent 'exec' will clear them.
      return 0;
    } else {
      // Parent. This is the bookkeeping process which will wait for
      // the perf process to finish.

      // Close the files to prevent interference on the communication
      // between the slave and the perf process.
      close(STDIN_FILENO);
      close(STDOUT_FILENO);
      close(STDERR_FILENO);

      // Block until the perf process finishes.
      int status = 0;
      if (waitpid(pid, &status, 0) == -1) {
        abort();
      }

      // Forward the exit status if the perf process exits normally.
      if (WIFEXITED(status)) {
        _exit(WEXITSTATUS(status));
      }

      abort();
      UNREACHABLE();
    }
  }

  void execute()
  {
    Try<Subprocess> _perf = subprocess(
        "perf",
        argv,
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        None(),
        None(),
        setupChild);

    if (_perf.isError()) {
      promise.fail("Failed to launch perf process: " + _perf.error());
      terminate(self());
      return;
    }
    perf = _perf.get();

    // Wait for the process to exit.
    await(perf->status(),
          io::read(perf->out().get()),
          io::read(perf->err().get()))
      .onReady(defer(self(), [this](const tuple<
          Future<Option<int>>,
          Future<string>,
          Future<string>>& results) {
        Future<Option<int>> status = std::get<0>(results);
        Future<string> output = std::get<1>(results);

        Option<Error> error = None();

        if (!status.isReady()) {
          error = Error("Failed to execute perf: " +
                        (status.isFailed() ? status.failure() : "discarded"));
        } else if (status->isNone()) {
          error = Error("Failed to execute perf: failed to reap");
        } else if (status->get() != 0) {
          error = Error("Failed to execute perf: " +
                        WSTRINGIFY(status->get()));
        } else if (!output.isReady()) {
          error = Error("Failed to read perf output: " +
                        (output.isFailed() ? output.failure() : "discarded"));
        }

        if (error.isSome()) {
          promise.fail(error->message);
          terminate(self());
          return;
        }

        promise.set(output.get());
        terminate(self());
        return;
    }));
  }

  vector<string> argv;
  Promise<string> promise;
  Option<Subprocess> perf;
};

} // namespace internal {


Future<Version> version()
{
  internal::Perf* perf = new internal::Perf({"--version"});
  Future<string> future = perf->future();
  spawn(perf, true);

  return future
    .then([=](const string& output) -> Future<Version> {
      // Trim off the leading 'perf version ' text to convert.
      return Version::parse(strings::remove(
          output, "perf version ", strings::PREFIX));
    });
};


Future<hashmap<string, mesos::PerfStatistics>> sample(
    const set<string>& events,
    const set<string>& cgroups,
    const Duration& duration)
{
  if (!supported()) {
    return Failure("Perf is not supported");
  }

  vector<string> argv = {
    "stat",

    // System-wide collection from all CPUs.
    "--all-cpus",

    // Print counts using a CSV-style output to make it easy to import
    // directly into spreadsheets. Columns are separated by the string
    // specified in PERF_DELIMITER.
    "--field-separator", PERF_DELIMITER,

    // Ensure all output goes to stdout.
    "--log-fd", "1"
  };

  // Add all pairwise combinations of event and cgroup.
  foreach (const string& event, events) {
    foreach (const string& cgroup, cgroups) {
      argv.push_back("--event");
      argv.push_back(event);
      argv.push_back("--cgroup");
      argv.push_back(cgroup);
    }
  }

  argv.push_back("--");
  argv.push_back("sleep");
  argv.push_back(stringify(duration.secs()));

  Time start = Clock::now();

  internal::Perf* perf = new internal::Perf(argv);
  Future<string> future = perf->future();
  spawn(perf, true);

  auto parse = [start, duration](const string& output) ->
      Future<hashmap<string, mesos::PerfStatistics>> {
    Try<hashmap<string, mesos::PerfStatistics>> parse = perf::parse(output);

    if (parse.isError()) {
      return Failure("Failed to parse perf sample: " + parse.error());
    }

    foreachvalue (mesos::PerfStatistics& statistics, parse.get()) {
      statistics.set_timestamp(start.secs());
      statistics.set_duration(duration.secs());
    }

    return parse.get();
  };

  return future.then(parse);
}


bool valid(const set<string>& events)
{
  ostringstream command;

  // Log everything to stderr which is then redirected to /dev/null.
  command << "perf stat --log-fd 2";
  foreach (const string& event, events) {
    command << " --event " << event;
  }
  command << " true 2>/dev/null";

  return (os::system(command.str()) == 0);
}


bool supported()
{
  // Require perf version >= 2.6.39 to support cgroups and formatting.
  Future<Version> version = perf::version();

  // If perf does not respond in a reasonable time, mark as unsupported.
  version.await(Seconds(5));

  if (!version.isReady()) {
    if (version.isFailed()) {
      LOG(ERROR) << "Failed to get perf version: " << version.failure();
    } else {
      LOG(ERROR) << "Failed to get perf version: timeout of 5secs exceeded";
    }

    version.discard();
    return false;
  }

  return version.get() >= Version(2, 6, 39);
}


Try<hashmap<string, mesos::PerfStatistics>> parse(const string& output)
{
  hashmap<string, mesos::PerfStatistics> statistics;

  foreach (const string& line, strings::tokenize(output, "\n")) {
    vector<string> tokens = strings::tokenize(line, PERF_DELIMITER);
    // Expected format for an output line is: value,event,cgroup
    // (assuming PERF_DELIMITER = ",").
    if (tokens.size() != 3) {
      return Error("Unexpected perf output at line: " + line);
    }

    const string value = tokens[0];
    const string event = internal::normalize(tokens[1]);
    const string cgroup = tokens[2];

    if (!statistics.contains(cgroup)) {
      statistics.put(cgroup, mesos::PerfStatistics());
    }

    const google::protobuf::Reflection* reflection =
      statistics[cgroup].GetReflection();
    const google::protobuf::FieldDescriptor* field =
      statistics[cgroup].GetDescriptor()->FindFieldByName(event);
    if (!field) {
      return Error("Unexpected perf output at line: " + line);
    }

    if (value == "<not supported>") {
      LOG(WARNING) << "Unsupported perf counter, ignoring: " << line;
      continue;
    }

    switch (field->type()) {
      case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
        {
          Try<double> number =
            (value == "<not counted>") ?  0 : numify<double>(value);

          if (number.isError()) {
            return Error("Unable to parse perf value at line: " + line);
          }

          reflection->SetDouble(&(statistics[cgroup]), field, number.get());
          break;
        }
      case google::protobuf::FieldDescriptor::TYPE_UINT64:
        {
          Try<uint64_t> number =
            (value == "<not counted>") ?  0 : numify<uint64_t>(value);

          if (number.isError()) {
            return Error("Unable to parse perf value at line: " + line);
          }

          reflection->SetUInt64(&(statistics[cgroup]), field, number.get());
          break;
        }
      default:
        return Error("Unsupported perf field type at line: " + line);
      }
  }

  return statistics;
}

} // namespace perf {
