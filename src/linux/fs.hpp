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

#ifndef __FS_HPP__
#define __FS_HPP__

#include <mntent.h>

#include <sys/mount.h>
#include <sys/types.h>
#include <sys/vfs.h>

#include <string>
#include <vector>

#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

// Define relevant MS_* flags for old includes.
// This is taken from the enum in sys/mount.h.
#ifndef MS_RDONLY
#define MS_RDONLY 1
#endif

#ifndef MS_NOSUID
#define MS_NOSUID 2
#endif

#ifndef MS_NODEV
#define MS_NODEV 4
#endif

#ifndef MS_NOEXEC
#define MS_NOEXEC 8
#endif

#ifndef MS_SYNCHRONOUS
#define MS_SYNCHRONOUS 16
#endif

#ifndef MS_REMOUNT
#define MS_REMOUNT 32
#endif

#ifndef MS_MANDLOCK
#define MS_MANDLOCK 64
#endif

#ifndef MS_DIRSYNC
#define MS_DIRSYNC 128
#endif

#ifndef MS_NOATIME
#define MS_NOATIME 1024
#endif

#ifndef MS_NODIRATIME
#define MS_NODIRATIME 2048
#endif

#ifndef MS_BIND
#define MS_BIND 4096
#endif

#ifndef MS_MOVE
#define MS_MOVE 8192
#endif

#ifndef MS_REC
#define MS_REC 16384
#endif

#ifndef MS_SILENT
#define MS_SILENT 32768
#endif

#ifndef MS_POSIXACL
#define MS_POSIXACL (1 << 16)
#endif

#ifndef MS_UNBINDABLE
#define MS_UNBINDABLE (1 << 17)
#endif

#ifndef MS_PRIVATE
#define MS_PRIVATE (1 << 18)
#endif

#ifndef MS_SLAVE
#define MS_SLAVE (1 << 19)
#endif

#ifndef MS_SHARED
#define MS_SHARED (1 << 20)
#endif

#ifndef MS_RELATIME
#define MS_RELATIME (1 << 21)
#endif

#ifndef MS_KERNMOUNT
#define MS_KERNMOUNT (1 << 22)
#endif

#ifndef MS_I_VERSION
#define MS_I_VERSION (1 << 23)
#endif

#ifndef MS_STRICTATIME
#define MS_STRICTATIME (1 << 24)
#endif

#ifndef MS_ACTIVE
#define MS_ACTIVE (1 << 30)
#endif

#ifndef MS_NOUSER
#define MS_NOUSER (1 << 31)
#endif

#ifndef MNT_FORCE
#define MNT_FORCE 1
#endif

#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif

#ifndef MNT_EXPIRE
#define MNT_EXPIRE 4
#endif

#ifndef UMOUNT_NOFOLLOW
#define UMOUNT_NOFOLLOW 8
#endif

// Define FS_MAGIC_* flags for filesystem types.
// http://man7.org/linux/man-pages/man2/fstatfs64.2.html
#define FS_TYPE_AUFS 0x61756673
#define FS_TYPE_BTRFS 0x9123683E
#define FS_TYPE_CRAMFS 0x28cd3d45
#define FS_TYPE_ECRYPTFS 0xf15f
#define FS_TYPE_EXTFS 0x0000EF53
#define FS_TYPE_F2FS 0xF2F52010
#define FS_TYPE_GPFS 0x47504653
#define FS_TYPE_JFFS2FS 0x000072b6
#define FS_TYPE_JFS 0x3153464a
#define FS_TYPE_NFSFS 0x00006969
#define FS_TYPE_RAMFS 0x858458f6
#define FS_TYPE_REISERFS 0x52654973
#define FS_TYPE_SMBFS 0x0000517B
#define FS_TYPE_SQUASHFS 0x73717368
#define FS_TYPE_TMPFS 0x01021994
#define FS_TYPE_VXFS 0xa501fcf5
#define FS_TYPE_XFS 0x58465342
#define FS_TYPE_ZFS 0x2fc12fc1
#define FS_TYPE_OVERLAY 0x794C7630

