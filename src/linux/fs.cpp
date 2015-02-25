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

#include <stout/error.hpp>
#include <stout/strings.hpp>

#include <stout/os/stat.hpp>

#include "common/lock.hpp"

#include "linux/fs.hpp"


namespace mesos {
namespace internal {
namespace fs {


bool MountTable::Entry::hasOption(const std::string& option) const
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
    return Error("Failed to open '" + path + "'");
  }

  while (true) {
#if defined(_BSD_SOURCE) || defined(_SVID_SOURCE)
    // Reentrant version exists.
    struct mntent mntentBuffer;
    char strBuffer[PATH_MAX];
    struct mntent* mntent =
      ::getmntent_r(file, &mntentBuffer, strBuffer, sizeof(strBuffer));
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
    // Mutex for guarding calls into non-reentrant mount table
    // functions. We use a static local variable to avoid unused
    // variable warnings.
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

    {
      Lock lock(&mutex);
      struct mntent* mntent = ::getmntent(file);
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
    }
#endif
  }

  ::endmntent(file);

  return table;
}


Try<FileSystemTable> FileSystemTable::read()
{
  // Mutex for guarding calls into non-reentrant fstab functions. We
  // use a static local variable to avoid unused variable warnings.
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

  FileSystemTable table;

  // Use locks since fstab functions are not thread-safe.
  {
    Lock lock(&mutex);

    // Open file _PATH_FSTAB (/etc/fstab).
    if (::setfsent() == 0) {
      return Error("Failed to open file system table");
    }

    while (true) {
      struct fstab* fstab = ::getfsent();
      if (fstab == NULL) {
        break; // NULL means the end of enties.
      }

      FileSystemTable::Entry entry(
          fstab->fs_spec,
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


Try<Nothing> mount(const Option<std::string>& source,
                   const std::string& target,
                   const Option<std::string>& type,
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
        (source.isSome() ? source.get().c_str() : NULL),
        target.c_str(),
        (type.isSome() ? type.get().c_str() : NULL),
        flags,
        data) < 0) {
    return ErrnoError();
  }

  return Nothing();
}


Try<Nothing> unmount(const std::string& target, int flags)
{
  // The prototype of function 'umount2' on Linux is as follows:
  // int umount2(const char *target, int flags);
  if (::umount2(target.c_str(), flags) < 0) {
    return ErrnoError("Failed to unmount '" + target + "'");
  }

  return Nothing();
}


Try<Nothing> pivot_root(
    const std::string& newRoot,
    const std::string& putOld)
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
#elif __x86_64__
  // A workaround for systems that have an old glib but have a new
  // kernel. The magic number '155' is the syscall number for
  // 'pivot_root' on the x86_64 architecture, see
  // arch/x86/syscalls/syscall_64.tbl
  int ret = ::syscall(155, newRoot.c_str(), putOld.c_str());
#else
#error "pivot_root is not available"
#endif
  if (ret == -1) {
    return ErrnoError();
  }

  return Nothing();
}


} // namespace fs {
} // namespace internal {
} // namespace mesos {
