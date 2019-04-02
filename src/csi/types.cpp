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

#include <mesos/csi/types.hpp>

#include <google/protobuf/util/message_differencer.h>

using google::protobuf::util::MessageDifferencer;

namespace mesos {
namespace csi {
namespace types {

bool operator==(const VolumeCapability& left, const VolumeCapability& right)
{
  // NOTE: `MessageDifferencer::Equivalent` would ignore unknown fields and load
  // default values for unset fields (which are indistinguishable in proto3).
  return MessageDifferencer::Equivalent(left, right);
}

} // namespace types {
} // namespace csi {
} // namespace mesos {
