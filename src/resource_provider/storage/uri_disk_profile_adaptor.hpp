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

#ifndef __RESOURCE_PROVIDER_URI_DISK_PROFILE_ADAPTOR_HPP__
#define __RESOURCE_PROVIDER_URI_DISK_PROFILE_ADAPTOR_HPP__

#include <list>
#include <string>
#include <tuple>

#include <mesos/resource_provider/storage/disk_profile_adaptor.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <process/ssl/flags.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/flags.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include "resource_provider/storage/disk_profile_utils.hpp"

namespace mesos {
namespace internal {
namespace storage {

// Forward declaration.
class UriDiskProfileAdaptorProcess;

// The `UriDiskProfileAdaptor` is an example DiskProfileAdaptor module
// that takes a URI as a module parameter and fetches that URI
// periodically. The fetched data is parsed into the required CSI
// protobufs (which also acts as validation).
//
// If there is an error during fetching, any previously fetched results
// will be used until fetching is successful.
//
// This module does not filter return results based on
// `CSIPluginInfo::type` and assumes that all fetched profiles are meant
// for all resource providers.
//
// See `UriDiskProfileAdaptor::Flags` below for more information.
class UriDiskProfileAdaptor : public DiskProfileAdaptor
{
public:
  struct Flags : public virtual flags::FlagsBase
  {
    Flags()
    {
      add(&Flags::uri,
          "uri",
          None(),
          "URI to a JSON object containing the disk profile mapping.\n"
          "This module supports both HTTP(s) and file URIs\n."
          "\n"
          "The JSON object should consist of some top-level string keys\n"
          "corresponding to the disk profile name. Each value should contain\n"
          "a `ResourceProviderSelector` under 'resource_provider_selector' or\n"
          "a `CSIPluginTypeSelector` under 'csi_plugin_type_selector' to\n"
          "specify the set of resource providers this profile applies to,\n"
          "followed by a `VolumeCapability` under 'volume_capabilities'\n"
          "and a free-form string-string mapping under 'create_parameters'.\n"
          "\n"
          "The JSON is modeled after a protobuf found in\n"
          "`src/resource_provider/storage/disk_profile.proto`.\n"
          "\n"
          "For example:\n"
          "{\n"
          "  \"profile_matrix\" : {\n"
          "    \"my-profile\" : {\n"
          "      \"csi_plugin_type_selector\": {\n"
          "        \"plugin_type\" : \"org.apache.mesos.csi.test\"\n"
          "      \"},\n"
          "      \"volume_capabilities\" : {\n"
          "        \"block\" : {},\n"
          "        \"access_mode\" : { \"mode\" : \"SINGLE_NODE_WRITER\" }\n"
          "      },\n"
          "      \"create_parameters\" : {\n"
          "        \"mesos-does-not\" : \"interpret-these\",\n"
          "        \"type\" : \"raid5\",\n"
          "        \"stripes\" : \"3\",\n"
          "        \"stripesize\" : \"64\"\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}",
          static_cast<const Path*>(nullptr),
          [](const Path& value) -> Option<Error> {
            // For now, just check if the URI has a supported scheme.
            //
            // TODO(josephw): Once we have a proper URI class and parser,
            // consider validating this URI more thoroughly.
            if (strings::startsWith(value.string(), "http://")
#ifdef USE_SSL_SOCKET
                || (process::network::openssl::flags().enabled &&
                    strings::startsWith(value.string(), "https://"))
#endif // USE_SSL_SOCKET
            ) {
              Try<process::http::URL> url =
                process::http::URL::parse(value.string());

              if (url.isError()) {
                return Error("Failed to parse URI: " + url.error());
              }

              return None();
            }

            // NOTE: The `Path` class will strip off the 'file://' prefix.
            if (strings::contains(value.string(), "://")) {
              return Error(
                  "--uri must use a supported scheme (file or http(s))");
            }

            // We only allow absolute paths for file paths.
            if (!value.is_absolute()) {
              return Error("--uri to a file must be an absolute path");
            }

            return None();
          });

      add(&Flags::poll_interval,
          "poll_interval",
          "How long to wait between polling the specified `--uri`.\n"
          "The time is checked each time the `translate` method is called.\n"
          "If the given time has elapsed, then the URI is re-fetched.\n"
          "If not specified, the URI is only fetched once.",
          [](const Option<Duration>& value) -> Option<Error> {
            if (value.isSome() && value.get() <= Seconds(0)) {
              return Error("--poll_interval must be non-negative");
            }

            return None();
          });

      add(&Flags::max_random_wait,
          "max_random_wait",
          "How long at most to wait between discovering a new set of profiles\n"
          "and notifying the callers of `watch`. The actual wait time is a\n"
          "uniform random value between 0 and this value. If `--uri` points\n"
          "to a centralized location, it may be good to scale this number\n"
          "according to the number of resource providers in the cluster.",
          Seconds(0),
          [](const Duration& value) -> Option<Error> {
            if (value < Seconds(0)) {
              return Error("--max_random_wait must be zero or greater");
            }

            return None();
          });
    }

