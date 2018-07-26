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

#ifndef __CSI_SPEC_HPP__
#define __CSI_SPEC_HPP__

// ONLY USEFUL AFTER RUNNING PROTOC.
#include <csi/csi.pb.h>

// ONLY USEFUL AFTER RUNNING PROTOC WITH GRPC CPP PLUGIN.
#include <csi/csi.grpc.pb.h>

namespace mesos {
namespace csi {
namespace v0 {

using namespace ::csi::v0;

} // namespace v0 {
} // namespace csi {
} // namespace mesos {

#endif // __CSI_SPEC_HPP__
