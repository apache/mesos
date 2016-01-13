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

#include <fstab.h>
#include <mntent.h>

#include <sys/mount.h>
#include <sys/types.h>

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

namespace mesos {
namespace internal {
namespace fs {

// TODO(idownes): These three variations on mount information should
// be consolidated and moved to stout, along with mount and umount.

// Structure describing the per-process mounts as found in
// /proc/[pid]/mountinfo. In particular, entries in this table specify
// the propagation properties of mounts, information not present in
// the MountTable or FileSystemTable. Entry order is preserved when
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

  // If pid is None() the "self" is used, i.e., the mountinfo table
  // for the calling process.
  static Try<MountInfoTable> read(const Option<pid_t>& pid = None());

  // TODO(jieyu): Introduce 'find' methods to find entries that match
  // the given conditions (e.g., target, root, devno, etc.).

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


// Structure describing a file system table (e.g. /etc/fstab).
struct FileSystemTable {
  // Structure describing a file system table entry. This is a wrapper for
  // struct fstab defined in <fstab.h>
  struct Entry {
    Entry() : freq(0), passno(0) {}

    Entry(const std::string& _spec,
          const std::string& _file,
          const std::string& _vfstype,
          const std::string& _mntops,
          const std::string& _type,
          int _freq,
          int _passno)
      : spec(_spec),
        file(_file),
        vfstype(_vfstype),
        mntops(_mntops),
        type(_type),
        freq(_freq),
        passno(_passno)
    {}

    std::string spec;     // Block special device name.
    std::string file;     // File system path prefix.
    std::string vfstype;  // File system type, ufs, nfs.
    std::string mntops;   // Mount options ala -o.
    std::string type;     // FSTAB_* from fs_mntops.
    int freq;             // Dump frequency, in days.
    int passno;           // Pass number on parallel dump.
  };

  // Read the file system table from a file.
  // @param   path    The path of the file storing the file system table.
  // @return  An instance of FileSystemTable if success.
  static Try<FileSystemTable> read();

  std::vector<Entry> entries;
};


// Mount a file system.
// @param   source    Specify the file system (often a device name but
//                    it can also be a directory for a bind mount).
//                    If None(), NULL will be passed as a dummy
//                    argument to mount(), i.e., it is not used for
//                    the specified mount operation. For example, you
//                    can mount a filesystem specified in /etc/fstab
//                    by just specifying the target.
// @param   target    Directory to be attached to.
// @param   type      File system type (listed in /proc/filesystems).
//                    If None(), NULL will be passed as a dummy
//                    argument to mount(), i.e., it is not used for
//                    the specified mount operation. For example, it
//                    should be None() for a bind mount as it will
//                    inherit the type of the source.
// @param   flags     Mount flags.
// @param   data      Extra data interpreted by different file systems.
// @return  Whether the mount operation succeeds.
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

// Enter a 'chroot' enviroment. The caller should be in a new mount
// namespace. Basic configuration of special filesystems and device
// nodes is performed. Any mounts to the current root will be
// unmounted.
Try<Nothing> enter(const std::string& root);

} // namespace chroot {

} // namespace fs {
} // namespace internal {
} // namespace mesos {


#endif // __FS_HPP__