    // NOTE: We use the `Path` type here so that the stout flags parser
    // does not attempt to read a file if given a `file://` prefixed value.
    //
    // TODO(josephw): Replace with a URI type when stout gets one.
    Path uri;

    Option<Duration> poll_interval;
    Duration max_random_wait;
  };


  UriDiskProfileAdaptor(const Flags& _flags);

  ~UriDiskProfileAdaptor() override;

  process::Future<DiskProfileAdaptor::ProfileInfo> translate(
      const std::string& profile,
      const ResourceProviderInfo& resourceProviderInfo) override;

  process::Future<hashset<std::string>> watch(
      const hashset<std::string>& knownProfiles,
      const ResourceProviderInfo& resourceProviderInfo) override;

protected:
  Flags flags;
  process::Owned<UriDiskProfileAdaptorProcess> process;
};


class UriDiskProfileAdaptorProcess :
  public process::Process<UriDiskProfileAdaptorProcess>
{
public:
  UriDiskProfileAdaptorProcess(const UriDiskProfileAdaptor::Flags& _flags);

  void initialize() override;

  process::Future<DiskProfileAdaptor::ProfileInfo> translate(
      const std::string& profile,
      const ResourceProviderInfo& resourceProviderInfo);

  process::Future<hashset<std::string>> watch(
      const hashset<std::string>& knownProfiles,
      const ResourceProviderInfo& resourceProviderInfo);

  // Helpers for fetching the `--uri`.
  // If `--poll_interval` is set, this method will dispatch to itself with
  // a delay once the fetch is complete.
  // Made public for testing purpose.
  void poll();
  void _poll(const process::Future<process::http::Response>& future);
  void __poll(const Try<std::string>& fetched);

private:
  // Helper that is called upon successfully polling and parsing the `--uri`.
  // This method will validate that the capability and parameters of a known
  // profile must remain the same. Then, any watcher will be notified if its set
  // of profiles has been changed.
  void notify(const resource_provider::DiskProfileMapping& parsed);

  UriDiskProfileAdaptor::Flags flags;

  struct ProfileRecord
  {
    resource_provider::DiskProfileMapping::CSIManifest manifest;

    // True if the profile is seen in the last fetched profile mapping.
    bool active;
  };

  // The mapping of all profiles seen so far.
  // Profiles can only be added and never removed from this mapping. Once added,
  // a profile's volume capability and parameters cannot be changed.
  //
  // TODO(josephw): Consider persisting this mapping across agent restarts.
  hashmap<std::string, ProfileRecord> profileMatrix;

  struct WatcherData
  {
    WatcherData(
        const hashset<std::string>& _known, const ResourceProviderInfo& _info)
      : known(_known), info(_info) {}

    hashset<std::string> known;
    ResourceProviderInfo info;
    process::Promise<hashset<std::string>> promise;
  };

  std::vector<WatcherData> watchers;
};

} // namespace storage {
} // namespace internal {
} // namespace mesos {

#endif // __RESOURCE_PROVIDER_URI_DISK_PROFILE_ADAPTOR_HPP__
