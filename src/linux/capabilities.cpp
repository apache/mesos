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

#include <string.h>

#include <linux/capability.h>

#include <sys/prctl.h>

#include <set>
#include <string>

#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/unreachable.hpp>

#include "linux/capabilities.hpp"

// Vendor the prctl constants for ambient capabilities so that we can build
// on systems with old glibc but still work on modern kernels.
#if !defined(PR_CAP_AMBIENT)
#define PR_CAP_AMBIENT 47
#endif

#if !defined(PR_CAP_AMBIENT_IS_SET)
#define PR_CAP_AMBIENT_IS_SET 1
#endif

#if !defined(PR_CAP_AMBIENT_RAISE)
#define PR_CAP_AMBIENT_RAISE 2
#endif

#if !defined(PR_CAP_AMBIENT_CLEAR_ALL)
#define PR_CAP_AMBIENT_CLEAR_ALL 4
#endif

using std::hex;
using std::ostream;
using std::string;

// We declare two functions provided in the libcap headers here to
// prevent introduction of that build-time dependency.
extern "C" {
extern int capset(cap_user_header_t header, cap_user_data_t data);
extern int capget(cap_user_header_t header, const cap_user_data_t data);
}


namespace mesos {
namespace internal {
namespace capabilities {

constexpr char PROC_CAP_LAST_CAP[] = "/proc/sys/kernel/cap_last_cap";
constexpr int CAPABIILITY_PROTOBUF_OFFSET = 1000;


// System call payload (for linux capability version 3).
struct SyscallPayload
{
  __user_cap_header_struct head;
  __user_cap_data_struct set[_LINUX_CAPABILITY_U32S_3];

  SyscallPayload()
  {
    memset(this, 0, sizeof(SyscallPayload));
  }

  uint64_t effective()
  {
    uint64_t low = static_cast<uint64_t>(set[0].effective);
    uint64_t high = static_cast<uint64_t>(set[1].effective) << 32;
    return low | high;
  }

  uint64_t permitted()
  {
    uint64_t low = static_cast<uint64_t>(set[0].permitted);
    uint64_t high = static_cast<uint64_t>(set[1].permitted) << 32;
    return low | high;
  }

  uint64_t inheritable()
  {
    uint64_t low = static_cast<uint64_t>(set[0].inheritable);
    uint64_t high = static_cast<uint64_t>(set[1].inheritable) << 32;
    return low | high;
  }

  void setEffective(uint64_t effective)
  {
    set[0].effective = static_cast<uint32_t>(effective);
    set[1].effective = static_cast<uint32_t>(effective >> 32);
  }

  void setPermitted(uint64_t permitted)
  {
    set[0].permitted = static_cast<uint32_t>(permitted);
    set[1].permitted = static_cast<uint32_t>(permitted >> 32);
  }