namespace mesos {
namespace internal {
namespace fs {

// Detect whether the given file system is supported by the kernel.
Try<bool> supported(const std::string& fsname);


// Detect whether the given file system supports `d_type`
// in `struct dirent`.
// @directory must not be empty for correct `d_type` detection.
// It is the caller's responsibility to ensure this holds.
Try<bool> dtypeSupported(const std::string& directory);


// Returns a filesystem type id, given a directory.
// http://man7.org/linux/man-pages/man2/fstatfs64.2.html
Try<uint32_t> type(const std::string& path);


// Returns the filesystem type name, given a filesystem type id.
Try<std::string> typeName(uint32_t fsType);

// TODO(idownes): These different variations of mount information
// should be consolidated and moved to stout, along with mount and
// umount.

// Structure describing the per-process mounts as found in
// /proc/[pid]/mountinfo. In particular, entries in this table specify
// the propagation properties of mounts, information not present in
// the MountTable or /etc/fstab. Entry order is preserved when
// parsing /proc/[pid]/mountinfo.
struct MountInfoTable {
  // Structure describing an individual /proc/[pid]/mountinfo entry.
  // See the /proc/[pid]/mountinfo section in 'man proc' for further
  // details on each field.
  struct Entry {
    static Try<Entry> parse(const std::string& s);

    Entry() {}

    // Returns the ID of the peer group in which this mount resides.
    // Returns none if this mount is not a shared mount.
    Option<int> shared() const;

    // Returns the ID of the peer group in which this mount's master
    // resides in. Returns none if this mount is not a slave mount.
    Option<int> master() const;

    int id;                     // mountinfo[1]: mount ID.
    int parent;                 // mountinfo[2]: parent ID.
    dev_t devno;                // mountinfo[3]: st_dev.

    std::string root;           // mountinfo[4]: root of the mount.
    std::string target;         // mountinfo[5]: mount point.

    // Filesystem independent (VFS) options, e.g., "rw,noatime".
    std::string vfsOptions;     // mountinfo[6]: per-mount options.
    // Filesystem dependent options, e.g., "rw,memory" for a memory
    // cgroup filesystem.
    std::string fsOptions;      // mountinfo[11]: per-block options.

    // Current possible optional fields include shared:X, master:X,
    // propagate_from:X, unbindable.
    std::string optionalFields; // mountinfo[7]: optional fields.

    // mountinfo[8] is a separator.

    std::string type;           // mountinfo[9]: filesystem type.
    std::string source;         // mountinfo[10]: source dev, other.
  };

  // Read the mountinfo table for a process.
  // @param   pid     The `pid` of the process for which we should
  //                  read the mountinfo table. If `pid` is None(),
  //                  then "self" is used, i.e., the mountinfo table
  //                  for the calling process.
  // @param   hierarchicalSort
  //                  A boolean indicating whether the entries in the
  //                  mountinfo table should be sorted according to
  //                  their parent / child relationship (as opposed to
  //                  the temporal ordering of when they were
  //                  mounted). The two orderings may differ (for
  //                  example) if a filesystem is remounted after some
  //                  of its children have been mounted.
  // @return  An instance of MountInfoTable if success.
  static Try<MountInfoTable> read(
      const Option<pid_t>& pid = None(),
      bool hierarchicalSort = true);

  // Read a mountinfo table from a string.
  // @param   lines   The contents of a mountinfo table represented as
  //                  a string. Different entries in the string are
  //                  separated by a newline.
  // @param   hierarchicalSort
  //                  A boolean indicating whether the entries in the
  //                  mountinfo table should be sorted according to
  //                  their parent / child relationship (as opposed to
  //                  the temporal ordering of when they were
  //                  mounted). The two orderings may differ (for
  //                  example) if a filesystem is remounted after some
  //                  of its children have been mounted.
  // @return  An instance of MountInfoTable if success.
  static Try<MountInfoTable> read(
      const std::string& lines,
      bool hierarchicalSort = true);

