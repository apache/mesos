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

#ifndef __LINUX_SECCOMP_HPP__
#define __LINUX_SECCOMP_HPP__

#include <seccomp.h>

#include <mesos/seccomp/seccomp.hpp>

#include <process/owned.hpp>

#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "linux/capabilities.hpp"

namespace mesos {
namespace internal {
namespace seccomp {

class SeccompFilter
{
public:
  // Create a Seccomp filter based on provided info.
  static Try<process::Owned<SeccompFilter>> create(
      const mesos::seccomp::ContainerSeccompProfile& profile,
      const Option<mesos::internal::capabilities::ProcessCapabilities>&
        capabilities = None());

  // Check if the provided architecture is a native architecture.
  // Returns failure for unrecognized architectures.
  static Try<bool> nativeArch(
      const mesos::seccomp::ContainerSeccompProfile::Architecture& arch);

  ~SeccompFilter();

  // Load the seccomp filter into the Linux kernel for the current process.
  Try<Nothing> load() const;

private:
  explicit SeccompFilter(const scmp_filter_ctx& _ctx) : ctx(_ctx) {}

  scmp_filter_ctx ctx;
};

} // namespace seccomp {
} // namespace internal {
} // namespace mesos {

#endif // __LINUX_SECCOMP_HPP__
