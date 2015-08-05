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

// Use an empty string as the key for the parse output when sampling a
// set of pids. No valid cgroup can be an empty string.
static const char PIDS_KEY[] = "";

namespace internal {

vector<string> argv(
    const set<string>& events,
    const set<string>& cgroups,
    const Duration& duration)
{
  vector<string> argv = {
    "perf", "stat",

    // System-wide collection from all CPUs.
    "--all-cpus",

    // Print counts using a CSV-style output to make it easy to import
    // directly into spreadsheets. Columns are separated by the string
    // specified in PERF_DELIMITER.
    "--field-separator", PERF_DELIMITER,

    // Ensure all output goes to stdout.
    "--log-fd", "1"
  };

  // Nested loop to produce all pairings of event and cgroup.
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

  return argv;
}


vector<string> argv(
    const set<string>& events,
    const string& cgroup,
    const Duration& duration)
{
  set<string> cgroups;
  cgroups.insert(cgroup);

  return argv(events, cgroups, duration);
}


vector<string> argv(
    const set<string>& events,
    const set<pid_t>& pids,
    const Duration& duration)
{
  vector<string> argv = {
    "perf", "stat",

    // System-wide collection from all CPUs.
    "--all-cpus",

    // Print counts using a CSV-style output to make it easy to import
    // directly into spreadsheets. Columns are separated by the string
    // specified in PERF_DELIMITER.
    "--field-separator", PERF_DELIMITER,

    // Ensure all output goes to stdout.
    "--log-fd", "1",

    "--event", strings::join(",", events),
    "--pid", strings::join(",", pids),
    "--",
    "sleep", stringify(duration.secs())
  };

  return argv;
}


// Normalize a perf event name. After normalization the event name
// should match an event field in the PerfStatistics protobuf.
inline string normalize(const string& s)
{
  string lower = strings::lower(s);
  return strings::replace(lower, "-", "_");
}


class PerfSampler : public Process<PerfSampler>
{
public:
  PerfSampler(const vector<string>& _argv, const Duration& _duration)
    : argv(_argv), duration(_duration) {}

  virtual ~PerfSampler() {}

  Future<hashmap<string, mesos::PerfStatistics>> future()
  {
    return promise.future();
  }

protected:
  virtual void initialize()
  {
    // Stop when no one cares.
    promise.future().onDiscard(lambda::bind(
          static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));

    if (duration < Seconds(0)) {
      promise.fail("Perf sample duration cannot be negative: '" +
                   stringify(duration.secs()) + "'");
      terminate(self());
      return;
    }

    start = Clock::now();

    sample();
  }

  virtual void finalize()
  {
    // Kill the perf process if it's still running.
    if (perf.isSome() && perf.get().status().isPending()) {
      kill(perf.get().pid(), SIGTERM);
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

  void sample()
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
    await(perf.get().status(),
          io::read(perf.get().out().get()),
          io::read(perf.get().err().get()))
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
        } else if (status.get().isNone()) {
          error = Error("Failed to execute perf: failed to reap");
        } else if (status.get().get() != 0) {
          error = Error("Failed to collect perf statistics: " +
                        WSTRINGIFY(status.get().get()));
        } else if (!output.isReady()) {
          error = Error("Failed to read perf output: " +
                        (output.isFailed() ? output.failure() : "discarded"));
        }

        if (error.isSome()) {
          promise.fail(error.get().message);
          terminate(self());
          return;
        }

        // Parse output from stdout.
        Try<hashmap<string, mesos::PerfStatistics>> parse =
            perf::parse(output.get());

        if (parse.isError()) {
          promise.fail("Failed to parse perf output: " + parse.error());
          terminate(self());
          return;
        }

        // Create a non-const copy from the Try<> so we can set the
        // timestamp and duration.
        hashmap<string, mesos::PerfStatistics> statistics = parse.get();
        foreachvalue (mesos::PerfStatistics& s, statistics) {
          s.set_timestamp(start.secs());
          s.set_duration(duration.secs());
        }

        promise.set(statistics);
        terminate(self());
        return;
    }));
}

  const vector<string> argv;
  const Duration duration;
  Time start;
  Option<Subprocess> perf;
  Promise<hashmap<string, mesos::PerfStatistics>> promise;
};


// Helper to select a single key from the hashmap of perf statistics.
Future<mesos::PerfStatistics> select(
    const string& key,
    const hashmap<string, mesos::PerfStatistics>& statistics)
{
  return statistics.get(key).get();
}

} // namespace internal {


Future<mesos::PerfStatistics> sample(
    const set<string>& events,
    pid_t pid,
    const Duration& duration)
{
  set<pid_t> pids;
  pids.insert(pid);
  return sample(events, pids, duration);
}


Future<mesos::PerfStatistics> sample(
    const set<string>& events,
    const set<pid_t>& pids,
    const Duration& duration)
{
  if (!supported()) {
    return Failure("Perf is not supported");
  }

  const vector<string> argv = internal::argv(events, pids, duration);
  internal::PerfSampler* sampler = new internal::PerfSampler(argv, duration);
  Future<hashmap<string, mesos::PerfStatistics>> future = sampler->future();
  spawn(sampler, true);
  return future
    .then(lambda::bind(&internal::select, PIDS_KEY, lambda::_1));
}


Future<mesos::PerfStatistics> sample(
    const set<string>& events,
    const string& cgroup,
    const Duration& duration)
{
  set<string> cgroups;
  cgroups.insert(cgroup);
  return sample(events, cgroups, duration)
    .then(lambda::bind(&internal::select, cgroup, lambda::_1));
}


Future<hashmap<string, mesos::PerfStatistics>> sample(
    const set<string>& events,
    const set<string>& cgroups,
    const Duration& duration)
{
  if (!supported()) {
    return Failure("Perf is not supported");
  }

  const vector<string> argv = internal::argv(events, cgroups, duration);
  internal::PerfSampler* sampler = new internal::PerfSampler(argv, duration);
  Future<hashmap<string, mesos::PerfStatistics>> future = sampler->future();
  spawn(sampler, true);
  return future;
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
  // Require Linux kernel version >= 2.6.38 for "-x" and >= 2.6.39 for
  // "--cgroup"
  Try<Version> release = os::release();

  // This is not expected to ever be an Error.
  CHECK_SOME(release);

  return release.get() >= Version(2, 6, 39);
}


Try<hashmap<string, mesos::PerfStatistics>> parse(const string& output)
{
  hashmap<string, mesos::PerfStatistics> statistics;

  foreach (const string& line, strings::tokenize(output, "\n")) {
    vector<string> tokens = strings::tokenize(line, PERF_DELIMITER);
    // Expected format for an output line is either:
    // value,event          (when sampling pids)
    // value,event,cgroup   (when sampling a cgroup)
    // assuming PERF_DELIMITER = ",".
    if (tokens.size() < 2 || tokens.size() > 3) {
      return Error("Unexpected perf output at line: " + line);
    }

    const string value = tokens[0];
    const string event = internal::normalize(tokens[1]);
    // Use the special PIDS_KEY when sampling pids.
    const string cgroup = (tokens.size() == 3 ? tokens[2] : PIDS_KEY);

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
