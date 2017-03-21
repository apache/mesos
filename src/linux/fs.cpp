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

#include "linux/fs.hpp"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <linux/limits.h>
#include <linux/unistd.h>

#include <list>
#include <set>
#include <utility>

#include <stout/adaptor.hpp>
#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/numify.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/synchronized.hpp>

#include <stout/fs.hpp>
#include <stout/os.hpp>

#include <stout/os/read.hpp>
#include <stout/os/shell.hpp>
#include <stout/os/stat.hpp>

#include "common/status_utils.hpp"

using std::list;
using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace fs {


Try<bool> supported(const string& fsname)
{
  hashset<string> overlayfs{"overlay", "overlayfs"};

  Try<string> lines = os::read("/proc/filesystems");
  if (lines.isError()) {
    return Error("Failed to read /proc/filesystems: " + lines.error());
  }

  // Each line of /proc/filesystems is "nodev" + "\t" + "fsname", and the
  // field "nodev" is optional. For the details, check the kernel src code:
  // https://github.com/torvalds/linux/blob/2101ae42899a14fe7caa73114e2161e778328661/fs/filesystems.c#L222-L237 NOLINT(whitespace/line_length)
  foreach (const string& line, strings::tokenize(lines.get(), "\n")) {
    vector<string> tokens = strings::tokenize(line, "\t");
    if (tokens.size() != 1 && tokens.size() != 2) {
      return Error("Failed to parse /proc/filesystems: '" + line + "'");
    }

    // The "overlayfs" was renamed to "overlay" in kernel 4.2, for overlay
    // support check function, it should check both "overlay" and "overlayfs"
    // in "/proc/filesystems".
    if (overlayfs.contains(fsname) && overlayfs.contains(tokens.back())) {
      return true;
    }

    if (fsname == tokens.back()) {
      return true;
    }
  }

  return false;
}


Try<uint32_t> type(const string& path)
{
  struct statfs buf;
  if (statfs(path.c_str(), &buf) < 0) {
    return ErrnoError();
  }
  return (uint32_t) buf.f_type;
}


Try<string> typeName(uint32_t fsType)
{
  // `typeNames` maps a filesystem id to its filesystem type name.
  hashmap<uint32_t, string> typeNames = {
    {FS_TYPE_AUFS      , "aufs"},
    {FS_TYPE_BTRFS     , "btrfs"},
    {FS_TYPE_CRAMFS    , "cramfs"},
    {FS_TYPE_ECRYPTFS  , "ecryptfs"},
    {FS_TYPE_EXTFS     , "extfs"},
    {FS_TYPE_F2FS      , "f2fs"},
    {FS_TYPE_GPFS      , "gpfs"},
    {FS_TYPE_JFFS2FS   , "jffs2fs"},
    {FS_TYPE_JFS       , "jfs"},
    {FS_TYPE_NFSFS     , "nfsfs"},
    {FS_TYPE_RAMFS     , "ramfs"},
    {FS_TYPE_REISERFS  , "reiserfs"},
    {FS_TYPE_SMBFS     , "smbfs"},
    {FS_TYPE_SQUASHFS  , "squashfs"},
    {FS_TYPE_TMPFS     , "tmpfs"},
    {FS_TYPE_VXFS      , "vxfs"},
    {FS_TYPE_XFS       , "xfs"},
    {FS_TYPE_ZFS       , "zfs"},
    {FS_TYPE_OVERLAY   , "overlay"}
  };

  if (!typeNames.contains(fsType)) {
    return Error("Unexpected filesystem type '" + stringify(fsType) + "'");
  }

  return typeNames[fsType];
}


Try<MountInfoTable> MountInfoTable::read(
    const string& lines,
    bool hierarchicalSort)
{
  MountInfoTable table;

  foreach (const string& line, strings::tokenize(lines, "\n")) {
    Try<Entry> parse = MountInfoTable::Entry::parse(line);
    if (parse.isError()) {
      return Error("Failed to parse entry '" + line + "': " + parse.error());
    }

    table.entries.push_back(parse.get());
  }

  // If `hierarchicalSort == true`, then sort the entries in
  // the newly constructed table hierarchically. That is, sort
  // them according to the invariant that all parent entries
  // appear before their child entries.
  if (hierarchicalSort) {
    Option<int> rootParentId = None();

    // Construct a representation of the mount hierarchy using a hashmap.
    hashmap<int, vector<MountInfoTable::Entry>> parentToChildren;

    foreach (const MountInfoTable::Entry& entry, table.entries) {
      if (entry.target == "/") {
        CHECK_NONE(rootParentId);
        rootParentId = entry.parent;
      }
      parentToChildren[entry.parent].push_back(entry);
    }

    // Walk the hashmap and construct a list of entries sorted
    // hierarchically. The recursion eventually terminates because
    // entries in MountInfoTable are guaranteed to have no cycles.
    // We double check though, just to make sure.
    hashset<int> visitedParents;
    vector<MountInfoTable::Entry> sortedEntries;

    std::function<void(int)> sortFrom = [&](int parentId) {
      CHECK(!visitedParents.contains(parentId))
        << "Cycle found in mount table hierarchy at entry"
        << " '" << stringify(parentId) << "': " << std::endl << lines;

      visitedParents.insert(parentId);

      foreach (const MountInfoTable::Entry& entry, parentToChildren[parentId]) {
        sortedEntries.push_back(entry);

        // It is legal to have a `MountInfoTable` entry whose
        // `entry.id` is the same as its `entry.parent`. This can
        // happen (for example), if a system boots from the network
        // and then keeps the original `/` in RAM. To avoid cycles
        // when walking the mount hierarchy, we only recurse into our
        // children if this case is not satisfied.
        if (parentId != entry.id) {
          sortFrom(entry.id);
        }
      }
    };

    // We know the node with a parent id of
    // `rootParentId` is the root mount point.
    CHECK_SOME(rootParentId);
    sortFrom(rootParentId.get());

    table.entries = std::move(sortedEntries);
  }

  return table;
}


Try<MountInfoTable> MountInfoTable::read(
    const Option<pid_t>& pid,
    bool hierarchicalSort)
{
  const string path = path::join(
      "/proc",
      (pid.isSome() ? stringify(pid.get()) : "self"),
      "mountinfo");

  Try<string> lines = os::read(path);
  if (lines.isError()) {
    return Error("Failed to read mountinfo file: " + lines.error());
  }

  return MountInfoTable::read(lines.get(), hierarchicalSort);
}


Try<MountInfoTable::Entry> MountInfoTable::Entry::parse(const string& s)
{
  MountInfoTable::Entry entry;

  const string separator = " - ";
  size_t pos = s.find(separator);
  if (pos == string::npos) {
    return Error("Could not find separator ' - '");
  }

  // First group of fields (before the separator): 6 required fields
  // then zero or more optional fields
  vector<string> tokens = strings::tokenize(s.substr(0, pos), " ");
  if (tokens.size() < 6) {
    return Error("Failed to parse entry");
  }

  Try<int> id = numify<int>(tokens[0]);
  if (id.isError()) {
    return Error("Mount ID is not a number");
  }
  entry.id = id.get();

  Try<int> parent = numify<int>(tokens[1]);
  if (parent.isError()) {
    return Error("Parent ID is not a number");
  }
  entry.parent = parent.get();

  // Parse out the major:minor device number.
  vector<string> device = strings::split(tokens[2], ":");
  if (device.size() != 2) {
    return Error("Invalid major:minor device number");
  }

  Try<int> major = numify<int>(device[0]);
  if (major.isError()) {
    return Error("Device major is not a number");
  }

  Try<int> minor = numify<int>(device[1]);
  if (minor.isError()) {
    return Error("Device minor is not a number");
  }

  entry.devno = makedev(major.get(), minor.get());

  entry.root = tokens[3];
  entry.target = tokens[4];

  entry.vfsOptions = tokens[5];

  // The "proc" manpage states there can be zero or more optional
  // fields. The kernel source (fs/proc_namespace.c) has the optional
  // fields ("tagged fields") separated by " " when printing the table
  // (see show_mountinfo()).
  if (tokens.size() > 6) {
    tokens.erase(tokens.begin(), tokens.begin() + 6);
    entry.optionalFields = strings::join(" ", tokens);
  }

  // Second set of fields: 3 required fields.
  tokens = strings::tokenize(s.substr(pos + separator.size() - 1), " ");
  if (tokens.size() != 3) {
    return Error("Failed to parse type, source or options");
  }

  entry.type = tokens[0];
  entry.source = tokens[1];
  entry.fsOptions = tokens[2];

  return entry;
}


Option<int> MountInfoTable::Entry::shared() const
{
  foreach (const string& token, strings::tokenize(optionalFields, " ")) {
    if (strings::startsWith(token, "shared:")) {
      Try<int> id = numify<int>(
          strings::remove(token, "shared:", strings::PREFIX));

      CHECK_SOME(id);
      return id.get();
    }
  }

  return None();
}


Option<int> MountInfoTable::Entry::master() const
{
  foreach (const string& token, strings::tokenize(optionalFields, " ")) {
    if (strings::startsWith(token, "master:")) {
      Try<int> id = numify<int>(
          strings::remove(token, "master:", strings::PREFIX));

      CHECK_SOME(id);
      return id.get();
    }
  }

  return None();
}


bool MountTable::Entry::hasOption(const string& option) const
{
  struct mntent mntent;
  mntent.mnt_fsname = const_cast<char*>(fsname.c_str());
  mntent.mnt_dir = const_cast<char*>(dir.c_str());
  mntent.mnt_type = const_cast<char*>(type.c_str());
  mntent.mnt_opts = const_cast<char*>(opts.c_str());
  mntent.mnt_freq = freq;
  mntent.mnt_passno = passno;
  return ::hasmntopt(&mntent, option.c_str()) != nullptr;
}


Try<MountTable> MountTable::read(const string& path)
{
  MountTable table;

  FILE* file = ::setmntent(path.c_str(), "r");
  if (file == nullptr) {
    return Error("Failed to open '" + path + "'");
  }

  while (true) {
#if defined(_BSD_SOURCE) || defined(_SVID_SOURCE)
    // Reentrant version exists.
    struct mntent mntentBuffer;
    char strBuffer[PATH_MAX];
    struct mntent* mntent =
      ::getmntent_r(file, &mntentBuffer, strBuffer, sizeof(strBuffer));
    if (mntent == nullptr) {
      // nullptr means the end of enties.
      break;
    }

    MountTable::Entry entry(mntent->mnt_fsname,
                            mntent->mnt_dir,
                            mntent->mnt_type,
                            mntent->mnt_opts,
                            mntent->mnt_freq,
                            mntent->mnt_passno);
    table.entries.push_back(entry);
#else
    // Mutex for guarding calls into non-reentrant mount table
    // functions. We use a static local variable to avoid unused
    // variable warnings.
    static std::mutex mutex;

    synchronized (mutex) {
      struct mntent* mntent = ::getmntent(file);
      if (mntent == nullptr) {
        // nullptr means the end of enties.
        break;
      }

      MountTable::Entry entry(mntent->mnt_fsname,
                              mntent->mnt_dir,
                              mntent->mnt_type,
                              mntent->mnt_opts,
                              mntent->mnt_freq,
                              mntent->mnt_passno);
      table.entries.push_back(entry);
    }
#endif
  }

  ::endmntent(file);

  return table;
}


Try<Nothing> mount(const Option<string>& source,
                   const string& target,
                   const Option<string>& type,
                   unsigned long flags,
                   const void* data)
{
  // The prototype of function 'mount' on Linux is as follows:
  // int mount(const char *source,
  //           const char *target,
  //           const char *filesystemtype,
  //           unsigned long mountflags,
  //           const void *data);
  if (::mount(
        (source.isSome() ? source.get().c_str() : nullptr),
        target.c_str(),
        (type.isSome() ? type.get().c_str() : nullptr),
        flags,
        data) < 0) {
    return ErrnoError();
  }

  return Nothing();
}


Try<Nothing> mount(const Option<string>& source,
                   const string& target,
                   const Option<string>& type,
                   unsigned long flags,
                   const Option<string>& options)
{
  return mount(
      source,
      target,
      type,
      flags,
      options.isSome() ? options.get().c_str() : nullptr);
}


Try<Nothing> unmount(const string& target, int flags)
{
  // The prototype of function 'umount2' on Linux is as follows:
  // int umount2(const char *target, int flags);
  if (::umount2(target.c_str(), flags) < 0) {
    return ErrnoError("Failed to unmount '" + target + "'");
  }

  return Nothing();
}


Try<Nothing> unmountAll(const string& target, int flags)
{
  Try<fs::MountTable> mountTable = fs::MountTable::read("/proc/mounts");
  if (mountTable.isError()) {
    return Error("Failed to read mount table: " + mountTable.error());
  }

  foreach (const MountTable::Entry& entry,
           adaptor::reverse(mountTable.get().entries)) {
    if (strings::startsWith(entry.dir, target)) {
      Try<Nothing> unmount = fs::unmount(entry.dir, flags);
      if (unmount.isError()) {
        return unmount;
      }

      // This normally should not fail even if the entry is not in
      // mtab or mtab doesn't exist or is not writable. However we
      // still catch the error here in case there's an error somewhere
      // else while running this command.
      // TODO(xujyan): Consider using `setmntent(3)` to implement this.
      int status = os::spawn("umount", {"umount", "--fake", entry.dir});

      const string message =
        "Failed to clean up '" + entry.dir + "' in /etc/mtab";

      if (status == -1) {
        return ErrnoError(message);
      }

      if (!WSUCCEEDED(status)) {
        return Error(message + ": " + WSTRINGIFY(status));
      }
    }
  }

  return Nothing();
}


Try<Nothing> pivot_root(
    const string& newRoot,
    const string& putOld)
{
  // These checks are done in the syscall but we'll do them here to
  // provide less cryptic error messages. See 'man 2 pivot_root'.
  if (!os::stat::isdir(newRoot)) {
    return Error("newRoot '" + newRoot + "' is not a directory");
  }

  if (!os::stat::isdir(putOld)) {
    return Error("putOld '" + putOld + "' is not a directory");
  }

  // TODO(idownes): Verify that newRoot (and putOld) is on a different
  // filesystem to the current root. st_dev distinguishes the device
  // an inode is on, but bind mounts (which are acceptable to
  // pivot_root) share the same st_dev as the source of the mount so
  // st_dev is not generally sufficient.

  if (!strings::startsWith(putOld, newRoot)) {
    return Error("putOld '" + putOld +
                 "' must be beneath newRoot '" + newRoot);
  }

#ifdef __NR_pivot_root
  int ret = ::syscall(__NR_pivot_root, newRoot.c_str(), putOld.c_str());
#else
#error "pivot_root is not available"
#endif
  if (ret == -1) {
    return ErrnoError();
  }

  return Nothing();
}


namespace chroot {

namespace internal {

Try<Nothing> copyDeviceNode(const string& source, const string& target)
{
  // We are likely to be operating in a multi-threaded environment so
  // it's not safe to change the umask. Instead, we'll explicitly set
  // permissions after we create the device node.
  Try<mode_t> mode = os::stat::mode(source);
  if (mode.isError()) {
    return Error("Failed to source mode: " + mode.error());
  }

  Try<dev_t> dev = os::stat::rdev(source);
  if (dev.isError()) {
    return Error("Failed to get source dev: " + dev.error());
  }

  Try<Nothing> mknod = os::mknod(target, mode.get(), dev.get());
  if (mknod.isError()) {
    return Error("Failed to create device:" +  mknod.error());
  }

  Try<Nothing> chmod = os::chmod(target, mode.get());
  if (chmod.isError()) {
    return Error("Failed to chmod device: " + chmod.error());
  }

  return Nothing();
}


// Some helpful types.
struct Mount
{
  Option<string> source;
  string target;
  Option<string> type;
  Option<string> options;
  unsigned long flags;
};

struct SymLink
{
  string original;
  string link;
};


Try<Nothing> mountSpecialFilesystems(const string& root)
{
  // List of special filesystems useful for a chroot environment.
  // NOTE: This list is ordered, e.g., mount /proc before bind
  // mounting /proc/sys and then making it read-only.
  vector<Mount> mounts = {
    {"proc",      "/proc",     "proc",   None(),      MS_NOSUID | MS_NOEXEC | MS_NODEV},             // NOLINT(whitespace/line_length)
    {"/proc/sys", "/proc/sys", None(),   None(),      MS_BIND},
    {None(),      "/proc/sys", None(),   None(),      MS_BIND | MS_RDONLY | MS_REMOUNT},             // NOLINT(whitespace/line_length)
    {"sysfs",     "/sys",      "sysfs",  None(),      MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV}, // NOLINT(whitespace/line_length)
    {"tmpfs",     "/dev",      "tmpfs",  "mode=755",  MS_NOSUID | MS_STRICTATIME},                   // NOLINT(whitespace/line_length)
    {"devpts",    "/dev/pts",  "devpts", "newinstance,ptmxmode=0666", MS_NOSUID | MS_NOEXEC},        // NOLINT(whitespace/line_length)
    {"tmpfs",     "/dev/shm",  "tmpfs",  "mode=1777", MS_NOSUID | MS_NODEV | MS_STRICTATIME},        // NOLINT(whitespace/line_length)
  };

  foreach (const Mount& mount, mounts) {
    // Target is always under the new root.
    const string target = path::join(root, mount.target);

    // Try to create the mount point, if it doesn't already exist.
    if (!os::exists(target)) {
      Try<Nothing> mkdir = os::mkdir(target);

      if (mkdir.isError()) {
        return Error("Failed to create mount point '" + target +
                     "': " + mkdir.error());
      }
    }

    // If source is a path, e.g,. for a bind mount, then it needs to
    // be prefixed by the new root.
    Option<string> source;
    if (mount.source.isSome() && strings::startsWith(mount.source.get(), "/")) {
      source = path::join(root, mount.source.get());
    } else {
      source = mount.source;
    }

    Try<Nothing> mnt = fs::mount(
        source,
        target,
        mount.type,
        mount.flags,
        mount.options);

    if (mnt.isError()) {
      return Error("Failed to mount '" + target + "': " + mnt.error());
    }
  }

  return Nothing();
}


Try<Nothing> createStandardDevices(const string& root)
{
  // List of standard devices useful for a chroot environment.
  // TODO(idownes): Make this list configurable.
  vector<string> devices = {
    "full",
    "null",
    "random",
    "tty",
    "urandom",
    "zero"
  };

  // Glob all Nvidia GPU devices on the system and add them to the
  // list of devices injected into the chroot environment.
  //
  // TODO(klueska): Only inject these devices if the 'gpu/nvidia'
  // isolator is enabled.
  Try<list<string>> nvidia = os::glob("/dev/nvidia*");
  if (nvidia.isError()) {
    return Error("Failed to glob /dev/nvidia* on the host filesystem:"
                 " " + nvidia.error());
  }

  foreach (const string& device, nvidia.get()) {
    if (os::exists(device)) {
      devices.push_back(Path(device).basename());
    }
  }

  // Inject each device into the chroot environment. Copy both the
  // mode and the device itself from the corresponding host device.
  foreach (const string& device, devices) {
    Try<Nothing> copy = copyDeviceNode(
        path::join("/",  "dev", device),
        path::join(root, "dev", device));

    if (copy.isError()) {
      return Error("Failed to copy device '" + device + "': " + copy.error());
    }
  }

  vector<SymLink> symlinks = {
    {"/proc/self/fd",   path::join(root, "dev", "fd")},
    {"/proc/self/fd/0", path::join(root, "dev", "stdin")},
    {"/proc/self/fd/1", path::join(root, "dev", "stdout")},
    {"/proc/self/fd/2", path::join(root, "dev", "stderr")},
    {"pts/ptmx",       path::join(root, "dev", "ptmx")}
  };

  foreach (const SymLink& symlink, symlinks) {
    Try<Nothing> link = ::fs::symlink(symlink.original, symlink.link);
    if (link.isError()) {
      return Error("Failed to symlink '" + symlink.original +
                   "' to '" + symlink.link + "': " + link.error());
    }
  }

  // TODO(idownes): Set up console device.
  return Nothing();
}

} // namespace internal {


// TODO(idownes): Add unit test.
Try<Nothing> enter(const string& root)
{
  // Recursively mark current mounts as slaves to prevent propagation.
  Try<Nothing> mount =
    fs::mount(None(), "/", None(), MS_REC | MS_SLAVE, nullptr);

  if (mount.isError()) {
    return Error("Failed to make slave mounts: " + mount.error());
  }

  // Bind mount 'root' itself. This is because pivot_root requires
  // 'root' to be not on the same filesystem as process' current root.
  mount = fs::mount(root, root, None(), MS_REC | MS_BIND, nullptr);
  if (mount.isError()) {
    return Error("Failed to bind mount root itself: " + mount.error());
  }

  // Mount special filesystems.
  mount = internal::mountSpecialFilesystems(root);
  if (mount.isError()) {
    return Error("Failed to mount: " + mount.error());
  }

  // Create basic device nodes.
  Try<Nothing> create = internal::createStandardDevices(root);
  if (create.isError()) {
    return Error("Failed to create devices: " + create.error());
  }

  // Prepare /tmp in the new root. Note that we cannot assume that the
  // new root is writable (i.e., it could be a read only filesystem).
  // Therefore, we always mount a tmpfs on /tmp in the new root so
  // that we can create the mount point for the old root.
  //
  // NOTE: If the new root is a read-only filesystem (e.g., using bind
  // backend), the 'tmpfs' mount point '/tmp' must already exist in the
  // new root. Otherwise, mkdir would return an error because of unable
  // to create it in read-only filesystem.
  Try<Nothing> mkdir = os::mkdir(path::join(root, "tmp"));
  if (mkdir.isError()) {
    return Error("Failed to create 'tmpfs' mount point at '" +
                 path::join(root, "tmp") + "': " + mkdir.error());
  }

  // TODO(jieyu): Consider limiting the size of the tmpfs.
  mount = fs::mount(
      "tmpfs",
      path::join(root, "tmp"),
      "tmpfs",
      MS_NOSUID | MS_NOEXEC | MS_NODEV,
      "mode=1777");

  if (mount.isError()) {
    return Error("Failed to mount the temporary tmpfs at /tmp in new root: " +
                 mount.error());
  }

  // Create a mount point for the old root.
  Try<string> old = os::mkdtemp(path::join(root, "tmp", "._old_root_.XXXXXX"));
  if (old.isError()) {
    return Error("Failed to create mount point for old root: " + old.error());
  }

  // Chroot to the new root. This is done by a particular sequence of
  // operations, each of which is necessary: chdir, pivot_root,
  // chroot, chdir. After these operations, the process will be
  // chrooted to the new root.

  // Chdir to the new root.
  Try<Nothing> chdir = os::chdir(root);
  if (chdir.isError()) {
    return Error("Failed to chdir to new root: " + chdir.error());
  }

  // Pivot the root to the cwd.
  Try<Nothing> pivot = fs::pivot_root(root, old.get());
  if (pivot.isError()) {
    return Error("Failed to pivot to new root: " + pivot.error());
  }

  // Chroot to the new "/". This is necessary to correctly set the
  // base for all paths.
  Try<Nothing> chroot = os::chroot(".");
  if (chroot.isError()) {
    return Error("Failed to chroot to new root: " + chroot.error());
  }

  // Ensure all references are within the new root.
  chdir = os::chdir("/");
  if (chdir.isError()) {
    return Error("Failed to chdir to new root: " + chdir.error());
  }

  // Unmount filesystems on the old root. Note, any filesystems that
  // were mounted to the chroot directory will be correctly pivoted.
  Try<fs::MountTable> mountTable = fs::MountTable::read("/proc/mounts");
  if (mountTable.isError()) {
    return Error("Failed to read mount table: " + mountTable.error());
  }

  // The old root is now relative to chroot so remove the chroot path.
  const string relativeOld = strings::remove(old.get(), root, strings::PREFIX);

  foreach (const fs::MountTable::Entry& entry, mountTable.get().entries) {
    // TODO(idownes): sort the entries and remove depth first so we
    // don't rely on the lazy umount and can check the status.
    if (strings::startsWith(entry.dir, relativeOld)) {
      fs::unmount(entry.dir, MNT_DETACH);
    }
  }

  // TODO(idownes): If any of the lazy umounts above is still pending
  // this will fail, leaving behind an empty directory which we'll
  // ignore.
  // Check status when we stop using lazy umounts.
  os::rmdir(relativeOld);

  Try<Nothing> unmount = fs::unmount("/tmp");
  if (unmount.isError()) {
    return Error("Failed to umount /tmp in the chroot: " + unmount.error());
  }

  return Nothing();
}

} // namespace chroot {
} // namespace fs {
} // namespace internal {
} // namespace mesos {
