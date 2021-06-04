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
#include <string>
#include <tuple>
#include <vector>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include <stout/os.hpp>
#include <stout/strings.hpp>

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
  Perf(const vector<string>& _argv)
    : ProcessBase(process::ID::generate("perf")),
      argv(_argv)
  {
    // The first argument should be 'perf'. Note that this is
    // a bit hacky because this class is specialized to only
    // execute the 'perf' binary. Ultimately, this should be
    // generalized to something like process::os::shell.
    if (argv.empty() || argv.front() != "perf") {
      argv.insert(argv.begin(), "perf");
    }
  }

  ~Perf() override {}

  Future<string> output()
  {
    return promise.future();
  }

protected:
  void initialize() override
  {
    // Stop when no one cares.
    promise.future().onDiscard(lambda::bind(
        static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));

    execute();
  }

  void finalize() override
  {
    // Kill the perf process (if it's still running) by sending
    // SIGTERM to the signal handler which will then SIGKILL the
    // perf process group created by the supervisor process.
    if (perf.isSome() && perf->status().isPending()) {
      kill(perf->pid(), SIGTERM);
    }

    promise.discard();
  }

private:
  void execute()
  {
    // If the locale is such that `LC_NUMERIC` uses the comma ',' as decimal
    // separator, parsing won't work - because of unexpected number of fields
    // and floating points format - so make sure it's set to `C`.
    std::map<string, string> env = os::environment();
    env["LC_ALL"] = "C";

    // NOTE: The supervisor childhook places perf in its own process group
    // and will kill the perf process when the parent dies.
    Try<Subprocess> _perf = subprocess(
        "perf",
        argv,
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        nullptr,
        env,
        None(),
        {},
        {Subprocess::ChildHook::SUPERVISOR()});

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
        const Future<Option<int>>& status = std::get<0>(results);
        const Future<string>& output = std::get<1>(results);

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
      return parseVersion(output);
    });
};


// Since there is a lot of variety in perf(1) version strings
// across distributions, we parse just the first 2 version
// components, which is enough of a version number to implement
// perf::supported().
Try<Version> parseVersion(const string& output)
{
  // Trim off the leading 'perf version ' text to convert.
  string trimmed = strings::remove(
      strings::trim(output), "perf version ", strings::PREFIX);

  vector<string> components = strings::split(trimmed, ".");

  // perf(1) always has a version with least 2 components.
  if (components.size() > 2) {
    components.resize(2);
  }

  return Version::parse(strings::join(".", components));
}


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

  // TODO(pbrett): Don't wait for these forever!
  return output
    .then([start, duration](const string output)
        -> Future<hashmap<string, mesos::PerfStatistics>> {
      Try<hashmap<string, mesos::PerfStatistics>> result =
        perf::parse(output);

      if (result.isError()) {
        return Failure("Failed to parse perf sample: " + result.error());
      }

      foreachvalue (mesos::PerfStatistics& statistics, result.get()) {
        statistics.set_timestamp(start.secs());
        statistics.set_duration(duration.secs());
      }

      return result.get();
    });
}


bool valid(const set<string>& events)
{
  vector<string> argv = {"stat"};

  foreach (const string& event, events) {
    argv.push_back("--event");
    argv.push_back(event);
  }

  argv.push_back("true");

  internal::Perf* perf = new internal::Perf(argv);
  Future<string> output = perf->output();
  spawn(perf, true);

  output.await();

  // We don't care about the output, just whether it exited non-zero.
  return output.isReady();
}


struct Sample
{
  const string value;
  const string event;
  const string cgroup;

  // Convert a single line of perf output in CSV format (using
  // PERF_DELIMITER as a separator) to a sample.
  static Try<Sample> parse(const string& line)
  {
    // We use strings::split to separate the tokens
    // because the unit field can be empty.
    vector<string> tokens = strings::split(line, PERF_DELIMITER);

    // A number of CSV formats are possible.  Note that we do not
    // use the kernel version when parsing because OS vendors often
    // backport perf tool functionality into older kernel versions.

    switch (tokens.size()) {
      // value,event,cgroup (since Linux v2.6.39)
      case 3:
        return Sample({tokens[0], internal::normalize(tokens[1]), tokens[2]});

      // value,unit,event,cgroup (since Linux v3.14)
      case 4:

      // value,unit,event,cgroup,running,measurement-ratio (since Linux v4.1)
      case 6:

      // value,unit,event,cgroup,running,measurement-ratio,
      // aggregate-value,aggregate-unit (since Linux v4.6)
      case 8:
        return Sample({tokens[0], internal::normalize(tokens[2]), tokens[3]});

      // This is the same format as the one with 8 fields. Due to a
      // bug 'perf stat' may print 4 CSV separators instead of 2 empty
      // fields (https://lkml.org/lkml/2018/3/6/22).
      case 10:
        // Check that the last 4 fields are empty. Otherwise this is
        // an unknown format.
        for (int i = 6; i < 10; ++i) {
          if (!tokens[i].empty()) {
            return Error(
                "Unexpected number of fields (" + stringify(tokens.size()) +
                ")");
          }
        }

        return Sample({tokens[0], internal::normalize(tokens[2]), tokens[3]});

      // Bail out if the format is not recognized.
      default:
        return Error(
            "Unexpected number of fields (" + stringify(tokens.size()) + ")");
    }
  }
};


Try<hashmap<string, mesos::PerfStatistics>> parse(
    const string& output)
{
  hashmap<string, mesos::PerfStatistics> statistics;

  foreach (const string& line, strings::tokenize(output, "\n")) {
    Try<Sample> sample = Sample::parse(line);

    if (sample.isError()) {
      return Error("Failed to parse perf sample line '" + line + "': " +
                   sample.error());
    }

    // Some additional metrics (e.g. stalled cycles per instruction)
    // are printed without an event. Ignore them.
    if (sample->event.empty()) {
      continue;
    }

    if (!statistics.contains(sample->cgroup)) {
      statistics.put(sample->cgroup, mesos::PerfStatistics());
    }

    const google::protobuf::Reflection* reflection =
      statistics[sample->cgroup].GetReflection();
    const google::protobuf::FieldDescriptor* field =
      statistics[sample->cgroup].GetDescriptor()->FindFieldByName(
          sample->event);

    if (field == nullptr) {
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
