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

#ifndef __ISOLATOR_DOCKER_VOLUME_STATE_HPP__
#define __ISOLATOR_DOCKER_VOLUME_STATE_HPP__

#include <boost/functional/hash.hpp>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include "slave/containerizer/mesos/isolators/docker/volume/state.pb.h"

namespace mesos {
namespace internal {
namespace slave {

inline bool operator==(const DockerVolume& left, const DockerVolume& right)
{
  return (left.driver() == right.driver()) && (left.name() == right.name());
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {


namespace std {

template <>
struct hash<mesos::internal::slave::DockerVolume>
{
  typedef size_t result_type;

  typedef mesos::internal::slave::DockerVolume argument_type;

  result_type operator()(const argument_type& volume) const
  {
    size_t seed = 0;
    boost::hash_combine(seed, std::hash<std::string>()(volume.driver()));
    boost::hash_combine(seed, std::hash<std::string>()(volume.name()));
    return seed;
  }
};

} // namespace std {

#endif // __ISOLATOR_DOCKER_VOLUME_STATE_HPP__
