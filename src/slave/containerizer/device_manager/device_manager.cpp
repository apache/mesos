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

#include <algorithm>
#include <string>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/os/exists.hpp>
#include <stout/stringify.hpp>
#include <stout/unreachable.hpp>

#include "slave/containerizer/device_manager/device_manager.hpp"
#include "slave/containerizer/device_manager/state.hpp"
#include "slave/containerizer/mesos/paths.hpp"
#include "slave/paths.hpp"
#include "slave/state.hpp"
#include "linux/cgroups2.hpp"

using google::protobuf::RepeatedPtrField;

using std::string;
using std::vector;

using process::dispatch;
using process::Failure;
using process::Future;
using process::Owned;

using cgroups::devices::Entry;

using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {


Entry convert_to_entry(
    const DeviceManager::NonWildcardEntry& non_wildcard_entry)
{
  Entry entry;
  entry.access = non_wildcard_entry.access;
  entry.selector.type = [&]() {
    switch (non_wildcard_entry.selector.type) {
      case DeviceManager::NonWildcardEntry::Selector::Type::BLOCK:
        return Entry::Selector::Type::BLOCK;
      case DeviceManager::NonWildcardEntry::Selector::Type::CHARACTER:
        return Entry::Selector::Type::CHARACTER;
    }
    UNREACHABLE();
  }();
  entry.selector.major = non_wildcard_entry.selector.major;
  entry.selector.minor = non_wildcard_entry.selector.minor;
  return entry;
}


vector<Entry> convert_to_entries(
    const vector<DeviceManager::NonWildcardEntry>& non_wildcard_entries)
{
  vector<Entry> entries = {};
  foreach (const DeviceManager::NonWildcardEntry& non_wildcard,
           non_wildcard_entries) {
    entries.push_back(convert_to_entry(non_wildcard));
  }
  return entries;
}


Try<vector<DeviceManager::NonWildcardEntry>>
  DeviceManager::NonWildcardEntry::create(
      const vector<cgroups::devices::Entry>& entries)
{
  vector<DeviceManager::NonWildcardEntry> non_wildcards = {};

  foreach (const cgroups::devices::Entry& entry, entries) {
    if (entry.selector.has_wildcard()) {
      return Error("Entry cannot have wildcard");
    }
    DeviceManager::NonWildcardEntry non_wildcard;
    non_wildcard.access = entry.access;
    non_wildcard.selector.major = *entry.selector.major;
    non_wildcard.selector.minor = *entry.selector.minor;
    non_wildcard.selector.type = [&]() {
      switch (entry.selector.type) {
        case cgroups::devices::Entry::Selector::Type::BLOCK:
          return DeviceManager::NonWildcardEntry::Selector::Type::BLOCK;
        case cgroups::devices::Entry::Selector::Type::CHARACTER:
          return DeviceManager::NonWildcardEntry::Selector::Type::CHARACTER;
        case cgroups::devices::Entry::Selector::Type::ALL:
          UNREACHABLE();
      }
      UNREACHABLE();
    }();
    non_wildcards.push_back(non_wildcard);
  }

  return non_wildcards;
}


class DeviceManagerProcess : public process::Process<DeviceManagerProcess>
{
public:
  DeviceManagerProcess(const Flags& flags)
    : ProcessBase(process::ID::generate("device-manager")),
      meta_dir(paths::getMetaRootDir(flags.work_dir)),
      cgroups_root(flags.cgroups_root) {}

  Future<Nothing> configure(
      const string& cgroup,
      const vector<Entry>& allow_list,
      const vector<DeviceManager::NonWildcardEntry>& non_wildcard_deny_list)
  {
    vector<Entry> deny_list = convert_to_entries(non_wildcard_deny_list);

    if (!cgroups2::devices::normalized(allow_list)
        || !cgroups2::devices::normalized(deny_list)) {
      return Failure("Failed to configure allow and deny devices:"
                     " the input allow or deny list is not normalized");
    }

    foreach (const Entry& allow_entry, allow_list) {
      foreach (const Entry& deny_entry, deny_list) {
        if (deny_entry.encompasses(allow_entry)) {
          return Failure(
              "Failed to configure allow and deny devices:"
              " allow entry '" + stringify(allow_entry) + "' cannot be"
              " encompassed by deny entry '" + stringify(deny_entry) + "'");
        }
      }
    }

    auto result = device_access_per_cgroup.emplace(
        cgroup,
        CHECK_NOTERROR(
          DeviceManager::CgroupDeviceAccess::create(allow_list, deny_list)));
    if (!result.second) {
      return Failure("cgroup entry already exists");
    }

    Try<Nothing> commit = commit_device_access_changes(cgroup);
    if (commit.isError()) {
      // We do not rollback the state when something goes wrong in the
      // update because the container will be destroyed when this fails.
      return Failure("Failed to commit cgroup device access changes: "
                     + commit.error());
    }

    return Nothing();
  }

  Future<Nothing> reconfigure(
      const string& cgroup,
      const vector<DeviceManager::NonWildcardEntry>& non_wildcard_additions,
      const vector<DeviceManager::NonWildcardEntry>& non_wildcard_removals)
  {
    vector<Entry> additions = convert_to_entries(non_wildcard_additions);
    vector<Entry> removals = convert_to_entries(non_wildcard_removals);
    foreach (const Entry& addition, additions) {
      foreach (const Entry& removal, removals) {
        if (removal.encompasses(addition)) {
          return Failure(
              "Failed to configure allow and deny devices:"
              " addition '" + stringify(addition) + "' cannot be"
              " encompassed by removal '" + stringify(removal) + "'");
        }
      }
    }

    auto it = device_access_per_cgroup.find(cgroup);
    if (it != device_access_per_cgroup.end()) {
      it->second = DeviceManager::apply_diff(
          it->second, non_wildcard_additions, non_wildcard_removals);
    } else {
      auto result = device_access_per_cgroup.emplace(
        cgroup,
        DeviceManager::apply_diff(
          CHECK_NOTERROR(DeviceManager::CgroupDeviceAccess::create({}, {})),
          non_wildcard_additions,
          non_wildcard_removals));
      CHECK(result.second);
    }

    Try<Nothing> commit = commit_device_access_changes(cgroup);
    if (commit.isError()) {
      // We do not rollback the state when something goes wrong in the
      // update because the container will be destroyed when this fails.
      return Failure("Failed to commit cgroup device access changes: "
                     + commit.error());
    }

    return Nothing();
  }

  hashmap<string, DeviceManager::CgroupDeviceAccess> state() const
  {
    return device_access_per_cgroup;
  }

  DeviceManager::CgroupDeviceAccess state(const string& cgroup) const
  {
    return device_access_per_cgroup.contains(cgroup)
      ? device_access_per_cgroup.at(cgroup)
      : CHECK_NOTERROR(DeviceManager::CgroupDeviceAccess::create({}, {}));
  }

  Future<Nothing> remove(const std::string& cgroup)
  {
    if (device_access_per_cgroup.contains(cgroup)) {
      device_access_per_cgroup.erase(cgroup);
    }
    return Nothing();
  }

  Future<Nothing> recover(const vector<ContainerState>& states)
  {
    hashset<string> cgroups_to_recover;
    foreach(const ContainerState& state, states) {
      cgroups_to_recover.insert(containerizer::paths::cgroups2::container(
          cgroups_root, state.container_id(), false));
    }

    const string checkpoint_path = paths::getDevicesStatePath(meta_dir);
    if (!os::exists(checkpoint_path)) {
      return Nothing(); // This happens on the first run.
    }

    Result<CgroupDeviceAccessStates> device_states =
      state::read<CgroupDeviceAccessStates>(checkpoint_path);

    if (device_states.isError()) {
      return Failure("Failed to read device configuration info from"
                     " '" + checkpoint_path + "': " + device_states.error());
    } else if (device_states.isNone()) {
      LOG(WARNING) << "The device info file at '" << checkpoint_path << "'"
                   << " is empty";
      return Nothing();
    }
    CHECK_SOME(device_states);

    vector<string> recovered_cgroups = {};
    foreach (const auto& entry, device_states->device_access_per_cgroup()) {
      const string& cgroup = entry.first;
      const CgroupDeviceAccessState& state = entry.second;

      if (!cgroups_to_recover.contains(cgroup)) {
        LOG(WARNING)
          << "The cgroup '" << cgroup << "' from the device manager's"
             " checkpointed state is not present in the expected cgroups of"
             " the containerizer";
        continue;
      }

      auto parse = [&](const RepeatedPtrField<string>& list)
          -> Try<vector<Entry>>
      {
        vector<Entry> parsed_entries;
        foreach (const string& entry, list) {
          Try<Entry> parsed_entry = Entry::parse(entry);
          if (parsed_entry.isError()) {
            return Error("Failed to parse entry " + entry + " during recover"
                         " for cgroup " + cgroup + ": " + parsed_entry.error());
          }
          parsed_entries.push_back(*parsed_entry);
        }
        return parsed_entries;
      };

      // We return failure because we expect all data in the checkpoint file
      // to be valid.
      Try<vector<Entry>> allow_entries = parse(state.allow_list());
      if (allow_entries.isError()) {
        return Failure(allow_entries.error());
      }

      Try<vector<Entry>> deny_entries = parse(state.deny_list());
      if (deny_entries.isError()) {
        return Failure(deny_entries.error());
      }

      auto result = device_access_per_cgroup.emplace(
          cgroup,
          CHECK_NOTERROR(DeviceManager::CgroupDeviceAccess::create(
              *allow_entries, *deny_entries)));

      CHECK(result.second); // There should be a single insertion per cgroup.

      recovered_cgroups.push_back(cgroup);
    }

    foreach (const string& cgroup, recovered_cgroups) {
      // Commit with checkpoint = false, since there's no need to re-checkpoint.
      Try<Nothing> commit = commit_device_access_changes(cgroup, false);
      if (commit.isError()) {
        // Return failure as the checkpointed state should be valid, allowing us
        // to generate and attach BPF programs. This is because the cgroup
        // previously succeeded in doing so.
        return Failure(
            "Failed to perform configuration of ebpf file for cgroup"
            " '" + cgroup + "': " + commit.error());
      }
    }

    // Checkpoint only after all cgroups are recovered to avoid deleting states
    // of unrecovered cgroups.
    Try<Nothing> status = checkpoint();

    if (status.isError()) {
      return Failure(
          "Failed to checkpoint device access state: " + status.error());
    }

    foreach(const string& cgroup, cgroups_to_recover) {
      if (!device_access_per_cgroup.contains(cgroup)) {
        LOG(WARNING)
          << "Unable to recover state for cgroup '" + cgroup + "' as requested"
             " by the containerizer, because it was missing in the device"
             " manager's checkpointed state";
      }
    }

    return Nothing();
  }

private:
  const string meta_dir;

  const string cgroups_root;

  hashmap<string, DeviceManager::CgroupDeviceAccess> device_access_per_cgroup;

  Try<Nothing> checkpoint() const
  {
    CgroupDeviceAccessStates states;

    foreachpair (const string& cgroup,
                 const DeviceManager::CgroupDeviceAccess& access,
                 device_access_per_cgroup) {
      CgroupDeviceAccessState* state = &(*(states.mutable_device_access_per_cgroup()))[cgroup];

      foreach (const Entry& entry, access.allow_list) {
        state->add_allow_list(stringify(entry));
      }
      foreach (const Entry& entry, access.deny_list) {
        state->add_deny_list(stringify(entry));
      }
    }

    Try<Nothing> status =
      state::checkpoint(paths::getDevicesStatePath(meta_dir), states);

    if (status.isError()) {
      return Error("Failed to perform checkpoint: " + status.error());
    }

    return Nothing();
  }

  Try<Nothing> commit_device_access_changes(
      const string& cgroup,
      bool write_checkpoint = true) const
  {
    if (write_checkpoint) {
      Try<Nothing> status = checkpoint();

      if (status.isError()) {
        return Error("Failed to checkpoint device access state: "
                    + status.error());
      }
    }

    Try<Nothing> status = cgroups2::devices::configure(
        cgroup,
        device_access_per_cgroup.at(cgroup).allow_list,
        device_access_per_cgroup.at(cgroup).deny_list);

    if (status.isError()) {
      return Error("Failed to configure device access: " + status.error());
    }

    return Nothing();
  }
};


Try<DeviceManager*> DeviceManager::create(const Flags& flags)
{
  return new DeviceManager(
      Owned<DeviceManagerProcess>(new DeviceManagerProcess(flags)));
}


DeviceManager::DeviceManager(
    const Owned<DeviceManagerProcess>& _process)
  : process(_process)
{
  spawn(*process);
}


DeviceManager::~DeviceManager()
{
  terminate(*process);
  process::wait(*process);
}


Future<Nothing> DeviceManager::reconfigure(
    const string& cgroup,
    const vector<DeviceManager::NonWildcardEntry>& additions,
    const vector<DeviceManager::NonWildcardEntry>& removals)
{
  return dispatch(
      *process,
      &DeviceManagerProcess::reconfigure,
      cgroup,
      additions,
      removals);
}


Future<Nothing> DeviceManager::configure(
    const string& cgroup,
    const vector<Entry>& allow_list,
    const vector<DeviceManager::NonWildcardEntry>& deny_list)
{
  return dispatch(
      *process,
      &DeviceManagerProcess::configure,
      cgroup,
      allow_list,
      deny_list);
}


Future<hashmap<string, DeviceManager::CgroupDeviceAccess>>
  DeviceManager::state() const
{
  // Necessary due to overloading of state().
  auto process_copy = process;
  return dispatch(*process, [process_copy]() {
    return process_copy->state();
  });
}


Future<DeviceManager::CgroupDeviceAccess> DeviceManager::state(
    const string& cgroup) const
{
  // Necessary due to overloading of state().
  auto process_copy = process;
  return dispatch(*process, [process_copy, cgroup]() {
    return process_copy->state(cgroup);
  });
}


DeviceManager::CgroupDeviceAccess DeviceManager::apply_diff(
    const DeviceManager::CgroupDeviceAccess& old_state,
    const vector<DeviceManager::NonWildcardEntry>& non_wildcard_additions,
    const vector<DeviceManager::NonWildcardEntry>& non_wildcard_removals)
{
  auto revoke_accesses = [](Entry* entry, const Entry& diff_entry) {
    CHECK(!entry->selector.has_wildcard());
    CHECK(!diff_entry.selector.has_wildcard());

    if (entry->selector.major == diff_entry.selector.major
        && entry->selector.minor == diff_entry.selector.minor
        && entry->selector.type == diff_entry.selector.type) {
      entry->access.mknod = entry->access.mknod && !diff_entry.access.mknod;
      entry->access.read = entry->access.read && !diff_entry.access.read;
      entry->access.write = entry->access.write && !diff_entry.access.write;
    }
  };

  DeviceManager::CgroupDeviceAccess new_state = old_state;
  vector<Entry> additions = convert_to_entries(non_wildcard_additions);
  vector<Entry> removals = convert_to_entries(non_wildcard_removals);

  foreach (const Entry& addition, additions) {
    // Go over each entry in deny list, find any entries that match the new
    // addition's major & minor numbers, remove any accesses they specify
    // that the addition also specifies.
    // Invariant: No device wildcards are allowed in the deny list.
    foreach (Entry& deny_entry, new_state.deny_list) {
      revoke_accesses(&deny_entry, addition);
    }

    new_state.allow_list.push_back(addition);
  }

  foreach (const Entry& removal, removals) {
    Entry::Access accesses_by_matching_wildcards;
    accesses_by_matching_wildcards.read = false;
    accesses_by_matching_wildcards.write = false;
    accesses_by_matching_wildcards.mknod = false;

    foreach (Entry& allow_entry, new_state.allow_list) {
      // Matching against wildcard - we cannot revoke wildcard privileges
      // so we will insert a deny entry replicating whatever privileges we
      // need to deny which the wildcard grants.
      if (allow_entry.selector.has_wildcard()) {
        // Does the allow wildcard match the removal device? Skip if not.
        if (allow_entry.selector.type != Entry::Selector::Type::ALL
            && allow_entry.selector.type != removal.selector.type) {
          continue; // Type doesn't match.
        }
        if (allow_entry.selector.major.isSome()
            && allow_entry.selector.major != removal.selector.major) {
          continue; // Major doesn't match.
        }
        if (allow_entry.selector.minor.isSome()
            && allow_entry.selector.minor != removal.selector.minor) {
          continue; // Minor doesn't match.
        }
        accesses_by_matching_wildcards.mknod |= allow_entry.access.mknod;
        accesses_by_matching_wildcards.read |= allow_entry.access.read;
        accesses_by_matching_wildcards.write |= allow_entry.access.write;
      } else {
        revoke_accesses(&allow_entry, removal);
      }
    }

    Entry::Access removal_access = removal.access;
    removal_access.mknod &= accesses_by_matching_wildcards.mknod;
    removal_access.read &= accesses_by_matching_wildcards.read;
    removal_access.write &= accesses_by_matching_wildcards.write;

    if (!removal_access.none()) {
      Entry to_push = removal;
      to_push.access = removal_access;
      new_state.deny_list.push_back(to_push);
    }
  }

  new_state.allow_list = cgroups2::devices::normalize(new_state.allow_list);
  new_state.deny_list = cgroups2::devices::normalize(new_state.deny_list);

  return new_state;
}


bool DeviceManager::CgroupDeviceAccess::is_access_granted(
    const Entry& query) const
{
  CHECK(cgroups2::devices::normalized(allow_list));
  CHECK(cgroups2::devices::normalized(deny_list));

  auto allowed = [&]() {
    foreach (const Entry& allow, allow_list) {
      if (allow.encompasses(query)) {
        return true;
      }
    }
    return false;
  };

  auto denied = [&]() {
    foreach (const Entry& deny, deny_list) {
      if (deny.selector.encompasses(query.selector)
          && deny.access.overlaps(query.access)) {
        return true;
      }
    }
    return false;
  };

  return allowed() && !denied();
}

bool DeviceManager::CgroupDeviceAccess::is_access_granted(
    const DeviceManager::NonWildcardEntry& query) const
{
  return is_access_granted(convert_to_entry(query));
}


DeviceManager::CgroupDeviceAccess::CgroupDeviceAccess(
  const std::vector<cgroups::devices::Entry>& _allow_list,
  const std::vector<cgroups::devices::Entry>& _deny_list)
  : allow_list(_allow_list), deny_list(_deny_list) {}


Try<DeviceManager::CgroupDeviceAccess>
DeviceManager::CgroupDeviceAccess::create(
    const vector<Entry>& allow_list,
    const vector<Entry>& deny_list)
{
  if (!cgroups2::devices::normalized(allow_list)
      || !(cgroups2::devices::normalized(deny_list))) {
    return Error("Failed to create CgroupDeviceAccess:"
                 " The allow or deny list is not normalized");
  }
  return CgroupDeviceAccess(allow_list, deny_list);
}


Future<Nothing> DeviceManager::recover(const vector<ContainerState>& states)
{
  return dispatch(*process, &DeviceManagerProcess::recover, states);
}


Future<Nothing> DeviceManager::remove(const std::string& cgroup)
{
  return dispatch(
      *process,
      &DeviceManagerProcess::remove,
      cgroup);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
