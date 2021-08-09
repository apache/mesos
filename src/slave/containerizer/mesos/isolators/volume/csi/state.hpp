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

#ifndef __VOLUME_CSI_ISOLATOR_STATE_HPP__
#define __VOLUME_CSI_ISOLATOR_STATE_HPP__

#include <string>

#include <boost/functional/hash.hpp>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include "slave/containerizer/mesos/isolators/volume/csi/state.pb.h"

namespace mesos {
namespace internal {
namespace slave {

inline bool operator==(const CSIVolume& left, const CSIVolume& right)
{
  return (left.plugin_name() == right.plugin_name()) &&
           (left.id() == right.id());
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {


namespace std {

template <>
struct hash<mesos::internal::slave::CSIVolume>
{
  typedef size_t result_type;

  typedef mesos::internal::slave::CSIVolume argument_type;

  result_type operator()(const argument_type& volume) const
  {
    size_t seed = 0;
    boost::hash_combine(seed, std::hash<std::string>()(volume.plugin_name()));
    boost::hash_combine(seed, std::hash<std::string>()(volume.id()));
    return seed;
  }
};

} // namespace std {

#endif // __VOLUME_CSI_ISOLATOR_STATE_HPP__
