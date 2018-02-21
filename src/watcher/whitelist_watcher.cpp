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

#include <string>
#include <vector>

#include <glog/logging.h>

#include <process/delay.hpp>
#include <process/id.hpp>

#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "watcher/whitelist_watcher.hpp"

namespace mesos {
namespace internal {

using std::string;
using std::vector;

using process::Process;

using lambda::function;


WhitelistWatcher::WhitelistWatcher(
    const Option<Path>& path,
    const Duration& watchInterval,
    const function<void(const Option<hashset<string>>& whitelist)>& subscriber,
    const Option<hashset<string>>& initialWhitelist)
  : ProcessBase(process::ID::generate("whitelist")),
    path(path),
    watchInterval(watchInterval),
    subscriber(subscriber),
    lastWhitelist(initialWhitelist) {}


void WhitelistWatcher::initialize()
{
  // If no whitelist file is given (loaded whitelist is in state
  // (1) absent), then there is no need to watch. In case the
  // subscriber's initial policy was not permissive (initial
  // whitelist is not in (1) absent), notify the subscriber that
  // there is no whitelist any more.
  //
  // TODO(cmaloney): In older versions of Mesos the default value for
  // 'whitelist' which represented "accept all nodes" was the string
  // value '*' rather than 'None'. For backwards compatibility we
  // still check for '*'. Can be removed after 0.23.
  if (path.isSome() && path->string() == "*") {
    LOG(WARNING)
      << "Explicitly specifying '*' for the whitelist in order to "
      << "\"accept all\" is deprecated and will be removed in a future "
      << "release; simply don't specify the whitelist flag in order to "
      << "\"accept all\" agents";
  }

  if (path.isNone() || path->string() == "*") { // Accept all nodes.
    VLOG(1) << "No whitelist given";
    if (lastWhitelist.isSome()) {
      subscriber(None());
    }
  } else {
    watch();
  }
}


void WhitelistWatcher::watch()
{
  // Read the list of white listed nodes from local file.
  // TODO(vinod): Add support for reading from ZooKeeper.
  // TODO(vinod): Ensure this read is atomic w.r.t external
  // writes/updates to this file.
  Option<hashset<string>> whitelist;

  CHECK_SOME(path);
  Try<string> read = os::read(path->string());

  if (read.isError()) {
    LOG(ERROR) << "Error reading whitelist file: " << read.error() << ". "
               << "Retrying";
    whitelist = lastWhitelist;
  } else if (read->empty()) {
    VLOG(1) << "Empty whitelist file " << path->string();
    whitelist = hashset<string>();
  } else {
    hashset<string> hostnames;
    vector<string> lines = strings::tokenize(read.get(), "\n");
    foreach (const string& hostname, lines) {
      hostnames.insert(hostname);
    }
    whitelist = hostnames;
  }

  // Send the whitelist to subscriber, if necessary.
  if (whitelist != lastWhitelist) {
    subscriber(whitelist);
  }

  // Schedule the next check.
  lastWhitelist = whitelist;
  delay(watchInterval, self(), &WhitelistWatcher::watch);
}

} // namespace internal {
} // namespace mesos {
