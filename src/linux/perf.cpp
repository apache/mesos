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

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <list>
#include <sstream>
#include <tuple>
#include <vector>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include <stout/os.hpp>
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

  Future<string> output()
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
    // perf process group created by the watchdog process.
    if (perf.isSome() && perf->status().isPending()) {
      kill(perf->pid(), SIGTERM);
    }

    promise.discard();
  }

private:
  void execute()
  {
    // NOTE: The watchdog process places perf in its own process group
    // and will kill the perf process when the parent dies.
    Try<Subprocess> _perf = subprocess(
        "perf",
        argv,
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        NO_SETSID,
        None(),
        None(),
        None(),
        Subprocess::Hook::None(),
        None(),
        MONITOR);

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
  Future<string> output = perf->output();
  spawn(perf, true);

  return output
    .then([](const string& output) -> Future<Version> {
      string trimmed = strings::trim(output);

      // Trim off the leading 'perf version ' text to convert.
      return Version::parse(
          strings::remove(trimmed, "perf version ", strings::PREFIX));
    });
};


bool supported(const Version& version)
{
  // Require perf version >= 2.6.39 to support cgroups and formatting.
  return version >= Version(2, 6, 39);
}


bool supported()
{
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

  return supported(version.get());
}


Future<hashmap<string, mesos::PerfStatistics>> sample(
    const set<string>& events,
    const set<string>& cgroups,
    const Duration& duration)
{
  // Is this a no-op?
  if (cgroups.empty()) {
    return hashmap<string, mesos::PerfStatistics>();
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
  Future<string> output = perf->output();
  spawn(perf, true);

  auto parse = [start, duration](
      const tuple<Version, string> values) ->
      Future<hashmap<string, mesos::PerfStatistics>> {
    const Version& version = std::get<0>(values);
    const string& output = std::get<1>(values);

    // Check that the version is supported.
    if (!supported(version)) {
      return Failure("Perf " + stringify(version) + " is not supported");
    }

    Try<hashmap<string, mesos::PerfStatistics>> result =
      perf::parse(output, version);

    if (result.isError()) {
      return Failure("Failed to parse perf sample: " + result.error());
    }

    foreachvalue (mesos::PerfStatistics& statistics, result.get()) {
      statistics.set_timestamp(start.secs());
      statistics.set_duration(duration.secs());
    }

    return result.get();
  };

  // TODO(pbrett): Don't wait for these forever!
  return process::collect(perf::version(), output)
    .then(parse);
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


struct Sample
{
  const string value;
  const string event;
  const string cgroup;

  // Convert a single line of perf output in CSV format (using
  // PERF_DELIMITER as a separator) to a sample.
  static Try<Sample> parse(const string& line, const Version& version)
  {
    // We use strings::split to separate the tokens
    // because the unit field can be empty.
    vector<string> tokens = strings::split(line, PERF_DELIMITER);

    if (version >= Version(4, 0, 0)) {
      // Optional running time and ratio were introduced in Linux v4.0,
      // which make the format either:
      //   value,unit,event,cgroup
      //   value,unit,event,cgroup,running,ratio
      if ((tokens.size() == 4) || (tokens.size() == 6)) {
        return Sample({tokens[0], internal::normalize(tokens[2]), tokens[3]});
      }
    } else if (version >= Version(3, 13, 0)) {
      // Unit was added in Linux v3.13, making the format:
      //   value,unit,event,cgroup
      if (tokens.size() == 4) {
        return Sample({tokens[0], internal::normalize(tokens[2]), tokens[3]});
      }
    } else {
      // Expected format for Linux kernel <= 3.12 is:
      //   value,event,cgroup
      if (tokens.size() == 3) {
        return Sample({tokens[0], internal::normalize(tokens[1]), tokens[2]});
      }
    }

    return Error("Unexpected number of fields");
  }
};


Try<hashmap<string, mesos::PerfStatistics>> parse(
    const string& output,
    const Version& version)
{
  hashmap<string, mesos::PerfStatistics> statistics;

  foreach (const string& line, strings::tokenize(output, "\n")) {
    Try<Sample> sample = Sample::parse(line, version);

    if (sample.isError()) {
      return Error("Failed to parse perf sample line '" + line + "': " +
                   sample.error());
    }

    if (!statistics.contains(sample->cgroup)) {
      statistics.put(sample->cgroup, mesos::PerfStatistics());
    }

    const google::protobuf::Reflection* reflection =
      statistics[sample->cgroup].GetReflection();
    const google::protobuf::FieldDescriptor* field =
      statistics[sample->cgroup].GetDescriptor()->FindFieldByName(
          sample->event);

    if (field == NULL) {
      return Error("Unexpected event '" + sample->event + "'"
                   " in perf output at line: " + line);
    }

    if (sample->value == "<not supported>") {
      LOG(WARNING) << "Unsupported perf counter, ignoring: " << line;
      continue;
    }

    switch (field->type()) {
      case google::protobuf::FieldDescriptor::TYPE_DOUBLE: {
        Try<double> number = (sample->value == "<not counted>")
            ?  0
            : numify<double>(sample->value);

        if (number.isError()) {
          return Error("Unable to parse perf value at line: " + line);
        }

        reflection->SetDouble(&(
            statistics[sample->cgroup]), field, number.get());
        break;
      }
      case google::protobuf::FieldDescriptor::TYPE_UINT64: {
        Try<uint64_t> number = (sample->value == "<not counted>")
            ?  0
            : numify<uint64_t>(sample->value);

        if (number.isError()) {
          return Error("Unable to parse perf value at line: " + line);
        }

        reflection->SetUInt64(&(
            statistics[sample->cgroup]), field, number.get());
        break;
      }
      default:
        return Error("Unsupported perf field type at line: " + line);
      }
  }

  return statistics;
}

} // namespace perf {
