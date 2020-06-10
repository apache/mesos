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

#include <string.h> // For strlen().

#include <stout/json.hpp>
#include <stout/nothing.hpp>
#include <stout/strings.hpp>

#include <stout/os/read.hpp>

#include "linux/seccomp/seccomp.hpp"
#include "linux/seccomp/seccomp_parser.hpp"

using std::string;

using process::Owned;

using mesos::internal::seccomp::SeccompFilter;

using mesos::seccomp::ContainerSeccompProfile;

namespace mesos {
namespace internal {
namespace seccomp {

constexpr char SCMP_PREFIX[] = "SCMP_";
constexpr char CAP_PREFIX[] = "CAP_";


Try<ContainerSeccompProfile::Syscall::Action> parseSyscallAction(
    const string& value)
{
  ContainerSeccompProfile::Syscall::Action result;

  if (!strings::startsWith(value, SCMP_PREFIX)) {
    return Error("Unexpected syscall action: '" + value + "'");
  }

  if (!ContainerSeccompProfile::Syscall::Action_Parse(
          string(value, strlen(SCMP_PREFIX)), &result)) {
    return Error("Unknown syscall action: '" + value + "'");
  }

  return std::move(result);
}


Try<ContainerSeccompProfile::Architecture> parseArchitecture(
    const string& value)
{
  ContainerSeccompProfile::Architecture result;

  if (!strings::startsWith(value, SCMP_PREFIX)) {
    return Error("Unexpected architecture: '" + value + "'");
  }

  if (!ContainerSeccompProfile::Architecture_Parse(
          string(value, strlen(SCMP_PREFIX)), &result)) {
    return Error("Unknown architecture: '" + value + "'");
  }

  return std::move(result);
}


Try<Nothing> parseDefaultAction(
    const JSON::Object& json,
    ContainerSeccompProfile* profile)
{
  Result<JSON::String> defaultAction = json.at<JSON::String>("defaultAction");
  if (!defaultAction.isSome()) {
    return Error(
        "Cannot determine 'defaultAction' for the Seccomp configuration: " +
        (defaultAction.isError() ? defaultAction.error() : "Not found"));
  }

  const auto defaultActionValue = parseSyscallAction(defaultAction->value);
  if (defaultActionValue.isError()) {
    return Error(defaultActionValue.error());
  }

  profile->set_default_action(defaultActionValue.get());

  return Nothing();
}


Try<Nothing> parseArchMap(
    const JSON::Object& json,
    ContainerSeccompProfile* profile)
{
  Result<JSON::Array> archMap = json.at<JSON::Array>("archMap");
  if (!archMap.isSome()) {
    return Error(
        "Cannot determine 'archMap' for the Seccomp configuration: " +
        (archMap.isError() ? archMap.error() : "Not found"));
  }

  foreach (const JSON::Value& item, archMap->values) {
    if (!item.is<JSON::Object>()) {
      return Error("'archMap' contains a non-object item");
    }

    const auto architecture =
      item.as<JSON::Object>().at<JSON::String>("architecture");

    if (!architecture.isSome()) {
      return Error(
          "Cannot determine 'architecture' field for 'archMap' item: " +
          (architecture.isError() ? architecture.error() : "Not found"));
    }

    const auto arch = parseArchitecture(architecture->value);
    if (arch.isError()) {
      return Error(arch.error());
    }

    Try<bool> nativeArch = SeccompFilter::nativeArch(arch.get());
    if (nativeArch.isError()) {
      return Error(nativeArch.error());
    }

    // We ignore architectures that doesn't match the native architecture of
    // this host.
    if (!nativeArch.get()) {
      continue;
    }

    // Add this architecture with corresponding subarchitectures to the list of
    // architectures of the profile.
    const auto subArchitectures =
      item.as<JSON::Object>().at<JSON::Array>("subArchitectures");

    if (!subArchitectures.isSome()) {
      return Error(
          "Cannot determine 'subArchitectures' field for 'archMap' item: " +
          (subArchitectures.isError() ? subArchitectures.error()
                                      : "Not found"));
    }

    foreach (const JSON::Value& subItem, subArchitectures->values) {
      if (!subItem.is<JSON::String>()) {
        return Error("'subArchitectures' contains a non-string item");
      }

      const auto subArch = parseArchitecture(subItem.as<JSON::String>().value);
      if (subArch.isError()) {
        return Error(subArch.error());
      }

      profile->mutable_architectures()->Add(subArch.get());
    }

    profile->mutable_architectures()->Add(arch.get());
  }

  return Nothing();
}


Try<ContainerSeccompProfile::Syscall::Filter> parseSyscallFilter(
    const JSON::Object& json,
    bool* architectureMatched)
{
  ContainerSeccompProfile::Syscall::Filter result;

  // Parse `arches` section which defines filtering rules by CPU architecture.
  const auto arches = json.at<JSON::Array>("arches");
  if (arches.isError()) {
    return Error(arches.error());
  }

  if (arches.isSome()) {
    static const hashmap<string, ContainerSeccompProfile::Architecture>
      architectures({{"x86", ContainerSeccompProfile::ARCH_X86},
                     {"amd64", ContainerSeccompProfile::ARCH_X86_64},
                     {"x32", ContainerSeccompProfile::ARCH_X32},
                     {"arm", ContainerSeccompProfile::ARCH_ARM},
                     {"arm64", ContainerSeccompProfile::ARCH_AARCH64},
                     {"mips", ContainerSeccompProfile::ARCH_MIPS},
                     {"mipsel", ContainerSeccompProfile::ARCH_MIPSEL},
                     {"mips64", ContainerSeccompProfile::ARCH_MIPS64},
                     {"mipsel64", ContainerSeccompProfile::ARCH_MIPSEL64},
                     {"mips64n32", ContainerSeccompProfile::ARCH_MIPS64N32},
                     {"mipsel64n32", ContainerSeccompProfile::ARCH_MIPSEL64N32},
                     {"ppc", ContainerSeccompProfile::ARCH_PPC},
                     {"ppc64le", ContainerSeccompProfile::ARCH_PPC64LE},
                     {"s390", ContainerSeccompProfile::ARCH_S390},
                     {"s390x", ContainerSeccompProfile::ARCH_S390X}});

    foreach (const JSON::Value& item, arches->values) {
      if (!item.is<JSON::String>()) {
        return Error("'arches' contains non-string item");
      }

      const auto arch = item.as<JSON::String>().value;
      if (!architectures.contains(arch)) {
        return Error("Unknown architecture: '" + arch + "'");
      }

      Try<bool> nativeArch =
        SeccompFilter::nativeArch(architectures.get(arch).get());

      if (nativeArch.isError()) {
        return Error(nativeArch.error());
      }

      *architectureMatched = nativeArch.get();
      if (*architectureMatched) {
        break;
      }
    }
  }

  // Parse `caps` section which defines filtering rules by Linux capabilities.
  const auto caps = json.at<JSON::Array>("caps");
  if (caps.isError()) {
    return Error(caps.error());
  }

  if (caps.isSome()) {
    foreach (const JSON::Value& item, caps->values) {
      if (!item.is<JSON::String>()) {
        return Error("'caps' contains non-string item");
      }

      const auto cap = item.as<JSON::String>().value;

      if (!strings::startsWith(cap, CAP_PREFIX)) {
        return Error("Unexpected capability: '" + cap + "'");
      }

      CapabilityInfo::Capability capability;

      if (!CapabilityInfo::Capability_Parse(
              string(cap, strlen(CAP_PREFIX)), &capability)) {
        return Error("Unknown capability: '" + cap + "'");
      }

      result.mutable_capabilities()->Add(capability);
    }
  }

  return std::move(result);
}


Try<Nothing> parseSyscallArgument(
  const JSON::Object& json,
  ContainerSeccompProfile::Syscall::Arg* arg)
{
  auto parseField = [&json](const string& field) -> Try<JSON::Number> {
    const auto number = json.at<JSON::Number>(field);
    if (!number.isSome()) {
      return Error(
          "Cannot determine '" + field + "' field for 'args' item: " +
          (number.isError() ? number.error() : "Not found"));
    }

    return number.get();
  };

  const auto index = parseField("index");
  if (index.isError()) {
    return Error(index.error());
  }

  arg->set_index(index->as<uint32_t>());

  const auto value = parseField("value");
  if (value.isError()) {
    return Error(value.error());
  }

  arg->set_value(value->as<uint64_t>());

  const auto valueTwo = parseField("valueTwo");
  if (valueTwo.isError()) {
    return Error(valueTwo.error());
  }

  arg->set_value_two(valueTwo->as<uint64_t>());

  const auto op = json.at<JSON::String>("op");
  if (!op.isSome()) {
    return Error(
        "Cannot determine 'op' field for 'args' item: " +
        (op.isError() ? op.error() : "Not found"));
  }

  if (!strings::startsWith(op->value, SCMP_PREFIX)) {
    return Error("Unexpected operation: '" + op->value + "'");
  }

  ContainerSeccompProfile::Syscall::Arg::Operator opVal;

  if (!ContainerSeccompProfile::Syscall::Arg::Operator_Parse(
          string(op->value, strlen(SCMP_PREFIX)), &opVal)) {
    return Error("Unknown operator: '" + op->value + "'");
  }

  arg->set_op(opVal);

  return Nothing();
}


Try<Nothing> parseSyscalls(
  const JSON::Object& json,
  ContainerSeccompProfile* profile)
{
  Result<JSON::Array> syscalls = json.at<JSON::Array>("syscalls");
  if (!syscalls.isSome()) {
    return Error(
        "Cannot determine 'syscalls' for the Seccomp configuration: " +
        (syscalls.isError() ? syscalls.error() : "Not found"));
  }

  // Each item in `syscalls` section defines a seccomp filter for a subset
  // of system calls.
  foreach (const JSON::Value& item, syscalls->values) {
    if (!item.is<JSON::Object>()) {
      return Error("'syscalls' contains a non-object item");
    }

    ContainerSeccompProfile::Syscall syscall;

    // Both `includes` and `excludes` sections define rules for filtering out
    // this seccomp filter. We omit this seccomp rule in two cases:
    //   1. `excludes` rule is matched.
    //   2. `includes` rule is not matched.
    // Currently, we support filtering by Linux capabilities and by CPU
    // architecture. We do filtering by CPU architecture here, while we postpone
    // filtering by Linux capabilities until starting a container via the Linux
    // launcher. We can't filter by Linux capabilities here, because the list of
    // capabilities is unknown at the moment the `linux/seccomp` isolator parses
    // the seccomp profile.

    // Parse `includes` section.
    const auto includes = item.as<JSON::Object>().at<JSON::Object>("includes");
    if (!includes.isSome()) {
      return Error(
          "Cannot determine 'includes' field for 'syscalls' item: " +
          (includes.isError() ? includes.error() : "Not found"));
    }

    bool architectureMatched = false;
    const auto includesFilter =
      parseSyscallFilter(includes.get(), &architectureMatched);

    if (includesFilter.isError()) {
      return Error(includesFilter.error());
    }

    if (includes->values.count("arches") && !architectureMatched) {
      continue;
    }

    if (includes->values.count("caps")) {
      syscall.mutable_includes()->CopyFrom(includesFilter.get());
    }

    // Parse `excludes` section.
    const auto excludes = item.as<JSON::Object>().at<JSON::Object>("excludes");
    if (!excludes.isSome()) {
      return Error(
          "Cannot determine 'excludes' field for 'syscalls' item: " +
          (excludes.isError() ? excludes.error() : "Not found"));
    }

    architectureMatched = false;
    const auto excludesFilter =
      parseSyscallFilter(excludes.get(), &architectureMatched);

    if (excludesFilter.isError()) {
      return Error(excludesFilter.error());
    }

    if (excludes->values.count("arches") && architectureMatched) {
      continue;
    }

    if (excludes->values.count("caps")) {
      syscall.mutable_excludes()->CopyFrom(excludesFilter.get());
    }

    // Parse `names` section which contains a list of syscalls.
    const auto names = item.as<JSON::Object>().at<JSON::Array>("names");
    if (!names.isSome()) {
      return Error(
          "Cannot determine 'names' field for 'syscalls' item: " +
          (names.isError() ? names.error() : "Not found"));
    }

    foreach (const JSON::Value& namesItem, names->values) {
      if (!namesItem.is<JSON::String>()) {
        return Error("'names' contains a non-string item");
      }

      syscall.add_names(namesItem.as<JSON::String>().value);
    }

    if (syscall.names().empty()) {
      return Error("Syscall 'names' list is empty");
    }

    // Parse `action` section.
    const auto action = item.as<JSON::Object>().at<JSON::String>("action");
    if (!action.isSome()) {
      return Error(
          "Cannot determine 'action' for 'syscalls' item: " +
          (action.isError() ? action.error() : "Not found"));
    }

    const auto actionValue = parseSyscallAction(action->value);
    if (actionValue.isError()) {
      return Error(actionValue.error());
    }

    syscall.set_action(actionValue.get());

    // Parse `args` section which contains seccomp filtering rules for syscall
    // arguments.
    const auto args = item.as<JSON::Object>().find<JSON::Value>("args");
    if (!args.isSome()) {
      return Error(
          "Cannot determine 'args' field for 'syscalls' item: " +
          (args.isError() ? args.error() : "Not found"));
    }

    // `args` can be either `null` or an array.
    if (args->is<JSON::Array>()) {
      foreach (const JSON::Value& argsItem, args->as<JSON::Array>().values) {
        if (!argsItem.is<JSON::Object>()) {
          return Error("'args' contains a non-object item");
        }

        Try<Nothing> arg =
          parseSyscallArgument(argsItem.as<JSON::Object>(), syscall.add_args());

        if (arg.isError()) {
          return Error(arg.error());
        }
      }
    }

    profile->add_syscalls()->CopyFrom(syscall);
  }

  return Nothing();
}


Try<ContainerSeccompProfile> parseProfileData(const string& data)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(data);
  if (json.isError()) {
    return Error(json.error());
  }

