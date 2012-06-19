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

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <linux/limits.h>

#include "common/lock.hpp"

#include "linux/fs.hpp"


namespace mesos {
namespace internal {
namespace fs {


// Lock for guarding accesses to fstab functions.
static pthread_mutex_t fstabMutex = PTHREAD_MUTEX_INITIALIZER;

// Lock for guarding accesses to mntent functions.
static pthread_mutex_t mntentMutex = PTHREAD_MUTEX_INITIALIZER;


bool MountTable::Entry::hasOption(const std::string& option)
{
  struct mntent mntent;
  mntent.mnt_fsname = const_cast<char*>(fsname.c_str());
  mntent.mnt_dir = const_cast<char*>(dir.c_str());
  mntent.mnt_type = const_cast<char*>(type.c_str());
  mntent.mnt_opts = const_cast<char*>(opts.c_str());
  mntent.mnt_freq = freq;
  mntent.mnt_passno = passno;
  return ::hasmntopt(&mntent, option.c_str()) != NULL;
}


Try<MountTable> MountTable::read(const std::string& path)
{
  MountTable table;

  FILE* file = ::setmntent(path.c_str(), "r");
  if (file == NULL) {
    return Try<MountTable>::error("Failed to open " + path);
  }

  while (true) {
#if defined(_BSD_SOURCE) || defined(_SVID_SOURCE)
    // Reentrant version exists.
    struct mntent mntentBuffer;
    char strBuffer[PATH_MAX];
    struct mntent* mntent = ::getmntent_r(file, &mntentBuffer, strBuffer,
                                          sizeof(strBuffer));
    if (mntent == NULL) {
      // NULL means the end of enties.
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
    // Reentrant version does not exist. Use locks.
    {
      Lock lock(&mntentLock);
      struct mntent* mntent = ::getmntent(file);
      if (mntent == NULL) {
        // NULL means the end of enties.
        mntentLock->unlock();
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


Try<FileSystemTable> FileSystemTable::read()
{
  FileSystemTable table;

  // Use locks since fstab functions are not thread-safe.
  {
    Lock lock(&fstabMutex);

    // Open file _PATH_FSTAB (/etc/fstab).
    if (::setfsent() == 0) {
      return Try<FileSystemTable>::error("Failed to open file system table");
    }

    while (true) {
      struct fstab* fstab = ::getfsent();
      if (fstab == NULL) {
        // NULL means the end of enties.
        break;
      }

      FileSystemTable::Entry entry(fstab->fs_spec,
                                   fstab->fs_file,
                                   fstab->fs_vfstype,
                                   fstab->fs_mntops,
                                   fstab->fs_type,
                                   fstab->fs_freq,
                                   fstab->fs_passno);
      table.entries.push_back(entry);
    }

    ::endfsent();
  }

  return table;
}


Try<bool> mount(const std::string& source,
                const std::string& target,
                const std::string& type,
                unsigned long flags,
                const void* data)
{
  // The prototype of function 'mount' on Linux is as follows:
  // int mount(const char *source, const char *target,
  //           const char *filesystemtype, unsigned long mountflags,
  //           const void *data);
  if (::mount(source.c_str(), target.c_str(), type.c_str(), flags, data) < 0) {
    return Try<bool>::error(
        "Failed to mount " + source + " at " + target + ": " + strerror(errno));
  }

  return true;
}


Try<bool> unmount(const std::string& target, int flags)
{
  // The prototype of function 'umount2' on Linux is as follows:
  // int umount2(const char *target, int flags);
  if (::umount2(target.c_str(), flags) < 0) {
    return Try<bool>::error(
        "Failed to unmount " + target + ": " + strerror(errno));
  }

  return true;
}


} // namespace fs {
} // namespace internal {
} // namespace mesos {