  void setInheritable(uint64_t inheritable)
  {
    set[0].inheritable = static_cast<uint32_t>(inheritable);
    set[1].inheritable = static_cast<uint32_t>(inheritable >> 32);
  }
};


// Helper function to convert capability set to bitset.
static uint64_t toCapabilityBitset(const std::set<Capability>& capabilities)
{
  uint64_t result = 0;

  for (int i = 0; i < MAX_CAPABILITY; i++) {
    if (capabilities.count(static_cast<Capability>(i)) > 0) {
      result |= (1ULL << i);
    }
  }

  return result;
}


// Helper function to convert capability bitset to std::set.
static std::set<Capability> toCapabilitySet(uint64_t bitset)
{
  std::set<Capability> result;

  for (int i = 0; i < MAX_CAPABILITY; i++) {
    if ((bitset & (1ULL << i)) != 0) {
      result.insert(Capability(i));
    }
  }

  return result;
}


const std::set<Capability>& ProcessCapabilities::get(const Type& type) const
{
  switch (type) {
    case EFFECTIVE:   return effective;
    case PERMITTED:   return permitted;
    case INHERITABLE: return inheritable;
    case BOUNDING:    return bounding;
    case AMBIENT:     return ambient;
  }

  UNREACHABLE();
}


void ProcessCapabilities::set(
    const Type& type,
    const std::set<Capability>& capabilities)
{
  switch (type) {
    case EFFECTIVE:   effective = capabilities;   return;
    case PERMITTED:   permitted = capabilities;   return;
    case INHERITABLE: inheritable = capabilities; return;
    case BOUNDING:    bounding = capabilities;    return;
    case AMBIENT:     ambient = capabilities;     return;
  }

  UNREACHABLE();
}


void ProcessCapabilities::add(
    const Type& type,
    const Capability& capability)
{
  switch (type) {
    case EFFECTIVE:   effective.insert(capability);   return;
    case PERMITTED:   permitted.insert(capability);   return;
    case INHERITABLE: inheritable.insert(capability); return;
    case BOUNDING:    bounding.insert(capability);    return;
    case AMBIENT:     ambient.insert(capability);     return;
  }

  UNREACHABLE();
}


void ProcessCapabilities::drop(
    const Type& type,
    const Capability& capability)
{
  switch (type) {
    case EFFECTIVE:   effective.erase(capability);   return;
    case PERMITTED:   permitted.erase(capability);   return;
    case INHERITABLE: inheritable.erase(capability); return;
    case BOUNDING:    bounding.erase(capability);    return;
    case AMBIENT:     ambient.erase(capability);     return;
  }

  UNREACHABLE();
}


Capabilities::Capabilities(int _lastCap, bool _ambientSupported)
  : ambientCapabilitiesSupported(_ambientSupported),
    lastCap(_lastCap) {}


Try<Capabilities> Capabilities::create()
{
  // Check for compatible linux capability version.
  SyscallPayload payload;

  if (capget(&payload.head, nullptr)) {
    // If capget fails with EINVAL it still populates the version field.
    if (errno != EINVAL) {
      return ErrnoError("Failed to get linux capability version");
    }
  }

  if (payload.head.version != _LINUX_CAPABILITY_VERSION_3) {
    return Error(
        "Unsupported linux capabilities version: " +
        stringify(payload.head.version));
  }

  // Read and check the maximum capability value.
  Try<string> _lastCap = os::read(PROC_CAP_LAST_CAP);
  if (_lastCap.isError()) {
    return Error(
        "Failed to read '" + string(PROC_CAP_LAST_CAP) + "': " +
        _lastCap.error());
  }

  Try<int> lastCap = numify<int>(strings::trim(
      _lastCap.get(),
      strings::SUFFIX,
      "\n"));

  if (lastCap.isError()) {
    return Error(
        "Failed to parse system last capability value '" +
        _lastCap.get() + "': " + lastCap.error());
  }

  if (lastCap.get() >= MAX_CAPABILITY) {
    return Error(
        "System last capability value '" + stringify(lastCap.get()) +
        "' is greater than maximum supported number of capabilities '" +
        stringify(static_cast<int>(MAX_CAPABILITY)) + "'");
  }

  // Test whether the kernel supports ambinent capabilities by testing
  // for the presence arbitrary capability in the ambient set. This can
  // only fail if the prctl option is not supported.
  bool ambientSupported =
    (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CHOWN, 0, 0) != -1);

  return Capabilities(lastCap.get(), ambientSupported);
}


Try<ProcessCapabilities> Capabilities::get() const
{
  SyscallPayload payload;

  payload.head.version = _LINUX_CAPABILITY_VERSION_3;
  payload.head.pid = 0;

  if (capget(&payload.head, &payload.set[0])) {
    return ErrnoError("Failed to get capabilities");
  }

  auto getBoundingCapabilities = [this]() {
    std::set<Capability> bounding;

    // TODO(bbannier): Parse bounding set from the `CapBnd` entry in
    // `/proc/self/status`.
    for (int i = 0; i <= lastCap; i++) {
      if (prctl(PR_CAPBSET_READ, i) == 1) {
        bounding.insert(Capability(i));
      }
    }

    return bounding;
  };

  auto getAmbientCapabilities = [this]() {
    std::set<Capability> ambient;

    // TODO(jpeach): Parse the ambient set from the `CapAmb` entry in
    // `/proc/self/status`.
    for (int i = 0; i <= lastCap; i++) {
      if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, i, 0, 0) == 1) {
        ambient.insert(Capability(i));
      }
    }

    return ambient;
  };

