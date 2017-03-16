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

#include <stout/json.hpp>
#include <stout/protobuf.hpp>

#include "slave/containerizer/mesos/isolators/network/cni/spec.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace cni {
namespace spec {

Try<NetworkConfig> parseNetworkConfig(const string& s)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(s);
  if (json.isError()) {
    return ::Error("JSON parse failed: " + json.error());
  }

  Try<NetworkConfig> parse = ::protobuf::parse<NetworkConfig>(json.get());
  if (parse.isError()) {
    return ::Error("Protobuf parse failed: " + parse.error());
  }

  return parse.get();
}


Try<NetworkInfo> parseNetworkInfo(const string& s)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(s);
  if (json.isError()) {
    return ::Error("JSON parse failed: " + json.error());
  }

  Try<NetworkInfo> parse = ::protobuf::parse<NetworkInfo>(json.get());
  if (parse.isError()) {
    return ::Error("Protobuf parse failed: " + parse.error());
  }

  return parse.get();
}


string formatResolverConfig(const DNS& dns)
{
  std::stringstream resolv;

  if (dns.has_domain()) {
    resolv << "domain " << dns.domain() << std::endl;
  }

  if (!dns.search().empty()) {
    resolv << "search";
    foreach (const string& domain, dns.search()) {
      resolv << " " << domain;
    }
    resolv << std::endl;
  }

  if (!dns.options().empty()) {
    resolv << "options";
    foreach (const string& opt, dns.options()) {
      resolv << " " << opt;
    }
    resolv << std::endl;
  }

  if (!dns.nameservers().empty()) {
    foreach (const string& nameserver, dns.nameservers()) {
      resolv << "nameserver " << nameserver << std::endl;
    }
  }

  return resolv.str();
}


string error(const string& msg, uint32_t code)
{
  spec::Error error;
  error.set_cniversion(CNI_VERSION);
  error.set_code(code);
  error.set_msg(msg);

  return stringify(JSON::protobuf(error));
}

} // namespace spec {
} // namespace cni {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
