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

#ifndef __RESOURCE_PROVIDER_URI_VOLUME_PROFILE_UTILS_HPP__
#define __RESOURCE_PROVIDER_URI_VOLUME_PROFILE_UTILS_HPP__

#include <stout/option.hpp>
#include <stout/try.hpp>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include "resource_provider/storage/volume_profile.pb.h"

namespace mesos {
namespace internal {
namespace profile {

// Helper for parsing a string as the expected data format.
Try<resource_provider::VolumeProfileMapping> parseVolumeProfileMapping(
    const std::string& data);


// Checks the fields inside a `VolumeProfileMapping` according to the
// comments above the protobuf.
Option<Error> validate(const resource_provider::VolumeProfileMapping& mapping);


// Checks the fields inside a `VolumeCapability` according to the
// comments above the protobuf.
Option<Error> validate(const csi::VolumeCapability& capability);

} // namespace profile {
} // namespace internal {
} // namespace mesos {

#endif // __RESOURCE_PROVIDER_URI_VOLUME_PROFILE_HPP__