  ProcessCapabilities capabilities;

  capabilities.set(EFFECTIVE, toCapabilitySet(payload.effective()));
  capabilities.set(PERMITTED, toCapabilitySet(payload.permitted()));
  capabilities.set(INHERITABLE, toCapabilitySet(payload.inheritable()));
  capabilities.set(BOUNDING, getBoundingCapabilities());

  if (ambientCapabilitiesSupported) {
    capabilities.set(AMBIENT, getAmbientCapabilities());
  }

  return capabilities;
}


// We do three separate operations:
//  1. Set the `bounding` capabilities for the process.
//  2. Set the `effective`, `permitted` and `inheritable` capabilities.
//  3. Clear and then set the `ambient` capabilities.
//
// TODO(jojy): Is there a way to make this atomic? Ideally, we would
// like to rollback any changes if any of the operation fails.
Try<Nothing> Capabilities::set(const ProcessCapabilities& capabilities)
{
  // If we are setting ambient capabilities, verify that they are consistent
  // so we don't fail after we have already changed our capabilities.
  if (!capabilities.get(AMBIENT).empty()) {
    const auto& ambient = capabilities.get(AMBIENT);
    const auto& permitted = capabilities.get(PERMITTED);
    const auto& inherited = capabilities.get(INHERITABLE);

    if ((ambient & permitted).size() != ambient.size()) {
      return Error("Ambient capabilities are not in the permitted set");
    }

    if ((ambient & inherited).size() != ambient.size()) {
      return Error("Ambient capabilities are not in the inheritable set");
    }
  }

  // NOTE: We can only drop capabilities in the bounding set.
  for (int i = 0; i <= lastCap; i++) {
    if (capabilities.get(BOUNDING).count(Capability(i)) > 0) {
      continue;
    }

    VLOG(1) << "Dropping capability " << Capability(i);

    if (prctl(PR_CAPBSET_DROP, i, 1) < 0) {
      return ErrnoError(
          "Failed to drop capability: "
          "PR_CAPBSET_DROP failed for the process");
    }
  }

  SyscallPayload payload;

  payload.head.version = _LINUX_CAPABILITY_VERSION_3;
  payload.head.pid = 0;

  payload.setEffective(toCapabilityBitset(capabilities.get(EFFECTIVE)));
  payload.setPermitted(toCapabilityBitset(capabilities.get(PERMITTED)));
  payload.setInheritable(toCapabilityBitset(capabilities.get(INHERITABLE)));

  if (capset(&payload.head, &payload.set[0])) {
    return ErrnoError("Failed to set capabilities");
  }

  if (ambientCapabilitiesSupported) {
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) < 0) {
      return ErrnoError("Failed to clear ambient capabilities");
    }

    foreach(auto cap, capabilities.get(AMBIENT)) {
      if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0) < 0) {
        return ErrnoError("Failed to raise capability " + stringify(cap) +
                          " to the ambient set");
      }
    }
  }

  return Nothing();
}


Try<Nothing> Capabilities::setKeepCaps()
{
  if (prctl(PR_SET_KEEPCAPS, 1) < 0) {
    return ErrnoError("Failed to set PR_SET_KEEPCAPS for the process");
  }

  return Nothing();
}


std::set<Capability> Capabilities::getAllSupportedCapabilities()
{
  std::set<Capability> result;

  for (int i = 0; i <= lastCap; i++) {
    result.insert(Capability(i));
  }

  return result;
}


Capability convert(const CapabilityInfo::Capability& capability)
{
  int value = capability - CAPABIILITY_PROTOBUF_OFFSET;

  CHECK_LE(0, value);
  CHECK_GT(MAX_CAPABILITY, value);

  return static_cast<Capability>(value);
}


std::set<Capability> convert(const CapabilityInfo& capabilityInfo)
{
  std::set<Capability> result;

  foreach (int value, capabilityInfo.capabilities()) {
    result.insert(convert(static_cast<CapabilityInfo::Capability>(value)));
  }

  return result;
}


CapabilityInfo convert(const std::set<Capability>& capabilities)
{
  CapabilityInfo capabilityInfo;

  foreach (const Capability& capability, capabilities) {
    capabilityInfo.add_capabilities(static_cast<CapabilityInfo::Capability>(
        capability + CAPABIILITY_PROTOBUF_OFFSET));
  }

  return capabilityInfo;
}


