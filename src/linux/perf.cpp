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

#include <list>
#include <ostream>
#include <vector>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include <stout/strings.hpp>

#include "linux/perf.hpp"

using std::list;
using std::ostringstream;
using std::set;
using std::string;
using std::vector;

using namespace process;

namespace perf {

// Delimiter for fields in perf stat output.
const string PERF_DELIMITER = ",";

// Use an empty string as the key for the parse output when sampling a
// set of pids. No valid cgroup can be an empty string.
const string PIDS_KEY = "";

namespace internal {

string command(
    const set<string>& events,
    const set<string>& cgroups,
    const Duration& duration)
{
  ostringstream command;

  command << "perf stat -x" << PERF_DELIMITER << " -a";
  command << " --log-fd 1";  // Ensure all output goes to stdout.
  // Nested loop to produce all pairings of event and cgroup.
  foreach (const string& event, events) {
    foreach (const string& cgroup, cgroups) {
      command << " --event " << event
              << " --cgroup " << cgroup;
    }
  }
  command << " -- sleep " << stringify(duration.secs());

  return command.str();
}


string command(
    const set<string>& events,
    const string& cgroup,
    const Duration& duration)
{
  set<string> cgroups;
  cgroups.insert(cgroup);

  return command(events, cgroups, duration);
}


string command(
    const set<string>& events,
    const set<pid_t>& pids,
    const Duration& duration)
{
  ostringstream command;

  command << "perf stat -x" << PERF_DELIMITER << " -a";
  command << " --log-fd 1";  // Ensure all output goes to stdout.
  command << " --event " << strings::join(",", events);
  command << " --pid " << strings::join(",", pids);
  command << " -- sleep " << stringify(duration.secs());

  return command.str();
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
  PerfSampler(const string& _command, const Duration& _duration)
    : command(_command), duration(_duration) {}

  virtual ~PerfSampler() {}

  Future<hashmap<string, mesos::PerfStatistics> > future()
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
    discard(output);

    // Kill the perf process if it's still running.
    if (perf.isSome() && perf.get().status().isPending()) {
      kill(perf.get().pid(), SIGKILL);
    }

    promise.discard();
  }

private:
  void sample()
  {
    Try<Subprocess> _perf = subprocess(
        command,
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        Subprocess::PIPE());

    if (_perf.isError()) {
      promise.fail("Failed to launch perf process: " + _perf.error());
      terminate(self());
      return;
    }
    perf = _perf.get();

    // Start reading from stdout and stderr now. We don't use stderr
    // but must read from it to avoid the subprocess blocking on the
    // pipe.
    output.push_back(process::io::read(perf.get().out().get()));
    output.push_back(process::io::read(perf.get().err().get()));

    // Wait for the process to exit.
    perf.get().status()
      .onAny(defer(self(), &Self::_sample, lambda::_1));
  }

  void _sample(const Future<Option<int> >& status)
  {
    if (!status.isReady()) {
      promise.fail("Failed to get exit status of perf process: " +
                   (status.isFailed() ? status.failure() : "discarded"));
      terminate(self());
      return;
    }

    if (status.get().get() != 0) {
      promise.fail("Failed to execute perf, exit status: " +
                    stringify(WEXITSTATUS(status.get().get())));

      terminate(self());
      return;
    }

    // Wait until we collect all output.
    collect(output).onAny(defer(self(), &Self::__sample, lambda::_1));
  }

  void  __sample(const Future<list<string> >& future)
  {
    if (!future.isReady()) {
      promise.fail("Failed to collect output of perf process: " +
                   (future.isFailed() ? future.failure() : "discarded"));
      terminate(self());
      return;
    }

    // Parse output from stdout.
    Try<hashmap<string, mesos::PerfStatistics> > parse =
      perf::parse(output.front().get());
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
  }

  const string command;
  const Duration duration;
  Time start;
  Option<Subprocess> perf;
  Promise<hashmap<string, mesos::PerfStatistics> > promise;
  list<Future<string> > output;
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

  const string command = internal::command(events, pids, duration);
  internal::PerfSampler* sampler = new internal::PerfSampler(command, duration);
  Future<hashmap<string, mesos::PerfStatistics> > future = sampler->future();
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


Future<hashmap<string, mesos::PerfStatistics> > sample(
    const set<string>& events,
    const set<string>& cgroups,
    const Duration& duration)
{
  if (!supported()) {
    return Failure("Perf is not supported");
  }

  const string command = internal::command(events, cgroups, duration);
  internal::PerfSampler* sampler = new internal::PerfSampler(command, duration);
  Future<hashmap<string, mesos::PerfStatistics> > future = sampler->future();
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


Try<hashmap<string, mesos::PerfStatistics> > parse(const string& output)
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
