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

#ifndef __WATCHER_WHITELIST_WATCHER_HPP__
#define __WATCHER_WHITELIST_WATCHER_HPP__

#include <string>

#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>

namespace mesos {
namespace internal {

// A whitelist may be (1) absent, (2) empty, (3) non-empty. The
// watcher notifies the subscriber if the state of the whitelist
// changes or if the contents changes in case the whitelist is in
// state (3) non-empty.
class WhitelistWatcher : public process::Process<WhitelistWatcher>
{
public:
  // By default the initial policy is assumed to be permissive, i.e.
  // initial whitelist is in state (1), meaning that peers are
  // accepted before a whitelist is first loaded. In this case a
  // subscriber is notified only if a set of peers (possibly empty)
  // is loaded from the whitelist file.
  // If a subscriber initially uses a nonpermissive policy, i.e.
  // initial whitelist is in state (2) or (3), peers before first
  // loaded whitelist are rejected. In this case a subscriber must
  // provide the initial whitelist and will be notified if the policy
  // becomes permissive (no whitelist file) or if a set of peers
  // loaded from the whitelist file changes.
  // NOTE: The caller should ensure a callback exists throughout
  // WhitelistWatcher's lifetime.
  WhitelistWatcher(
      const Option<Path>& path,
      const Duration& watchInterval,
      const lambda::function<
        void(const Option<hashset<std::string>>& whitelist)>& subscriber,
      const Option<hashset<std::string>>& initialWhitelist = None());

protected:
  void initialize() override;
  void watch();

private:
  const Option<Path> path;
  const Duration watchInterval;
  lambda::function<void(const Option<hashset<std::string>>& whitelist)>
    subscriber;
  Option<hashset<std::string>> lastWhitelist;
};

} // namespace internal {
} // namespace mesos {

#endif // __WATCHER_WHITELIST_WATCHER_HPP__
