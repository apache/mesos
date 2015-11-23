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

#include <mesos/authorizer/authorizer.hpp>

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
               json.get().values) {
    map[key] = stringify(value);
  }
  return map;
}

} // namespace flags {

#endif // __COMMON_PARSE_HPP__
