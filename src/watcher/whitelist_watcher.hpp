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

#ifndef __WHITELIST_WATCHER_HPP__
#define __WHITELIST_WATCHER_HPP__

#include <string>

#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>

namespace mesos {
namespace internal {

class WhitelistWatcher : public process::Process<WhitelistWatcher>
{
public:
  // NOTE: The caller should ensure a callback exists throughout
  // WhitelistWatcher's lifetime.
  WhitelistWatcher(
      const std::string& path,
      const Duration& watchInterval,
      const lambda::function<
        void(const Option<hashset<std::string>>& whitelist)>& subscriber);

protected:
  virtual void initialize();
  void watch();

private:
  const std::string path;
  const Duration watchInterval;
  lambda::function<void(const Option<hashset<std::string>>& whitelist)>
    subscriber;
  Option<hashset<std::string>> lastWhitelist;
};

} // namespace internal {
} // namespace mesos {

#endif // __WHITELIST_WATCHER_HPP__
