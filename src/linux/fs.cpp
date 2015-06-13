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
#include <stout/numify.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/synchronized.hpp>

#include <stout/os/read.hpp>
#include <stout/os/stat.hpp>

#include "linux/fs.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace fs {


Try<MountInfoTable> MountInfoTable::read(const Option<pid_t>& pid)
{
  MountInfoTable table;

  const string path = path::join(
      "/proc",
      (pid.isSome() ? stringify(pid.get()) : "self"),
      "mountinfo");

  Try<string> lines = os::read(path);
  if (lines.isError()) {
    return Error("Failed to read mountinfo file: " + lines.error());
  }

  foreach (const string& line, strings::tokenize(lines.get(), "\n")) {
    Try<Entry> parse = MountInfoTable::Entry::parse(line);
    if (parse.isError()) {
      return Error("Failed to parse entry '" + line + "': " + parse.error());
    }

    table.entries.push_back(parse.get());
  }

  return table;
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
  std::vector<string> tokens = strings::tokenize(s.substr(0, pos), " ");
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
  std::vector<string> device = strings::split(tokens[2], ":");
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


bool MountTable::Entry::hasOption(const string& option) const
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


Try<MountTable> MountTable::read(const string& path)
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
    static std::mutex mutex;

    synchronized (mutex) {
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
  static std::mutex mutex;

  FileSystemTable table;

  // Use locks since fstab functions are not thread-safe.
  synchronized (mutex) {
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
        (source.isSome() ? source.get().c_str() : NULL),
        target.c_str(),
        (type.isSome() ? type.get().c_str() : NULL),
        flags,
        data) < 0) {
    return ErrnoError();
  }

  return Nothing();
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
