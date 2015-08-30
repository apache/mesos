/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __MESOS_V1_HPP__
#define __MESOS_V1_HPP__

#include <ostream>

#include <boost/functional/hash.hpp>

#include <mesos/v1/mesos.pb.h> // ONLY USEFUL AFTER RUNNING PROTOC.

namespace mesos {
namespace v1 {

bool operator==(const CommandInfo& left, const CommandInfo& right);
bool operator==(const CommandInfo::URI& left, const CommandInfo::URI& right);
bool operator==(const Credential& left, const Credential& right);
bool operator==(const Environment& left, const Environment& right);
bool operator==(const ExecutorInfo& left, const ExecutorInfo& right);
bool operator==(const MasterInfo& left, const MasterInfo& right);

bool operator==(
    const ResourceStatistics& left,
    const ResourceStatistics& right);

bool operator==(const AgentInfo& left, const AgentInfo& right);
bool operator==(const Volume& left, const Volume& right);

bool operator==(const URL& left, const URL& right);

bool operator==(const TaskStatus& left, const TaskStatus& right);
bool operator!=(const TaskStatus& left, const TaskStatus& right);

inline bool operator==(const ContainerID& left, const ContainerID& right)
{
  return left.value() == right.value();
}


inline bool operator==(const ExecutorID& left, const ExecutorID& right)
{
  return left.value() == right.value();
}


inline bool operator==(const FrameworkID& left, const FrameworkID& right)
{
  return left.value() == right.value();
}


inline bool operator==(const FrameworkInfo& left, const FrameworkInfo& right)
{
  return (left.name() == right.name()) && (left.user() == right.user());
}


inline bool operator==(const OfferID& left, const OfferID& right)
{
  return left.value() == right.value();
}


inline bool operator==(const AgentID& left, const AgentID& right)
{
  return left.value() == right.value();
}


inline bool operator==(const TaskID& left, const TaskID& right)
{
  return left.value() == right.value();
}


inline bool operator==(const TimeInfo& left, const TimeInfo& right)
{
  return left.nanoseconds() == right.nanoseconds();
}


inline bool operator==(const DurationInfo& left, const DurationInfo& right)
{
  return left.nanoseconds() == right.nanoseconds();
}


inline bool operator==(const ContainerID& left, const std::string& right)
{
  return left.value() == right;
}


inline bool operator==(const ExecutorID& left, const std::string& right)
{
  return left.value() == right;
}


inline bool operator==(const FrameworkID& left, const std::string& right)
{
  return left.value() == right;
}


inline bool operator==(const OfferID& left, const std::string& right)
{
  return left.value() == right;
}


inline bool operator==(const AgentID& left, const std::string& right)
{
  return left.value() == right;
}


inline bool operator==(const TaskID& left, const std::string& right)
{
  return left.value() == right;
}


inline bool operator!=(const TimeInfo& left, const TimeInfo& right)
{
  return !(left == right);
}


inline bool operator!=(const DurationInfo& left, const DurationInfo& right)
{
  return !(left == right);
}


inline bool operator!=(const ContainerID& left, const ContainerID& right)
{
  return left.value() != right.value();
}


inline bool operator!=(const ExecutorID& left, const ExecutorID& right)
{
  return left.value() != right.value();
}


inline bool operator!=(const FrameworkID& left, const FrameworkID& right)
{
  return left.value() != right.value();
}


inline bool operator!=(const AgentID& left, const AgentID& right)
{
  return left.value() != right.value();
}


inline bool operator<(const ContainerID& left, const ContainerID& right)
{
  return left.value() < right.value();
}


inline bool operator<(const ExecutorID& left, const ExecutorID& right)
{
  return left.value() < right.value();
}


inline bool operator<(const FrameworkID& left, const FrameworkID& right)
{
  return left.value() < right.value();
}


inline bool operator<(const OfferID& left, const OfferID& right)
{
  return left.value() < right.value();
}


inline bool operator<(const AgentID& left, const AgentID& right)
{
  return left.value() < right.value();
}


inline bool operator<(const TaskID& left, const TaskID& right)
{
  return left.value() < right.value();
}


inline std::ostream& operator<<(std::ostream& stream, const ACLs& acls)
{
  return stream << acls.DebugString();
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const ContainerID& containerId)
{
  return stream << containerId.value();
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const ContainerInfo& containerInfo)
{
  return stream << containerInfo.DebugString();
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const ExecutorID& executorId)
{
  return stream << executorId.value();
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const ExecutorInfo& executor)
{
  return stream << executor.DebugString();
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const FrameworkID& frameworkId)
{
  return stream << frameworkId.value();
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const MachineID& machineId)
{
  if (machineId.has_hostname() && machineId.has_ip()) {
    return stream << machineId.hostname() << " (" << machineId.ip() << ")";
  }

  // If only a hostname is present.
  if (machineId.has_hostname()) {
    return stream << machineId.hostname();
  } else { // If there is no hostname, then there is an IP.
    return stream << "(" << machineId.ip() << ")";
  }
}


inline std::ostream& operator<<(std::ostream& stream, const MasterInfo& master)
{
  return stream << master.DebugString();
}


inline std::ostream& operator<<(std::ostream& stream, const OfferID& offerId)
{
  return stream << offerId.value();
}


inline std::ostream& operator<<(std::ostream& stream, const RateLimits& limits)
{
  return stream << limits.DebugString();
}


inline std::ostream& operator<<(std::ostream& stream, const AgentID& agentId)
{
  return stream << agentId.value();
}


inline std::ostream& operator<<(std::ostream& stream, const AgentInfo& agent)
{
  return stream << agent.DebugString();
}


inline std::ostream& operator<<(std::ostream& stream, const TaskID& taskId)
{
  return stream << taskId.value();
}


inline std::ostream& operator<<(std::ostream& stream, const TaskInfo& task)
{
  return stream << task.DebugString();
}


inline std::ostream& operator<<(std::ostream& stream, const TaskState& state)
{
  return stream << TaskState_Name(state);
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const std::vector<TaskID>& taskIds)
{
  stream << "[ ";
  for (auto it = taskIds.begin(); it != taskIds.end(); ++it) {
    if (it != taskIds.begin()) {
      stream << ", ";
    }
    stream << *it;
  }
  stream << " ]";
  return stream;
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const FrameworkInfo::Capability& capability)
{
  return stream << FrameworkInfo::Capability::Type_Name(capability.type());
}


template <typename T>
inline std::ostream& operator<<(
    std::ostream& stream,
    const google::protobuf::RepeatedPtrField<T>& messages)
{
  stream << "[ ";
  for (auto it = messages.begin(); it != messages.end(); ++it) {
    if (it != messages.begin()) {
      stream << ", ";
    }
    stream << *it;
  }
  stream << " ]";
  return stream;
}

} // namespace v1 {
} // namespace mesos {

