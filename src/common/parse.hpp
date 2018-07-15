// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __COMMON_PARSE_HPP__
#define __COMMON_PARSE_HPP__

#include <mesos/mesos.hpp>

#include <mesos/authorizer/acls.hpp>

#include <mesos/module/module.hpp>

#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/flags/parse.hpp>

#include "messages/messages.hpp"

namespace flags {

template <>
inline Try<mesos::ACLs> parse(const std::string& value)
{
  // Convert from string or file to JSON.
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  // Convert from JSON to Protobuf.
  return protobuf::parse<mesos::ACLs>(json.get());
}


template <>
inline Try<mesos::RateLimits> parse(const std::string& value)
{
  // Convert from string or file to JSON.
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  // Convert from JSON to Protobuf.
  return protobuf::parse<mesos::RateLimits>(json.get());
}


template <>
inline Try<mesos::Modules> parse(const std::string& value)
{
  // Convert from string or file to JSON.
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  // Convert from JSON to Protobuf.
  return protobuf::parse<mesos::Modules>(json.get());
}


template <>
inline Try<mesos::ContainerInfo> parse(const std::string& value)
{
  // Convert from string or file to JSON.
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  // Convert from JSON to Protobuf.
  return protobuf::parse<mesos::ContainerInfo>(json.get());
}


template <>
inline Try<mesos::DeviceWhitelist> parse(const std::string& value)
{
  // Convert from string or file to JSON.
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  // Convert from JSON to Protobuf.
  return protobuf::parse<mesos::DeviceWhitelist>(json.get());
}


// When the same variable is listed multiple times,
// uses only the last value.
template <>
inline Try<hashmap<std::string, std::string>> parse(const std::string& value)
{
  // Convert from string or file to JSON.
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  // Convert from JSON to Hashmap.
  hashmap<std::string, std::string> map;
  foreachpair (const std::string& key,
               const JSON::Value& value,
               json->values) {
    if (!value.is<JSON::String>()) {
      return Error(
          "The value of key '" + key + "' in '" + stringify(json.get()) + "'"
          " is not a string");
    }

    map[key] = value.as<JSON::String>().value;
  }
  return map;
}


template <>
inline Try<mesos::CapabilityInfo> parse(const std::string& value)
{
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  return protobuf::parse<mesos::CapabilityInfo>(json.get());
}


template <>
inline Try<mesos::Environment> parse(const std::string& value)
{
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  return protobuf::parse<mesos::Environment>(json.get());
}


template <>
inline Try<mesos::RLimitInfo> parse(const std::string& value)
{
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  return protobuf::parse<mesos::RLimitInfo>(json.get());
}


template <>
inline Try<mesos::DomainInfo> parse(const std::string& value)
{
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  return protobuf::parse<mesos::DomainInfo>(json.get());
}


template <>
inline Try<mesos::FrameworkID> parse(const std::string& value)
{
  mesos::FrameworkID frameworkId;
  frameworkId.set_value(value);

  return frameworkId;
}


template <>
inline Try<mesos::ExecutorID> parse(const std::string& value)
{
  mesos::ExecutorID executorId;
  executorId.set_value(value);

  return executorId;
}


template <>
inline Try<mesos::SlaveID> parse(const std::string& value)
{
  mesos::SlaveID slaveId;
  slaveId.set_value(value);

  return slaveId;
}

} // namespace flags {

#endif // __COMMON_PARSE_HPP__
