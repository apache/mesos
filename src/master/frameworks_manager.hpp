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

#ifndef __MASTER_FRAMEWORKS_MANAGER_HPP__
#define __MASTER_FRAMEWORKS_MANAGER_HPP__

// TODO(vinod): We are not using hashmap because it is giving
// strange compile errors for ref. Need to fix this in the future.
#include <map>

#include <process/process.hpp>

#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/time.hpp>

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace master {

using namespace process;

class FrameworksStorage : public Process<FrameworksStorage>
{
public:
  virtual Future<Result<std::map<FrameworkID, FrameworkInfo> > > list() = 0;

  virtual Future<Result<bool> > add(const FrameworkID& id,
      const FrameworkInfo& info) = 0;

  virtual Future<Result<bool> > remove(const FrameworkID& id) = 0;
};

class FrameworksManager : public Process<FrameworksManager>
{
public:
  // Initializes the framework with underlying storage.
  FrameworksManager(FrameworksStorage* _storage);

  // Return the map of all the frameworks.
  Result<std::map<FrameworkID, FrameworkInfo> > list();

  // Add a new framework.
  Result<bool> add(const FrameworkID& id, const FrameworkInfo& info);

  // Remove a framework after a specified number of seconds.
  Future<Result<bool> > remove(const FrameworkID& id, const seconds& s);

  // Resurrect the framework.
  Result<bool> resurrect(const FrameworkID& id);
  //Result<bool> resurrect(const FrameworkID& id, const FrameworkInfo& info);

  // Check if the framework exists.
  Result<bool> exists(const FrameworkID& id);

private:
  void expire(const FrameworkID& id, Promise<Result<bool> >* promise);

  bool cache();

  // Underlying storage implementation.
  FrameworksStorage* storage;

  // Whether or not we have cached the info from the underlying storage.
  bool cached;

  // Cached info about the frameworks.
  std::map<FrameworkID, std::pair<FrameworkInfo, Option<double> > > infos;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_FRAMEWORKS_MANAGER_HPP__