namespace std {

template <>
struct hash<mesos::v1::CommandInfo::URI>
{
  typedef size_t result_type;

  typedef mesos::v1::CommandInfo::URI argument_type;

  result_type operator()(const argument_type& uri) const
  {
    size_t seed = 0;

    if (uri.extract()) {
      seed += 11;
    }

    if (uri.executable()) {
      seed += 2003;
    }

    boost::hash_combine(seed, uri.value());
    return seed;
  }
};


template <>
struct hash<mesos::v1::ContainerID>
{
  typedef size_t result_type;

  typedef mesos::v1::ContainerID argument_type;

  result_type operator()(const argument_type& containerId) const
  {
    size_t seed = 0;
    boost::hash_combine(seed, containerId.value());
    return seed;
  }
};


template <>
struct hash<mesos::v1::ExecutorID>
{
  typedef size_t result_type;

  typedef mesos::v1::ExecutorID argument_type;

  result_type operator()(const argument_type& executorId) const
  {
    size_t seed = 0;
    boost::hash_combine(seed, executorId.value());
    return seed;
  }
};


template <>
struct hash<mesos::v1::FrameworkID>
{
  typedef size_t result_type;

  typedef mesos::v1::FrameworkID argument_type;

  result_type operator()(const argument_type& frameworkId) const
  {
    size_t seed = 0;
    boost::hash_combine(seed, frameworkId.value());
    return seed;
  }
};


template <>
struct hash<mesos::v1::OfferID>
{
  typedef size_t result_type;

  typedef mesos::v1::OfferID argument_type;

  result_type operator()(const argument_type& offerId) const
  {
    size_t seed = 0;
    boost::hash_combine(seed, offerId.value());
    return seed;
  }
};


template <>
struct hash<mesos::v1::AgentID>
{
  typedef size_t result_type;

  typedef mesos::v1::AgentID argument_type;

  result_type operator()(const argument_type& agentId) const
  {
    size_t seed = 0;
    boost::hash_combine(seed, agentId.value());
    return seed;
  }
};


template <>
struct hash<mesos::v1::TaskID>
{
  typedef size_t result_type;

  typedef mesos::v1::TaskID argument_type;

  result_type operator()(const argument_type& taskId) const
  {
    size_t seed = 0;
    boost::hash_combine(seed, taskId.value());
    return seed;
  }
};

} // namespace std {

#endif // __MESOS_V1_HPP__