ostream& operator<<(ostream& stream, const Capability& capability)
{
  switch (capability) {
    case CHOWN:                   return stream << "CHOWN";
    case DAC_OVERRIDE:            return stream << "DAC_OVERRIDE";
    case DAC_READ_SEARCH:         return stream << "DAC_READ_SEARCH";
    case FOWNER:                  return stream << "FOWNER";
    case FSETID:                  return stream << "FSETID";
    case KILL:                    return stream << "KILL";
    case SETGID:                  return stream << "SETGID";
    case SETUID:                  return stream << "SETUID";
    case SETPCAP:                 return stream << "SETPCAP";
    case LINUX_IMMUTABLE:         return stream << "LINUX_IMMUTABLE";
    case NET_BIND_SERVICE:        return stream << "NET_BIND_SERVICE";
    case NET_BROADCAST:           return stream << "NET_BROADCAST";
    case NET_ADMIN:               return stream << "NET_ADMIN";
    case NET_RAW:                 return stream << "NET_RAW";
    case IPC_LOCK:                return stream << "IPC_LOCK";
    case IPC_OWNER:               return stream << "IPC_OWNER";
    case SYS_MODULE:              return stream << "SYS_MODULE";
    case SYS_RAWIO:               return stream << "SYS_RAWIO";
    case SYS_CHROOT:              return stream << "SYS_CHROOT";
    case SYS_PTRACE:              return stream << "SYS_PTRACE";
    case SYS_PACCT:               return stream << "SYS_PACCT";
    case SYS_ADMIN:               return stream << "SYS_ADMIN";
    case SYS_BOOT:                return stream << "SYS_BOOT";
    case SYS_NICE:                return stream << "SYS_NICE";
    case SYS_RESOURCE:            return stream << "SYS_RESOURCE";
    case SYS_TIME:                return stream << "SYS_TIME";
    case SYS_TTY_CONFIG:          return stream << "SYS_TTY_CONFIG";
    case MKNOD:                   return stream << "MKNOD";
    case LEASE:                   return stream << "LEASE";
    case AUDIT_WRITE:             return stream << "AUDIT_WRITE";
    case AUDIT_CONTROL:           return stream << "AUDIT_CONTROL";
    case SETFCAP:                 return stream << "SETFCAP";
    case MAC_OVERRIDE:            return stream << "MAC_OVERRIDE";
    case MAC_ADMIN:               return stream << "MAC_ADMIN";
    case SYSLOG:                  return stream << "SYSLOG";
    case WAKE_ALARM:              return stream << "WAKE_ALARM";
    case BLOCK_SUSPEND:           return stream << "BLOCK_SUSPEND";
    case AUDIT_READ:              return stream << "AUDIT_READ";
    case PERFMON:                 return stream << "PERFMON";
    case BPF:                     return stream << "BPF";
    case CHECKPOINT_RESTORE:      return stream << "CHECKPOINT_RESTORE";
    case MAX_CAPABILITY:          UNREACHABLE();
  }

  UNREACHABLE();
}


ostream& operator<<(ostream& stream, const Type& type)
{
  switch (type) {
    case EFFECTIVE:   return stream << "eff";
    case PERMITTED:   return stream << "perm";
    case INHERITABLE: return stream << "inh";
    case BOUNDING:    return stream << "bnd";
    case AMBIENT:     return stream << "amb";
  }

  UNREACHABLE();
}


ostream& operator<<(
    ostream& stream,
    const ProcessCapabilities& processCapabilities)
{
  return stream
    << "{"
    << EFFECTIVE   << ": " << stringify(processCapabilities.effective)   << ", "
    << PERMITTED   << ": " << stringify(processCapabilities.permitted)   << ", "
    << INHERITABLE << ": " << stringify(processCapabilities.inheritable) << ", "
    << BOUNDING    << ": " << stringify(processCapabilities.bounding)    << ", "
    << AMBIENT     << ": " << stringify(processCapabilities.ambient)
    << "}";
}

} // namespace capabilities {
} // namespace internal {
} // namespace mesos {
