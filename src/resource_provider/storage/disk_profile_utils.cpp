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

#include "resource_provider/storage/disk_profile_utils.hpp"

#include <google/protobuf/util/json_util.h>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/unreachable.hpp>

using std::string;

using mesos::resource_provider::DiskProfileMapping;

namespace mesos {
namespace internal {
namespace storage {

Try<DiskProfileMapping> parseDiskProfileMapping(
    const string& data)
{
  // Use Google's JSON utility function to parse the JSON string.
  DiskProfileMapping output;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;

  google::protobuf::util::Status status =
    google::protobuf::util::JsonStringToMessage(data, &output, options);

  if (!status.ok()) {
    return Error(
        "Failed to parse DiskProfileMapping message: " +
        status.ToString());
  }

  Option<Error> validation = validate(output);
  if (validation.isSome()) {
    return Error(
        "Fetched profile mapping failed validation with: " +
        validation->message);
  }

  return output;
}


bool isSelectedResourceProvider(
    const DiskProfileMapping::CSIManifest& profileManifest,
    const ResourceProviderInfo& resourceProviderInfo)
{
  switch (profileManifest.selector_case()) {
    case DiskProfileMapping::CSIManifest::kResourceProviderSelector: {
      CHECK(profileManifest.has_resource_provider_selector());

      const auto& selector = profileManifest.resource_provider_selector();

      foreach (const auto& resourceProvider, selector.resource_providers()) {
        if (resourceProviderInfo.type() == resourceProvider.type() &&
            resourceProviderInfo.name() == resourceProvider.name()) {
          return true;
        }
      }

      return false;
    }
    case DiskProfileMapping::CSIManifest::kCsiPluginTypeSelector: {
      CHECK(profileManifest.has_csi_plugin_type_selector());

      const auto& selector = profileManifest.csi_plugin_type_selector();

      return resourceProviderInfo.has_storage() &&
        resourceProviderInfo.storage().plugin().type() ==
          selector.plugin_type();
    }
    case DiskProfileMapping::CSIManifest::SELECTOR_NOT_SET: {
      UNREACHABLE();
    }
  }

  UNREACHABLE();
}


static Option<Error> validateSelector(
    const DiskProfileMapping::CSIManifest& profileManifest)
{
  switch (profileManifest.selector_case()) {
    case DiskProfileMapping::CSIManifest::kResourceProviderSelector: {
      if (!profileManifest.has_resource_provider_selector()) {
        return Error("Expecting 'resource_provider_selector' to be present");
      }

      const auto& selector = profileManifest.resource_provider_selector();

      foreach (const auto& resourceProvider, selector.resource_providers()) {
        if (resourceProvider.type().empty()) {
          return Error(
              "'type' is a required field for ResourceProviderSelector");
        }

        if (resourceProvider.name().empty()) {
          return Error(
              "'name' is a required field for ResourceProviderSelector");
        }
      }

      break;
    }
    case DiskProfileMapping::CSIManifest::kCsiPluginTypeSelector: {
      if (!profileManifest.has_csi_plugin_type_selector()) {
        return Error("Expecting 'csi_plugin_type_selector' to be present");
      }

      const auto& selector = profileManifest.csi_plugin_type_selector();

      if (selector.plugin_type().empty()) {
        return Error(
            "'plugin_type' is a required field for CSIPluginTypeSelector");
      }

      break;
    }
    case DiskProfileMapping::CSIManifest::SELECTOR_NOT_SET: {
      return Error("Expecting a selector to be present");
    }
  }

  return None();
}


Option<Error> validate(const DiskProfileMapping& mapping)
{
  foreach (const auto& entry, mapping.profile_matrix()) {
    Option<Error> selector = validateSelector(entry.second);

    if (selector.isSome()) {
      return Error(
          "Profile '" + entry.first + "' has no valid selector: " +
          selector->message);
    }

    if (!entry.second.has_volume_capabilities()) {
      return Error(
          "Profile '" + entry.first +
          "' is missing the required field 'volume_capabilities'");
    }

    Option<Error> capabilities = validate(entry.second.volume_capabilities());

    if (capabilities.isSome()) {
      return Error(
          "Profile '" + entry.first + "' has an invalid VolumeCapability: " +
          capabilities->message);
    }

    // NOTE: The `create_parameters` field is optional and needs no further
    // validation after parsing.
  }

  return None();
}


// TODO(chhsiao): Move this to CSI validation implementation file.
Option<Error> validate(const csi::v0::VolumeCapability& capability)
{
  if (capability.has_mount()) {
    // The total size of this repeated field may not exceed 4 KB.
    //
    // TODO(josephw): The specification does not state how this maximum
    // size is calculated. So this check is conservative and does not
    // include padding or array separators in the size calculation.
    size_t size = 0;
    foreach (const string& flag, capability.mount().mount_flags()) {
      size += flag.size();
    }

    if (Bytes(size) > Kilobytes(4)) {
      return Error("Size of 'mount_flags' may not exceed 4 KB");
    }
  }

  if (!capability.has_access_mode()) {
    return Error("'access_mode' is a required field");
  }

  if (capability.access_mode().mode() ==
      csi::v0::VolumeCapability::AccessMode::UNKNOWN) {
    return Error("'access_mode.mode' is unknown or not set");
  }

  return None();
}

} // namespace storage {
} // namespace internal {
} // namespace mesos {
