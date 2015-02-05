/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __FS_HPP__
#define __FS_HPP__

#include <fstab.h>
#include <mntent.h>

#include <sys/mount.h>

#include <string>
#include <vector>

#include <stout/nothing.hpp>
#include <stout/try.hpp>


namespace mesos {
namespace fs {


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
// @param   source    Specify the file system (often a device name).
// @param   target    Directory to be attached to.
// @param   type      File system type (listed in /proc/filesystems).
// @param   flags     Mount flags.
// @param   data      Extra data interpreted by different file systems.
// @return  Whether the mount operation succeeds.
Try<Nothing> mount(const std::string& source,
                   const std::string& target,
                   const std::string& type,
                   unsigned long flags,
                   const void* data);


// Unmount a file system.
// @param   target    The (topmost) directory where the file system attaches.
// @param   flags     Unmount flags.
// @return  Whether the unmount operation succeeds.
Try<Nothing> unmount(const std::string& target, int flags = 0);


} // namespace fs {
} // namespace mesos {


#endif // __FS_HPP__
