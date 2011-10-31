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

#ifndef __TYPE_UTILS_HPP__
#define __TYPE_UTILS_HPP__

#include <google/protobuf/descriptor.h>

#include <mesos/mesos.hpp>

#include <boost/functional/hash.hpp>

#include "common/resources.hpp"
#include "messages/messages.hpp"


namespace mesos {

inline std::ostream& operator << (std::ostream& stream, const FrameworkID& frameworkId)
{
  stream << frameworkId.value();
  return stream;
}


inline std::ostream& operator << (std::ostream& stream, const OfferID& offerId)
{
  stream << offerId.value();
  return stream;
}


inline std::ostream& operator << (std::ostream& stream, const SlaveID& slaveId)
{
  stream << slaveId.value();
  return stream;
}


inline std::ostream& operator << (std::ostream& stream, const TaskID& taskId)
{
  stream << taskId.value();
  return stream;
}


inline std::ostream& operator << (std::ostream& stream, const ExecutorID& executorId)
{
  stream << executorId.value();
  return stream;
}


inline std::ostream& operator << (std::ostream& stream, const TaskState& state)
{
  stream << TaskState_descriptor()->FindValueByNumber(state)->name();
  return stream;
}


inline bool operator == (const FrameworkID& left, const FrameworkID& right)
{
  return left.value() == right.value();
}


inline bool operator == (const OfferID& left, const OfferID& right)
{
  return left.value() == right.value();
}


inline bool operator == (const SlaveID& left, const SlaveID& right)
{
  return left.value() == right.value();
}


inline bool operator == (const TaskID& left, const TaskID& right)
{
  return left.value() == right.value();
}


inline bool operator == (const ExecutorID& left, const ExecutorID& right)
{
  return left.value() == right.value();
}


inline bool operator == (const FrameworkID& left, const std::string& right)
{
  return left.value() == right;
}


inline bool operator == (const OfferID& left, const std::string& right)
{
  return left.value() == right;
}


inline bool operator == (const SlaveID& left, const std::string& right)
{
  return left.value() == right;
}


inline bool operator == (const TaskID& left, const std::string& right)
{
  return left.value() == right;
}


inline bool operator == (const ExecutorID& left, const std::string& right)
{
  return left.value() == right;
}


inline bool operator < (const FrameworkID& left, const FrameworkID& right)
{
  return left.value() < right.value();
}


inline bool operator < (const OfferID& left, const OfferID& right)
{
  return left.value() < right.value();
}


inline bool operator < (const SlaveID& left, const SlaveID& right)
{
  return left.value() < right.value();
}


inline bool operator < (const TaskID& left, const TaskID& right)
{
  return left.value() < right.value();
}


inline bool operator < (const ExecutorID& left, const ExecutorID& right)
{
  return left.value() < right.value();
}


inline std::size_t hash_value(const FrameworkID& frameworkId)
{
  size_t seed = 0;
  boost::hash_combine(seed, frameworkId.value());
  return seed;
}


inline std::size_t hash_value(const OfferID& offerId)
{
  size_t seed = 0;
  boost::hash_combine(seed, offerId.value());
  return seed;
}


inline std::size_t hash_value(const SlaveID& slaveId)
{
  size_t seed = 0;
  boost::hash_combine(seed, slaveId.value());
  return seed;
}


inline std::size_t hash_value(const TaskID& taskId)
{
  size_t seed = 0;
  boost::hash_combine(seed, taskId.value());
  return seed;
}


inline std::size_t hash_value(const ExecutorID& executorId)
{
  size_t seed = 0;
  boost::hash_combine(seed, executorId.value());
  return seed;
}


namespace internal {

inline std::ostream& operator << (std::ostream& stream, const Task* task)
{
  stream << "task " << task->framework_id() << ":" << task->task_id();
  return stream;
}

}} // namespace mesos { namespace internal {

#endif // __TYPE_UTILS_HPP__
