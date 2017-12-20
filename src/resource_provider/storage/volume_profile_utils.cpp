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

#include "resource_provider/storage/volume_profile_utils.hpp"

#include <google/protobuf/util/json_util.h>

#include <stout/bytes.hpp>
#include <stout/foreach.hpp>

using std::string;

using mesos::resource_provider::VolumeProfileMapping;

namespace mesos{
namespace internal {
namespace profile {

Try<VolumeProfileMapping> parseVolumeProfileMapping(
    const string& data)
{
  // Use Google's JSON utility function to parse the JSON string.
  VolumeProfileMapping output;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;

  google::protobuf::util::Status status =
    google::protobuf::util::JsonStringToMessage(data, &output, options);

  if (!status.ok()) {
    return Error(
        "Failed to parse VolumeProfileMapping message: "
        + status.ToString());
  }

  Option<Error> validation = validate(output);
  if (validation.isSome()) {
    return Error(
      "Fetched profile mapping failed validation with: " + validation->message);
  }

  return output;
}


Option<Error> validate(const VolumeProfileMapping& mapping)
{
  auto iterator = mapping.profile_matrix().begin();
  while (iterator != mapping.profile_matrix().end()) {
    if (!iterator->second.has_volume_capabilities()) {
      return Error(
          "Profile '" + iterator->first + "' is missing the required field "
          + "'volume_capabilities");
    }

    Option<Error> capabilities =
      validate(iterator->second.volume_capabilities());

    if (capabilities.isSome()) {
      return Error(
          "Profile '" + iterator->first + "' VolumeCapabilities are invalid: "
          + capabilities->message);
    }

    // NOTE: The `create_parameters` field is optional and needs no further
    // validation after parsing.

    iterator++;
  }

  return None();
}


// TODO(chhsiao): Move this to CSI validation implementation file.
Option<Error> validate(const csi::VolumeCapability& capability)
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
      csi::VolumeCapability::AccessMode::UNKNOWN) {
    return Error("'access_mode.mode' is unknown or not set");
  }

  return None();
}

} // namespace profile {
} // namespace internal {
} // namespace mesos {