  ContainerSeccompProfile profile;

  Try<Nothing> defaultAction = parseDefaultAction(json.get(), &profile);
  if (defaultAction.isError()) {
    return Error(defaultAction.error());
  }

  Try<Nothing> archMap = parseArchMap(json.get(), &profile);
  if (archMap.isError()) {
    return Error(archMap.error());
  }

  Try<Nothing> syscalls = parseSyscalls(json.get(), &profile);
  if (syscalls.isError()) {
    return Error(syscalls.error());
  }

  // Verify Seccomp profile.
  Try<Owned<SeccompFilter>> seccompFilter = SeccompFilter::create(profile);
  if (seccompFilter.isError()) {
    return Error(seccompFilter.error());
  }

  return std::move(profile);
}


Try<ContainerSeccompProfile> parseProfile(const string& path)
{
  Try<string> read = os::read(path);
  if (read.isError()) {
    return Error(
        "Failed to read Seccomp profile file '" + path + "': " + read.error());
  }

  Try<ContainerSeccompProfile> profile = parseProfileData(read.get());
  if (profile.isError()) {
    return Error(
        "Failed to parse Seccomp profile '" + path + "': " + profile.error());
  }

  return profile;
}

} // namespace seccomp {
} // namespace internal {
} // namespace mesos {
