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

#include "resource_provider/storage/uri_disk_profile_adaptor.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <tuple>

#include <mesos/type_utils.hpp>

#include <mesos/module/disk_profile_adaptor.hpp>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include "csi/v0_utils.hpp"

#include "resource_provider/storage/disk_profile_utils.hpp"

using namespace mesos;
using namespace process;

using std::map;
using std::string;
using std::tuple;

using google::protobuf::Map;

using mesos::resource_provider::DiskProfileMapping;

namespace mesos {
namespace internal {
namespace storage {

UriDiskProfileAdaptor::UriDiskProfileAdaptor(const Flags& _flags)
  : flags(_flags),
    process(new UriDiskProfileAdaptorProcess(flags))
{
  spawn(process.get());
}


UriDiskProfileAdaptor::~UriDiskProfileAdaptor()
{
  terminate(process.get());
  wait(process.get());
}


Future<DiskProfileAdaptor::ProfileInfo> UriDiskProfileAdaptor::translate(
    const string& profile,
    const ResourceProviderInfo& resourceProviderInfo)
{
  return dispatch(
      process.get(),
      &UriDiskProfileAdaptorProcess::translate,
      profile,
      resourceProviderInfo);
}


Future<hashset<string>> UriDiskProfileAdaptor::watch(
    const hashset<string>& knownProfiles,
    const ResourceProviderInfo& resourceProviderInfo)
{
  return dispatch(
      process.get(),
      &UriDiskProfileAdaptorProcess::watch,
      knownProfiles,
      resourceProviderInfo);
}


UriDiskProfileAdaptorProcess::UriDiskProfileAdaptorProcess(
    const UriDiskProfileAdaptor::Flags& _flags)
  : ProcessBase(ID::generate("uri-disk-profile-adaptor")),
    flags(_flags) {}


void UriDiskProfileAdaptorProcess::initialize()
{
  poll();
}


Future<DiskProfileAdaptor::ProfileInfo>
  UriDiskProfileAdaptorProcess::translate(
      const string& profile,
      const ResourceProviderInfo& resourceProviderInfo)
{
  if (!profileMatrix.contains(profile) || !profileMatrix.at(profile).active) {
    return Failure("Profile '" + profile + "' not found");
  }

  const DiskProfileMapping::CSIManifest& manifest =
    profileMatrix.at(profile).manifest;

  if (!isSelectedResourceProvider(manifest, resourceProviderInfo)) {
    return Failure(
        "Profile '" + profile + "' does not apply to resource provider with "
        "type '" + resourceProviderInfo.type() + "' and name '" +
        resourceProviderInfo.name() + "'");
  }

  return DiskProfileAdaptor::ProfileInfo{
    csi::v0::devolve(manifest.volume_capabilities()),
    manifest.create_parameters()
  };
}


Future<hashset<string>> UriDiskProfileAdaptorProcess::watch(
    const hashset<string>& knownProfiles,
    const ResourceProviderInfo& resourceProviderInfo)
{
  // Calculate the current set of profiles for the resource provider.
  hashset<string> currentProfiles;
  foreachpair (
      const string& profile, const ProfileRecord& record, profileMatrix) {
    if (record.active &&
        isSelectedResourceProvider(record.manifest, resourceProviderInfo)) {
      currentProfiles.insert(profile);
    }
  }

  if (currentProfiles != knownProfiles) {
    return currentProfiles;
  }

  // Wait for the next update if there is no change.
  watchers.emplace_back(knownProfiles, resourceProviderInfo);

  return watchers.back().promise.future();
}


void UriDiskProfileAdaptorProcess::poll()
{
  // NOTE: The flags do not allow relative paths, so this is guaranteed to
  // be either 'http://' or 'https://'.
  if (strings::startsWith(flags.uri, "http")) {
    // NOTE: We already validated that this URI is parsable in the flags.
    Try<http::URL> url = http::URL::parse(flags.uri.string());
    CHECK_SOME(url);

    http::get(url.get())
      .onAny(defer(self(), &Self::_poll, lambda::_1));
  } else {
    __poll(os::read(flags.uri.string()));
  }
}


void UriDiskProfileAdaptorProcess::_poll(const Future<http::Response>& response)
{
  if (response.isReady()) {
    if (response->code == http::Status::OK) {
      __poll(response->body);
    } else {
      __poll(Error("Unexpected HTTP response '" + response->status + "'"));
    }
  } else if (response.isFailed()) {
    __poll(Error(response.failure()));
  } else {
    __poll(Error("Future discarded or abandoned"));
  }
}


void UriDiskProfileAdaptorProcess::__poll(const Try<string>& fetched)
{
  if (fetched.isSome()) {
    Try<DiskProfileMapping> parsed = parseDiskProfileMapping(fetched.get());

    if (parsed.isSome()) {
      notify(parsed.get());
    } else {
      LOG(ERROR) << "Failed to parse result: " << parsed.error();
    }
  } else {
    LOG(WARNING) << "Failed to poll URI: " << fetched.error();
  }

  // TODO(josephw): Do we want to retry if polling fails and no polling
  // interval is set? Or perhaps we should exit in that case?
  if (flags.poll_interval.isSome()) {
    delay(flags.poll_interval.get(), self(), &Self::poll);
  }
}


void UriDiskProfileAdaptorProcess::notify(
    const DiskProfileMapping& parsed)
{
  bool hasErrors = false;

  foreach (const auto& entry, parsed.profile_matrix()) {
    if (!profileMatrix.contains(entry.first)) {
      continue;
    }

    bool matchingCapability =
      entry.second.volume_capabilities() ==
        profileMatrix.at(entry.first).manifest.volume_capabilities();

    bool matchingParameters =
      entry.second.create_parameters() ==
        profileMatrix.at(entry.first).manifest.create_parameters();

    if (!matchingCapability || !matchingParameters) {
      hasErrors = true;

      LOG(WARNING)
        << "Fetched profile mapping for profile '" << entry.first
        << "' does not match earlier data. "
        << "The fetched mapping will be ignored entirely";
    }
  }

  // When encountering a data conflict, this module assumes there is a
  // problem upstream (i.e. in the `--uri`). It is up to the operator
  // to notice and resolve this.
  if (hasErrors) {
    return;
  }

  // TODO(chhsiao): No need to update the profile matrix and send notifications
  // if the parsed mapping has the same size and profile selectors.

  // The fetched mapping satisfies our invariants.

  // Mark disappeared profiles as inactive.
  foreachpair (const string& profile, ProfileRecord& record, profileMatrix) {
    if (parsed.profile_matrix().count(profile) != 1) {
      record.active = false;

      LOG(INFO)
        << "Profile '" << profile << "' is marked inactive "
        << "because it is not in the fetched profile mapping";
    }
  }

  // Save the fetched profile mapping.
  foreach (const auto& entry, parsed.profile_matrix()) {
    profileMatrix.put(entry.first, {entry.second, true});
  }

  // Notify a watcher if its current set of profiles differs from its known set.
  //
  // TODO(josephw): Delay this based on the `--max_random_wait` option.
  foreach (WatcherData& watcher, watchers) {
    hashset<string> current;
    foreachpair (
        const string& profile, const ProfileRecord& record, profileMatrix) {
      if (record.active &&
          isSelectedResourceProvider(record.manifest, watcher.info)) {
        current.insert(profile);
      }
    }

    if (current != watcher.known) {
      CHECK(watcher.promise.set(current))
        << "Promise for watcher '" << watcher.info << "' is already "
        << watcher.promise.future();
    }
  }

  // Remove all notified watchers.
  watchers.erase(
      std::remove_if(
          watchers.begin(),
          watchers.end(),
          [](const WatcherData& watcher) {
            return watcher.promise.future().isReady();
          }),
      watchers.end());

  LOG(INFO)
    << "Updated disk profile mapping to " << parsed.profile_matrix().size()
    << " active profiles";
}

} // namespace storage {
} // namespace internal {
} // namespace mesos {


mesos::modules::Module<DiskProfileAdaptor>
org_apache_mesos_UriDiskProfileAdaptor(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "URI Disk Profile Adaptor module.",
    nullptr,
    [](const Parameters& parameters) -> DiskProfileAdaptor* {
      // Convert `parameters` into a map.
      map<string, string> values;
      foreach (const Parameter& parameter, parameters.parameter()) {
        values[parameter.key()] = parameter.value();
      }

      // Load and validate flags from the map.
      mesos::internal::storage::UriDiskProfileAdaptor::Flags flags;
      Try<flags::Warnings> load = flags.load(values);

      if (load.isError()) {
        LOG(ERROR) << "Failed to parse parameters: " << load.error();
        return nullptr;
      }

      // Log any flag warnings.
      foreach (const flags::Warning& warning, load->warnings) {
        LOG(WARNING) << warning.message;
      }

      return new mesos::internal::storage::UriDiskProfileAdaptor(flags);
    });
