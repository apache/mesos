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

#ifndef __LINUX_CAPABILITIES_HPP__
#define __LINUX_CAPABILITIES_HPP__

#include <set>

#include <stout/flags.hpp>
#include <stout/nothing.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include <mesos/mesos.hpp>

namespace mesos {
namespace internal {
namespace capabilities {

// Superset of all capabilities. This is the set currently supported
// by linux (kernel 5.9).
enum Capability : int
{
  CHOWN                  = 0,
  DAC_OVERRIDE           = 1,
  DAC_READ_SEARCH        = 2,
  FOWNER                 = 3,
  FSETID                 = 4,
  KILL                   = 5,
  SETGID                 = 6,
  SETUID                 = 7,
  SETPCAP                = 8,
  LINUX_IMMUTABLE        = 9,
  NET_BIND_SERVICE       = 10,
  NET_BROADCAST          = 11,
  NET_ADMIN              = 12,
  NET_RAW                = 13,
  IPC_LOCK               = 14,
  IPC_OWNER              = 15,
  SYS_MODULE             = 16,
  SYS_RAWIO              = 17,
  SYS_CHROOT             = 18,
  SYS_PTRACE             = 19,
  SYS_PACCT              = 20,
  SYS_ADMIN              = 21,
  SYS_BOOT               = 22,
  SYS_NICE               = 23,
  SYS_RESOURCE           = 24,
  SYS_TIME               = 25,
  SYS_TTY_CONFIG         = 26,
  MKNOD                  = 27,
  LEASE                  = 28,
  AUDIT_WRITE            = 29,
  AUDIT_CONTROL          = 30,
  SETFCAP                = 31,
  MAC_OVERRIDE           = 32,
  MAC_ADMIN              = 33,
  SYSLOG                 = 34,
  WAKE_ALARM             = 35,
  BLOCK_SUSPEND          = 36,
  AUDIT_READ             = 37,
  PERFMON                = 38,
  BPF                    = 39,
  CHECKPOINT_RESTORE     = 40,
  MAX_CAPABILITY         = 41,
};


enum Type
{
  EFFECTIVE,
  PERMITTED,
  INHERITABLE,
  BOUNDING,
  AMBIENT,
};


/**
 * Encapsulation of capability value sets.
 */
class ProcessCapabilities
{
public:
  const std::set<Capability>& get(const Type& type) const;
  void set(const Type& type, const std::set<Capability>& capabilities);
  void add(const Type& type, const Capability& capability);
  void drop(const Type& type, const Capability& capability);

  bool operator==(const ProcessCapabilities& right) const
  {
    return right.effective == effective &&
      right.permitted == permitted &&
      right.inheritable == inheritable &&
      right.bounding == bounding &&
      right.ambient == ambient;
  }

private:
  friend std::ostream& operator<<(
      std::ostream& stream,
      const ProcessCapabilities& processCapabilities);

  std::set<Capability> effective;
  std::set<Capability> permitted;
  std::set<Capability> inheritable;
  std::set<Capability> bounding;
  std::set<Capability> ambient;
};


/**
 * Provides wrapper for the linux process capabilities interface.
 * Note: This is a class instead of an interface because it has state
 * associated with it.
 *
 * TODO(jojy): Currently we only support linux capabilities. Consider
 * refactoring the interface so that we can support a generic
 * interface which can be used for other OSes(BSD, Windows etc).
 */
class Capabilities
{
public:
  /**
   * Factory method to create Capabilities object.
   *
   * @return `Capabilities` on success;
   *          Error on failure. Failure conditions could be:
   *           - Error getting system information (e.g, version).
   *           - Unsupported linux kernel capabilities version.
   *           - Maximum capability supported by kernel exceeds the
   *             ones defined in the enum `Capabilities`.
   */
  static Try<Capabilities> create();

  /**
   * Gets capability set for the calling process.
   *
   * @return ProcessCapabilities on success.
   *         Error on failure.
   */
  Try<ProcessCapabilities> get() const;

  /**
   * Sets capabilities for the calling process.
   *
   * @param `ProcessCapabilities` to be set for the process.
   * @return Nothing on success.
   *         Error on failure.
   */
  Try<Nothing> set(const ProcessCapabilities& processCapabilities);

  /**
   * Process control interface to enforce keeping the parent process's
   * capabilities after a change in uid/gid.
   *
   * @return Nothing on success.
   *         Error on failure.
   */
  Try<Nothing> setKeepCaps();

  /**
   * Get all capabilities supported by the system.
   *
   * @return the set of supported capabilities.
   */
  std::set<Capability> getAllSupportedCapabilities();

  /**
   * Whether ambient capabilities are supported on this host. If
   * ambient capabilities are supported, the AMBIENT set will be
   * populated when getting the process capabilities and applied
   * when setting them. Otherwise the AMBIENT set will be ignored.
   */
  const bool ambientCapabilitiesSupported;

private:
  Capabilities(int _lastCap, bool _ambientSupported);

  // Maximum count of capabilities supported by the system.
  const int lastCap;
};


Capability convert(const CapabilityInfo::Capability& capability);
std::set<Capability> convert(const CapabilityInfo& capabilityInfo);
CapabilityInfo convert(const std::set<Capability>& capabilitySet);


std::ostream& operator<<(
    std::ostream& stream,
    const Capability& capability);


std::ostream& operator<<(
    std::ostream& stream,
    const Type& type);


std::ostream& operator<<(
    std::ostream& stream,
    const ProcessCapabilities& capabilities);

} // namespace capabilities {
} // namespace internal {
} // namespace mesos {

#endif // __LINUX_CAPABILITIES_HPP__