  // Find the mount table entry by the given target path. If there is
  // no mount table entry that matches the exact target path, return
  // the mount table entry that is the immediate parent of the given
  // target path (similar to `findmnt --target [TARGET]`).
  Try<Entry> findByTarget(const std::string& target);

  std::vector<Entry> entries;
};


// Structure describing a mount table (e.g. /etc/mtab or /proc/mounts).
struct MountTable {
  // Structure describing a mount table entry. This is a wrapper for struct
  // mntent defined in <mntent.h>.
  struct Entry {
    Entry() : freq(0), passno(0) {}

    Entry(const std::string& _fsname,
          const std::string& _dir,
          const std::string& _type,
          const std::string& _opts,
          int _freq,
          int _passno)
      : fsname(_fsname),
        dir(_dir),
        type(_type),
        opts(_opts),
        freq(_freq),
        passno(_passno)
    {}

    // Check whether a given mount option exists in a mount table entry.
    // @param   option    The given mount option.
    // @return  Whether the given mount option exists.
    bool hasOption(const std::string& option) const;

    std::string fsname; // Device or server for filesystem.
    std::string dir;    // Directory mounted on.
    std::string type;   // Type of filesystem: ufs, nfs, etc.
    std::string opts;   // Comma-separated options for fs.
    int freq;           // Dump frequency (in days).
    int passno;         // Pass number for `fsck'.
  };

  // Read the mount table from a file.
  // @param   path    The path of the file storing the mount table.
  // @return  An instance of MountTable if success.
  static Try<MountTable> read(const std::string& path);

  std::vector<Entry> entries;
};


// Mount a file system.
// @param   source    Specify the file system (often a device name but
//                    it can also be a directory for a bind mount).
//                    If None(), nullptr will be passed as a dummy
//                    argument to mount(), i.e., it is not used for
//                    the specified mount operation. For example, you
//                    can mount a filesystem specified in /etc/fstab
//                    by just specifying the target.
// @param   target    Directory to be attached to.
// @param   type      File system type (listed in /proc/filesystems).
//                    If None(), nullptr will be passed as a dummy
//                    argument to mount(), i.e., it is not used for
//                    the specified mount operation. For example, it
//                    should be None() for a bind mount as it will
//                    inherit the type of the source.
// @param   flags     Mount flags.
// @param   data      Extra data interpreted by different file systems.
// @return  Whether the mount operation succeeds.
//
// Note that if this is a read-only bind mount (both the MS_BIND
// and MS_READONLY flags are set), the target will automatically
// be remounted in read-only mode.
Try<Nothing> mount(const Option<std::string>& source,
                   const std::string& target,
                   const Option<std::string>& type,
                   unsigned long flags,
                   const void* data);


// Alternate version of mount which passes an option string as
// additional data for the filesystem mount.
Try<Nothing> mount(const Option<std::string>& source,
                   const std::string& target,
                   const Option<std::string>& type,
                   unsigned long flags,
                   const Option<std::string>& options);


// Unmount a file system.
// @param   target    The (topmost) directory where the file system attaches.
// @param   flags     Unmount flags.
// @return  Whether the unmount operation succeeds.
Try<Nothing> unmount(const std::string& target, int flags = 0);


// Unmount a file system and all mounts under that file system.
// @param   target    The (topmost) directory where the file system attaches.
// @param   flags     Unmount flags.
// @return  Whether the unmountAll operation succeeds.
Try<Nothing> unmountAll(const std::string& target, int flags = 0);


// Change the root filesystem.
Try<Nothing> pivot_root(const std::string& newRoot, const std::string& putOld);

namespace chroot {

// Clone a device node to a target directory. Intermediate directory paths
// are created in the target.
Try<Nothing> copyDeviceNode(
    const std::string& device, const std::string& target);

//  Enter a 'chroot' environment. The caller should be in a new mount
//  unmounted. The root path must have already been provisioned by
//  calling `prepare`()`.
Try<Nothing> enter(const std::string& root);

} // namespace chroot {

} // namespace fs {
} // namespace internal {
} // namespace mesos {


#endif // __FS_HPP__
