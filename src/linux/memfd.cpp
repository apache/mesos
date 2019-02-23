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

#include <fcntl.h>
#include <unistd.h>

#include <sys/sendfile.h>
#include <sys/syscall.h>

#include <stout/stringify.hpp>

#include <stout/os/close.hpp>
#include <stout/os/open.hpp>
#include <stout/os/stat.hpp>

#include "linux/memfd.hpp"

using std::string;

#if !defined(F_ADD_SEALS)
#define F_ADD_SEALS 1033
#endif

#if !defined(F_GET_SEALS)
#define F_GET_SEALS 1034
#endif

#if !defined(F_SEAL_SEAL)
#define F_SEAL_SEAL   0x0001
#endif

#if !defined(F_SEAL_SHRINK)
#define F_SEAL_SHRINK 0x0002
#endif

#if !defined(F_SEAL_GROW)
#define F_SEAL_GROW   0x0004
#endif

#if !defined(F_SEAL_WRITE)
#define F_SEAL_WRITE  0x0008
#endif

namespace mesos {
namespace internal {
namespace memfd {

Try<int_fd> create(const string& name, unsigned int flags)
{
#ifdef __NR_memfd_create
  int_fd fd = ::syscall(__NR_memfd_create, name.c_str(), flags);
  if (fd == -1) {
    return ErrnoError("Failed to create memfd");
  }

  return fd;
#else
#error "The memfd syscall is not available."
#endif
}


Try<int_fd> cloneSealedFile(const std::string& filePath)
{
  if (!os::stat::isfile(filePath)) {
    return Error("The original file '" + filePath + "' is not a regular file");
  }

  Try<Bytes> size = os::stat::size(filePath);
  if (size.isError()) {
    return Error("Failed to get the size of the source file: " + size.error());
  }

  Try<int_fd> fileFd = os::open(filePath, O_CLOEXEC | O_RDONLY);
  if (fileFd.isError()) {
    return Error("Failed to open source file: " + fileFd.error());
  }

  Try<int_fd> memFd = create(filePath, MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (memFd.isError()) {
    os::close(fileFd.get());
    return Error("Failed to open memfd file: " + memFd.error());
  }

  size_t remaining = size->bytes();
  while (remaining > 0) {
    ssize_t written = sendfile(memFd.get(), fileFd.get(), nullptr, remaining);
    if (written == -1) {
      ErrnoError error("Failed to copy file");
      os::close(fileFd.get());
      os::close(memFd.get());
      return error;
    } else if (static_cast<size_t>(written) > remaining) {
      os::close(fileFd.get());
      os::close(memFd.get());
      return Error("More bytes written than requested");
    } else {
      remaining -= written;
    }
  }

  os::close(fileFd.get());

  int ret = fchmod(memFd.get(), S_IRWXU | S_IRWXG | S_IRWXO);
  if (ret == -1) {
    ErrnoError error("Failed to chmod");
    os::close(memFd.get());
    return error;
  }

  // Seal the memfd file.
  ret = fcntl(
      memFd.get(),
      F_ADD_SEALS,
      F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);

  if (ret == -1) {
    ErrnoError error("Failed to seal the memfd");
    os::close(memFd.get());
    return error;
  }

  return memFd.get();
}

} // namespace memfd {
} // namespace internal {
} // namespace mesos {
