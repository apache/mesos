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

#include <memory>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/util/message_differencer.h>

namespace mesos {
namespace typeutils {
namespace internal {

// Creates a protobuf `MessageDifferencer` specifically configured for
// comparing `FrameworkInfo`s (either V0 or V1 ones).
//
// TODO(asekretenko): Remove the `unique_ptr` wrapper if `MessageDifferencer`
// becomes move-constructible in future versions of protobuf.
template <class TFrameworkInfo>
std::unique_ptr<::google::protobuf::util::MessageDifferencer>
createFrameworkInfoDifferencer()
{
  static const ::google::protobuf::Descriptor* descriptor =
    TFrameworkInfo::descriptor();

  CHECK_EQ(13, descriptor->field_count())
    << "After adding a field to FrameworkInfo, please make sure "
    << "that FrameworkInfoDifferencer handles this field properly;"
    << "after that, adjust the expected fields count in this check.";

  std::unique_ptr<::google::protobuf::util::MessageDifferencer> differencer{
    new ::google::protobuf::util::MessageDifferencer()};

  differencer->TreatAsSet(descriptor->FindFieldByName("capabilities"));
  differencer->TreatAsSet(descriptor->FindFieldByName("roles"));
  return differencer;
}

} // namespace internal {
} // namespace typeutils {
} // namespace mesos {
