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

#ifndef __V1_PARSE_HPP__
#define __V1_PARSE_HPP__

#include <stout/error.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include <stout/flags/parse.hpp>

#include <mesos/v1/mesos.hpp>

namespace flags {

template <>
inline Try<mesos::v1::CapabilityInfo> parse(const std::string& value)
{
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  return protobuf::parse<mesos::v1::CapabilityInfo>(json.get());
}


template <>
inline Try<mesos::v1::RLimitInfo> parse(const std::string& value)
{
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  return protobuf::parse<mesos::v1::RLimitInfo>(json.get());
}


template <>
inline Try<mesos::v1::TaskGroupInfo> parse(const std::string& value)
{
  // Convert from string or file to JSON.
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  // Convert from JSON to Protobuf.
  return protobuf::parse<mesos::v1::TaskGroupInfo>(json.get());
}


template <>
inline Try<mesos::v1::TaskInfo> parse(const std::string& value)
{
  // Convert from string or file to JSON.
  Try<JSON::Object> json = parse<JSON::Object>(value);
  if (json.isError()) {
    return Error(json.error());
  }

  // Convert from JSON to Protobuf.
  return protobuf::parse<mesos::v1::TaskInfo>(json.get());
}

} // namespace flags {

#endif // __V1_PARSE_HPP__
