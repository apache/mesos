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

#ifndef __RESOURCE_PROVIDER_URI_VOLUME_PROFILE_HPP__
#define __RESOURCE_PROVIDER_URI_VOLUME_PROFILE_HPP__

#include <map>
#include <string>
#include <tuple>

#include <mesos/resource_provider/storage/volume_profile.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <process/ssl/flags.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <csi/spec.hpp>

#include "resource_provider/storage/volume_profile_utils.hpp"

namespace mesos {
namespace internal {
namespace profile {

// Forward declaration.
class UriVolumeProfileAdaptorProcess;

struct Flags : public virtual flags::FlagsBase
{
  Flags()
  {
    add(&Flags::uri,
        "uri",
        None(),
        "URI to a JSON object containing the volume profile mapping.\n"
        "This module supports both HTTP(s) and file URIs\n."
        "\n"
        "The JSON object should consist of some top-level string keys\n"
        "corresponding to the volume profile name. Each value should\n"
        "contain a `VolumeCapability` under a 'volume_capabilities'\n"
        "and a free-form string-string mapping under 'create_parameters'.\n"
        "\n"
        "The JSON is modeled after a protobuf found in\n"
        "`src/csi/uri_volume_profile.proto`.\n"
        "\n"
        "For example:\n"
        "{\n"
        "  \"profile_matrix\" : {\n"
        "    \"my-profile\" : {\n"
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
            return Error("--uri must use a supported scheme (file or http(s))");
          }

          // We only allow absolute paths for file paths.
          if (!value.absolute()) {
            return Error("--uri to a file must be an absolute path");
          }

          return None();
        });

    add(&Flags::poll_interval,
        "poll_interval",
        "How long to wait between polling the specified `--uri`.\n"
        "The time is checked each time the `translate` method is called.\n"
        "If the given time has elapsed, then the URI is re-fetched."
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
        "uniform random value between 0 and this value. If the `--uri` points\n"
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


// The `UriVolumeProfileAdaptor` is an example VolumeProfile module that
// takes a URI as a module parameter and fetches that URI periodically.
// The fetched data is parsed into the required CSI protobufs
// (which also acts as validation).
//
// If there is an error during fetching, any previously fetched results
// will be used until fetching is successful.
//
// This module does not filter return results based on `CSIPluginInfo::type`
// and assumes that all fetched profiles are meant for all resource providers.
//
// See `Flags` above for more information.
class UriVolumeProfileAdaptor : public mesos::VolumeProfileAdaptor
{
public:
  UriVolumeProfileAdaptor(const Flags& _flags);

  virtual ~UriVolumeProfileAdaptor();

  virtual process::Future<mesos::VolumeProfileAdaptor::ProfileInfo> translate(
      const std::string& profile,
      const std::string& csiPluginInfoType) override;

  virtual process::Future<hashset<std::string>> watch(
      const hashset<std::string>& knownProfiles,
      const std::string& csiPluginInfoType) override;

protected:
  Flags flags;
  process::Owned<UriVolumeProfileAdaptorProcess> process;
};


class UriVolumeProfileAdaptorProcess :
  public process::Process<UriVolumeProfileAdaptorProcess>
{
public:
  UriVolumeProfileAdaptorProcess(const Flags& _flags);

  virtual void initialize() override;

  process::Future<mesos::VolumeProfileAdaptor::ProfileInfo> translate(
      const std::string& profile,
      const std::string& csiPluginInfoType);

  process::Future<hashset<std::string>> watch(
      const hashset<std::string>& knownProfiles,
      const std::string& csiPluginInfoType);

private:
  // Helpers for fetching the `--uri`.
  // If `--poll_interval` is set, this method will dispatch to itself with
  // a delay once the fetch is complete.
  void poll();
  void _poll(const Try<std::string>& fetched);

  // Helper that is called upon successfully polling and parsing the `--uri`.
  // This method will check the following conditions before updating the state
  // of the module:
  //   * All known profiles must be included in the updated set.
  //   * All properties of known profiles must match those in the updated set.
  void notify(const resource_provider::VolumeProfileMapping& parsed);

private:
  Flags flags;

  // The last fetched profile mapping.
  // This module assumes that profiles can only be added and never removed.
  // Once added, profiles cannot be changed either.
  //
  // TODO(josephw): Consider persisting this mapping across agent restarts.
  std::map<std::string, VolumeProfileAdaptor::ProfileInfo> data;

  // Convenience set of the keys in `data` above.
  // This module does not filter based on `CSIPluginInfo::type`, so this
  // is valid for all input to `watch(...)`.
  hashset<std::string> profiles;

  // Will be satisfied whenever `data` is changed.
  process::Owned<process::Promise<hashset<std::string>>> watchPromise;
};

} // namespace profile {
} // namespace internal {
} // namespace mesos {

#endif // __RESOURCE_PROVIDER_URI_VOLUME_PROFILE_HPP__
