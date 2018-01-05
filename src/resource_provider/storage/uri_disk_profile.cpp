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

#include "resource_provider/storage/uri_disk_profile.hpp"

#include <map>
#include <string>
#include <tuple>

#include <mesos/mesos.hpp>

#include <mesos/module/disk_profile.hpp>

#include <mesos/resource_provider/storage/disk_profile.hpp>

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

#include <csi/spec.hpp>
#include <csi/utils.hpp>

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
namespace profile {

bool operator==(
    const Map<string, string>& left,
    const Map<string, string>& right) {
  if (left.size() != right.size()) {
    return false;
  }

  typename Map<string, string>::const_iterator iterator = left.begin();
  while (iterator != left.end()) {
    if (right.count(iterator->first) != 1) {
      return false;
    }

    if (iterator->second != right.at(iterator->first)) {
      return false;
    }
  }

  return true;
}


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
    const std::string& csiPluginInfoType)
{
  return dispatch(
      process.get(),
      &UriDiskProfileAdaptorProcess::translate,
      profile,
      csiPluginInfoType);
}


Future<hashset<string>> UriDiskProfileAdaptor::watch(
    const hashset<string>& knownProfiles,
    const std::string& csiPluginInfoType)
{
  return dispatch(
      process.get(),
      &UriDiskProfileAdaptorProcess::watch,
      knownProfiles,
      csiPluginInfoType);
}


UriDiskProfileAdaptorProcess::UriDiskProfileAdaptorProcess(
    const Flags& _flags)
  : ProcessBase(ID::generate("uri-volume-profile")),
    flags(_flags),
    watchPromise(new Promise<hashset<string>>()) {}


void UriDiskProfileAdaptorProcess::initialize()
{
  poll();
}


Future<DiskProfileAdaptor::ProfileInfo>
  UriDiskProfileAdaptorProcess::translate(
      const string& profile,
      const std::string& csiPluginInfoType)
{
  if (data.count(profile) != 1) {
    return Failure("Profile '" + profile + "' not found");
  }

  return data.at(profile);
}


Future<hashset<string>> UriDiskProfileAdaptorProcess::watch(
    const hashset<string>& knownProfiles,
    const std::string& csiPluginInfoType)
{
  if (profiles != knownProfiles) {
    return profiles;
  }

  return watchPromise->future();
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
      .onAny(defer(self(), [=](const Future<http::Response>& future) {
        if (future.isReady()) {
          // NOTE: We don't check the HTTP status code because we don't know
          // what potential codes are considered successful.
          _poll(future->body);
        } else if (future.isFailed()) {
          _poll(Error(future.failure()));
        } else {
          _poll(Error("Future discarded or abandoned"));
        }
      }));
  } else {
    _poll(os::read(flags.uri.string()));
  }
}


void UriDiskProfileAdaptorProcess::_poll(const Try<string>& fetched)
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

  foreachkey (const string& profile, data) {
    if (parsed.profile_matrix().count(profile) != 1) {
      hasErrors = true;

      LOG(WARNING)
        << "Fetched profile mapping does not contain profile '" << profile
        << "'. The fetched mapping will be ignored entirely";
      continue;
    }

    bool matchingCapability =
      data.at(profile).capability ==
        parsed.profile_matrix().at(profile).volume_capabilities();

    bool matchingParameters =
      data.at(profile).parameters ==
        parsed.profile_matrix().at(profile).create_parameters();

    if (!matchingCapability || !matchingParameters) {
      hasErrors = true;

      LOG(WARNING)
        << "Fetched profile mapping for profile '" << profile << "'"
        << " does not match earlier data."
        << " The fetched mapping will be ignored entirely";
    }
  }

  // When encountering a data conflict, this module assumes there is a
  // problem upstream (i.e. in the `--uri`). It is up to the operator
  // to notice and resolve this.
  if (hasErrors) {
    return;
  }

  // Profiles can only be added, so if the parsed data is the same size,
  // nothing has changed and no notifications need to be sent.
  if (parsed.profile_matrix().size() <= data.size()) {
    return;
  }

  // The fetched mapping satisfies our invariants.

  // Save the protobuf as a map we can expose through the module interface.
  // And update the convenience set of profile names.
  profiles.clear();
  auto iterator = parsed.profile_matrix().begin();
  while (iterator != parsed.profile_matrix().end()) {
    data[iterator->first] = {
      iterator->second.volume_capabilities(),
      iterator->second.create_parameters()
    };

    profiles.insert(iterator->first);
    iterator++;
  }

  // Notify any watchers and then prepare a new promise for the next
  // iteration of polling.
  //
  // TODO(josephw): Delay this based on the `--max_random_wait` option.
  watchPromise->set(profiles);
  watchPromise.reset(new Promise<hashset<string>>());

  LOG(INFO)
    << "Updated disk profile mapping to " << profiles.size()
    << " total profiles";
}

} // namespace profile {
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
      mesos::internal::profile::Flags flags;
      Try<flags::Warnings> load = flags.load(values);

      if (load.isError()) {
        LOG(ERROR) << "Failed to parse parameters: " << load.error();
        return nullptr;
      }

      // Log any flag warnings.
      foreach (const flags::Warning& warning, load->warnings) {
        LOG(WARNING) << warning.message;
      }

      return new mesos::internal::profile::UriDiskProfileAdaptor(flags);
    });
