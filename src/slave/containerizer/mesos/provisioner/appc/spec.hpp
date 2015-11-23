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

#ifndef __PROVISIONER_APPC_SPEC_HPP__
#define __PROVISIONER_APPC_SPEC_HPP__

#include <string>

#include <stout/error.hpp>
#include <stout/option.hpp>

#include <mesos/mesos.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace appc {
namespace spec {

// Validate if the specified image manifest conforms to the Appc spec.
Option<Error> validateManifest(const AppcImageManifest& manifest);

// Validate if the specified image ID conforms to the Appc spec.
Option<Error> validateImageID(const std::string& imageId);

// Validate if the specified image has the disk layout that conforms
// to the Appc spec.
Option<Error> validateLayout(const std::string& imagePath);

// Parse the AppcImageManifest in the specified JSON string.
Try<AppcImageManifest> parse(const std::string& value);

} // namespace spec {
} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_APPC_SPEC_HPP__
