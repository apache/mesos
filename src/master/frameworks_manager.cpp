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

#include <process/delay.hpp>
#include <process/dispatch.hpp>

#include <stout/foreach.hpp>

#include "master/frameworks_manager.hpp"

#include "common/type_utils.hpp"

#include "logging/logging.hpp"

using std::map;

namespace mesos {
namespace internal {
namespace master {

// Constructor: Initializes storage and resets cached flag.
FrameworksManager::FrameworksManager(FrameworksStorage* _storage)
  : storage(_storage), cached(false) {}


// Lists the map of all the frameworks.
// If no frameworks present, return an empty map.
Result<map<FrameworkID, FrameworkInfo> > FrameworksManager::list()
{
  if (!cache()) {
    return Result<map<FrameworkID, FrameworkInfo> >
      ::error("Error caching framework infos.");
  }

  map<FrameworkID, FrameworkInfo> result;

  foreachkey (const FrameworkID& id, infos) {
    result[id] = infos[id].first;
  }

  return result;
}


// Adds a new framework.
// Sends a message to the underlying storage to add the information
// about the new framework AND also caches it.
Result<bool> FrameworksManager::add(const FrameworkID& id,
                                    const FrameworkInfo& info)
{
  // TODO(vinod): Handle duplicate frameworks as following:
  // Case 1: If same ID and info exist, just return without going to storage.
  // Case 2: If ID exists but info is different, update both storage and cache.
  // Note: For Case 2, storage layer is expected to over-write info of duplicate
  // framework id.
  //  if (!cache()) {
  //      return Result<bool>::error("Error caching framework infos.");
  //    }
  //
  //  if (infos.count(id) > 0) {
  //    LOG(INFO) << "Duplicate framework detected...id: " << id;
  //
  //    if (infos[id].first == info)
  //    {
  //      LOG(INFO) << "Duplicate framework information detected...returning.";
  //      return Result<bool>::some(true);
  //    }
  //  }

  Future<Result<bool> > result =
    dispatch(storage, &FrameworksStorage::add, id, info);

  result.await();

  CHECK(result.isReady());

  if (result.get().isError()) {
    LOG(ERROR) << "Error adding framework to underlying storage: "
               << result.get().error();
    return result.get();
  }

  infos[id] = std::make_pair(info, Option<double>::none());

  return true;
}


Future<Result<bool> > FrameworksManager::remove(const FrameworkID& id,
                                                const Duration& timeout)
{
  if (!cache()) {
    return Result<bool>::error("Error caching framework infos.");
  }

  if (infos.count(id) == 0) {
    LOG(INFO) << "Can't remove non-existent Framework: " << id;
    return Result<bool>::error("Error removing non-existing framework.");
  }

  LOG(INFO) << "Expiring framework " << id << " in " << timeout;

  // Set the option to contain the firing time of the message.
  infos[id].second = Option<double>::some(Clock::now() + timeout.secs());

  Promise<Result<bool> >* promise = new Promise<Result<bool> >();
  delay(timeout, self(), &FrameworksManager::expire, id, promise);
  return promise->future();
}


// Resurrects the framework.
// Basically this stops any "prior" expire messages from removing
// the framework by setting a flag.
Result<bool> FrameworksManager::resurrect(const FrameworkID& id)
{
  if (!cache()) {
    return Result<bool>::error("Error caching framework infos.");
  }

  if (infos.count(id) > 0) {
    infos[id].second = Option<double>::none();

    return true;
  }

  return false;
}


// Checks if the given framework exists.
Result<bool> FrameworksManager::exists(const FrameworkID& id)
{
  if (!cache()) {
    return Result<bool>::error("Error caching framework infos.");
  }

  return Result<bool>::some(infos.count(id) > 0);
}


// Actually removes the framework from the underlying storage.
// Checks for the case when the framework is being resurrected.
void FrameworksManager::expire(const FrameworkID& id,
                               Promise<Result<bool> >* promise)
{
  if (infos.count(id) > 0) {
    const Option<double>& option = infos[id].second;

    if (option.isSome()) {
      if (Clock::now() >= option.get()) {
        LOG(INFO) << "Removing framework " << id << " from storage";

        Future<Result<bool> > result =
          dispatch(storage, &FrameworksStorage::remove, id);

        result.await();

        CHECK(result.isReady());

        // If storage returns successfully remove from cache.
        if (!result.get().isError()) {
          infos.erase(id);
        }
        promise->set(result.get());
        delete promise;
        return;
      } else {
        LOG(INFO) << "Framework appears to be resurrected, and then "
                  << "re-removed, so ignoring this delayed expire";
      }
    } else {
      LOG(INFO) << "Framework appears to have been "
                << "resurrected, ignoring delayed expire";
    }
  } else {
    LOG(INFO) << "Framework has already been removed by someone else,"
              << "ignoring delayed expire";
  }

  promise->set(Result<bool>::some(false));
  delete promise;
}


// Caches the framework information from the underlying storage.
bool FrameworksManager::cache()
{
  if (!cached) {
    LOG(INFO) << "Caching framework information";

    Future<Result<map<FrameworkID, FrameworkInfo> > > result =
      dispatch(storage, &FrameworksStorage::list);

    result.await();

    CHECK(result.isReady());

    if (result.get().isError()) {
      LOG(ERROR) << "Error getting framework info from underlying storage: "
                 << result.get().error();
      return false;
    }

    foreachpair (const FrameworkID& id, const FrameworkInfo& info, result.get().get()) {
      infos[id] = std::make_pair(info, Option<double>::none());
    }

    cached = true;
  }

  return true;
}

}  // namespace master {
}  // namespace internal {
}  // namespace mesos {
