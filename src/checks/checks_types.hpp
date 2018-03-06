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

#ifndef __CHECKS_TYPES_HPP__
#define __CHECKS_TYPES_HPP__

#include <cstdint>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <stout/option.hpp>

namespace mesos {
namespace internal {
namespace checks {
namespace check {

// Use '127.0.0.1' and '::1' instead of 'localhost', because the
// host file in some container images may not contain 'localhost'.
constexpr char DEFAULT_IPV4_DOMAIN[] = "127.0.0.1";
constexpr char DEFAULT_IPV6_DOMAIN[] = "::1";

constexpr char DEFAULT_HTTP_SCHEME[] = "http";

// The nested command check requires the entire command info struct
// so this struct is just a wrapper around `CommandInfo`.
struct Command {
  explicit Command(const CommandInfo& _info) : info(_info) {}

  CommandInfo info;
};

struct Http {
  explicit Http(
      uint32_t _port,
      const std::string& _path,
      const std::string& _scheme = DEFAULT_HTTP_SCHEME,
      bool ipv6 = false)
    : port(_port),
      path(_path),
      scheme(_scheme),
      domain(ipv6 ? "[" + std::string(DEFAULT_IPV6_DOMAIN) + "]"
                  : DEFAULT_IPV4_DOMAIN) {}

  uint32_t port;
  std::string path;
  std::string scheme;
  std::string domain;
};

struct Tcp {
  explicit Tcp(
      uint32_t _port,
      const std::string& _launcherDir,
      bool ipv6 = false)
    : port(_port),
      launcherDir(_launcherDir),
      domain(ipv6 ? DEFAULT_IPV6_DOMAIN : DEFAULT_IPV4_DOMAIN) {}

  uint32_t port;
  std::string launcherDir;
  std::string domain;
};

} // namespace check {
} // namespace checks {
} // namespace internal {
} // namespace mesos {

#endif // __CHECKS_TYPES_HPP__
