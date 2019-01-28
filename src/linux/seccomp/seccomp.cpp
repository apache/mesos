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

#include <stout/set.hpp>
#include <stout/stringify.hpp>

#include "linux/seccomp/seccomp.hpp"

using std::set;
using std::string;
using std::vector;

using process::Owned;

using mesos::internal::capabilities::Capability;
using mesos::internal::capabilities::ProcessCapabilities;

using mesos::seccomp::ContainerSeccompProfile;

namespace mesos {
namespace internal {
namespace seccomp {

Try<uint32_t> resolveAction(
    const ContainerSeccompProfile::Syscall::Action& action)
{
  static const hashmap<ContainerSeccompProfile::Syscall::Action, uint32_t>
    actions(
         {{ContainerSeccompProfile::Syscall::ACT_KILL, SCMP_ACT_KILL},
          {ContainerSeccompProfile::Syscall::ACT_TRAP, SCMP_ACT_TRAP},
          {ContainerSeccompProfile::Syscall::ACT_ERRNO, SCMP_ACT_ERRNO(EPERM)},
          {ContainerSeccompProfile::Syscall::ACT_TRACE, SCMP_ACT_TRACE(EPERM)},
          {ContainerSeccompProfile::Syscall::ACT_ALLOW, SCMP_ACT_ALLOW}});

  if (!actions.contains(action)) {
    return Error("Unknown action: " + stringify(action));
  }

  return actions.get(action).get();
}


Try<uint32_t> resolveArch(
    const ContainerSeccompProfile::Architecture& arch)
{
  static const hashmap<ContainerSeccompProfile::Architecture, uint32_t>
    architectures(
        {{ContainerSeccompProfile::ARCH_X86, SCMP_ARCH_X86},
         {ContainerSeccompProfile::ARCH_X86_64, SCMP_ARCH_X86_64},
         {ContainerSeccompProfile::ARCH_X32, SCMP_ARCH_X32},
         {ContainerSeccompProfile::ARCH_ARM, SCMP_ARCH_ARM},
         {ContainerSeccompProfile::ARCH_AARCH64, SCMP_ARCH_AARCH64},
         {ContainerSeccompProfile::ARCH_MIPS, SCMP_ARCH_MIPS},
         {ContainerSeccompProfile::ARCH_MIPSEL, SCMP_ARCH_MIPSEL},
         {ContainerSeccompProfile::ARCH_MIPS64, SCMP_ARCH_MIPS64},
         {ContainerSeccompProfile::ARCH_MIPSEL64, SCMP_ARCH_MIPSEL64},
         {ContainerSeccompProfile::ARCH_MIPS64N32, SCMP_ARCH_MIPS64N32},
         {ContainerSeccompProfile::ARCH_MIPSEL64N32, SCMP_ARCH_MIPSEL64N32},
         {ContainerSeccompProfile::ARCH_PPC, SCMP_ARCH_PPC},
         {ContainerSeccompProfile::ARCH_PPC64LE, SCMP_ARCH_PPC64LE},
         {ContainerSeccompProfile::ARCH_S390, SCMP_ARCH_S390},
         {ContainerSeccompProfile::ARCH_S390X, SCMP_ARCH_S390X}});

  if (!architectures.contains(arch)) {
    return Error("Unknown architecture: " + stringify(arch));
  }

  return architectures.get(arch).get();
}


Try<scmp_compare> resolveArgCmpOperator(
    const ContainerSeccompProfile::Syscall::Arg::Operator& op)
{
  static const hashmap<
      ContainerSeccompProfile::Syscall::Arg::Operator,
      scmp_compare>
    operators({{ContainerSeccompProfile::Syscall::Arg::CMP_NE, SCMP_CMP_NE},
               {ContainerSeccompProfile::Syscall::Arg::CMP_LT, SCMP_CMP_LT},
               {ContainerSeccompProfile::Syscall::Arg::CMP_LE, SCMP_CMP_LE},
               {ContainerSeccompProfile::Syscall::Arg::CMP_EQ, SCMP_CMP_EQ},
               {ContainerSeccompProfile::Syscall::Arg::CMP_GE, SCMP_CMP_GE},
               {ContainerSeccompProfile::Syscall::Arg::CMP_GT, SCMP_CMP_GT},
               {ContainerSeccompProfile::Syscall::Arg::CMP_MASKED_EQ,
                SCMP_CMP_MASKED_EQ}});

  if (!operators.contains(op)) {
    return Error("Unknown operator: " + stringify(op));
  }

  return operators.get(op).get();
}


Try<Owned<SeccompFilter>> SeccompFilter::create(
    const ContainerSeccompProfile& profile,
    const Option<ProcessCapabilities>& capabilities)
{
  // Init a Seccomp context.
  Try<uint32_t> defaultAction = resolveAction(profile.default_action());
  if (defaultAction.isError()) {
    return Error(defaultAction.error());
  }

  scmp_filter_ctx ctx = seccomp_init(defaultAction.get());
  if (ctx == nullptr) {
    return Error("Failed to initialize Seccomp filter");
  }

  Owned<SeccompFilter> filter(new SeccompFilter(ctx));

  // To install a Seccomp filter, one of the following must be met:
  //   1. The caller is privileged (CAP_SYS_ADMIN).
  //   2. The caller has to set no_new_privs process attribute (There is
  //      actually a filter attr to help to set this: SCMP_FLTATR_CTL_NNP).
  //
  // In Docker, this attr is explicitly turned off, assuming
  // the Docker daemon has CAP_SYS_ADMIN (privileged Docker daemon).
  // We turn off this bit as well; otherwise, a container process that
  // runs as an unprivileged user could not launch binaries with
  // the set-user-ID and set-group-ID bits (e.g., `ping` and `sudo`).
  // See https://www.kernel.org/doc/Documentation/prctl/no_new_privs.txt
  int ret = seccomp_attr_set(ctx, SCMP_FLTATR_CTL_NNP, 0);
  if (ret < 0) {
    return ErrnoError(-ret, "Failed to set no_new_privs bit");
  }

  // Add architectures.
  foreach (int arch, profile.architectures()) {
    const string& archName = ContainerSeccompProfile::Architecture_Name(
        static_cast<ContainerSeccompProfile::Architecture>(arch));

    Try<uint32_t> archToken =
      resolveArch(static_cast<ContainerSeccompProfile::Architecture>(arch));

    if (archToken.isError()) {
      return Error(archToken.error());
    }

    ret = seccomp_arch_add(ctx, archToken.get());
    if (ret != 0) {
      if (ret == -EEXIST) {
        // Libseccomp returns -EEXIST if requested arch is already persent.
        // However we simply ignore this since it's not fatal.
        VLOG(2) << "Seccomp architecture '" << archName
                << "' is already present, skipping it";

        continue;
      } else {
        return ErrnoError(
            -ret, "Failed to add Seccomp architecture '" + archName + "'");
      }
    }

    VLOG(2) << "Added Seccomp architecture '" << archName << "'";
  }

  // Add rules.
  foreach (
      const ContainerSeccompProfile::Syscall& syscall, profile.syscalls()) {
    if (capabilities.isSome()) {
      // Filter out the rule by Linux capabilities. We only add the rule if
      // the process's capabilities has all the `includesCaps` and has none of
      // the `excludesCaps`.
      const auto& boundingCaps = capabilities->get(capabilities::BOUNDING);

      if (syscall.has_includes() &&
          syscall.includes().capabilities_size() > 0) {
        set<Capability> includesCaps;
        foreach (int capability, syscall.includes().capabilities()) {
          includesCaps.emplace(capabilities::convert(
              static_cast<CapabilityInfo::Capability>(capability)));
        }

        const auto unionCaps = boundingCaps | includesCaps;

        // Skip the rule if `includesCaps` is not a subset of `boundingCaps`.
        if (unionCaps.size() > boundingCaps.size()) {
          continue;
        }
      }

      if (syscall.has_excludes() &&
          syscall.excludes().capabilities_size() > 0) {
        set<Capability> excludesCaps;
        foreach (int capability, syscall.excludes().capabilities()) {
          excludesCaps.emplace(capabilities::convert(
              static_cast<CapabilityInfo::Capability>(capability)));
        }

        const auto intersectCaps = boundingCaps & excludesCaps;

        // Skip the rule if `excludesCaps` contains elements of `boundingCaps`.
        if (!intersectCaps.empty()) {
          continue;
        }
      }
    }

    Try<uint32_t> action = resolveAction(syscall.action());
    if (action.isError()) {
      return Error(action.error());
    }

    const string& actionName =
      ContainerSeccompProfile::Syscall::Action_Name(syscall.action());

    vector<scmp_arg_cmp> args;
    if (syscall.args_size() > 0) {
      // There are arguments associated with this syscall rule.
      foreach (
          const ContainerSeccompProfile::Syscall::Arg& arg, syscall.args()) {
        Try<scmp_compare> cmpOperator = resolveArgCmpOperator(arg.op());
        if (cmpOperator.isError()) {
          return Error(cmpOperator.error());
        }

        const scmp_arg_cmp argCmp = {
          arg.index(),
          cmpOperator.get(),
          arg.value(),
          arg.value_two()
        };

        args.push_back(argCmp);
      }
    }

    foreach (const string& name, syscall.names()) {
      int syscallNumber = seccomp_syscall_resolve_name(name.c_str());
      if (syscallNumber == __NR_SCMP_ERROR) {
        return Error("Unrecognized syscall '" + name + "'");
      }

      if (syscall.args_size() > 0) {
        ret = seccomp_rule_add_array(
            ctx,
            action.get(),
            syscallNumber,
            syscall.args_size(),
            args.data());
      } else {
        // There are no arguments associated with this syscall rule.
        ret = seccomp_rule_add(ctx, action.get(), syscallNumber, 0);
      }

      if (ret < 0) {
        return ErrnoError(
            -ret,
            "Failed to add rule for syscall '" + name +
            "' with action '" + actionName + "'");
      }

      VLOG(2) << "Added Seccomp rule for syscall '" << name
              << "' with action '" << actionName << "'";
    }
  }

  return filter;
}


Try<bool> SeccompFilter::nativeArch(
    const ContainerSeccompProfile::Architecture& arch)
{
  const auto nativeArch = resolveArch(arch);
  if (nativeArch.isError()) {
    return Error(nativeArch.error());
  }

  return seccomp_arch_native() == nativeArch.get();
}


SeccompFilter::~SeccompFilter()
{
  seccomp_release(ctx);
}


Try<Nothing> SeccompFilter::load() const
{
  int ret = seccomp_load(ctx);
  if (ret < 0) {
    return ErrnoError(-ret, "Failed to load Seccomp filter");
  }

  return Nothing();
}

} // namespace seccomp {
} // namespace internal {
} // namespace mesos {
