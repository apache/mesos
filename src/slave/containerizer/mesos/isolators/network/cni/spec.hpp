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

#ifndef __ISOLATOR_CNI_SPEC_HPP__
#define __ISOLATOR_CNI_SPEC_HPP__

#include <stout/try.hpp>
#include <stout/json.hpp>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include "slave/containerizer/mesos/isolators/network/cni/spec.pb.h"

namespace mesos {
namespace internal {
namespace slave {
namespace cni {
namespace spec {

constexpr char CNI_VERSION[] = "0.3.0";


// CNI commands.
constexpr char CNI_CMD_ADD[] = "ADD";
constexpr char CNI_CMD_DEL[] = "DEL";


// Well-known CNI error codes:
// https://github.com/containernetworking/cni/blob/master/SPEC.md#well-known-error-codes
constexpr uint32_t CNI_ERROR_INCOMPATIBLE_VERSION = 1;
constexpr uint32_t CNI_ERROR_UNSUPPORTED_FIELD = 2;


Try<NetworkConfig> parseNetworkConfig(const std::string& s);
Try<NetworkInfo> parseNetworkInfo(const std::string& s);


// Takes a DNS specification and returns a string containing
// the equivalent resolv.conf(5) format.
std::string formatResolverConfig(const DNS& dns);


// Creates a `spec::Error` object, with the `msg` and `code`
// specified. Returns a JSON string representation of the
// `spec::Error` object. See details in:
// https://github.com/containernetworking/cni/blob/master/SPEC.md#result
std::string error(const std::string& msg, uint32_t code);


// This class encapsulates a JSON formatted string of type
// `spec::Error`. It can be used by CNI plugins to return a CNI error
// in a `Try` object when a failure occurs.
class PluginError : public ::Error
{
public:
  PluginError(const std::string& msg, uint32_t code)
    : ::Error(error(msg, code)) {}
};


inline std::ostream& operator<<(std::ostream& stream, const PluginError& _error)
{
  return stream << _error.message;
}

} // namespace spec {
} // namespace cni {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __ISOLATOR_CNI_SPEC_HPP__
