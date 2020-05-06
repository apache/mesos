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

#ifndef __COMMON_DOMAIN_SOCKETS_HPP__
#define __COMMON_DOMAIN_SOCKETS_HPP__

#include <unistd.h>  // unlink()

#include <process/socket.hpp>

#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os/open.hpp>
#include <stout/posix/os.hpp>  // chmod()
#include <stout/try.hpp>


namespace mesos {
namespace internal {
namespace common {

constexpr size_t DOMAIN_SOCKET_MAX_PATH_LENGTH = 108;
constexpr int DOMAIN_SOCKET_DEFAULT_MODE = 0666;


inline Try<process::network::unix::Socket> createDomainSocket(
    const std::string& path,
    int mode = DOMAIN_SOCKET_DEFAULT_MODE)
{
  // If the file exists, and it already is a socket, we assume that it is
  // left over from a previous run and unlink it before creating a new socket.
  //
  // Note that existing connections using this socket will not be interrupted
  // by deleting the socket file.
  //
  // Note also that if the file exists and is not a socket, we return an Error
  // when we try to `bind()`.
  if (os::stat::issocket(path)) {
    // TODO(bevers): Move to `os::unlink()`.
    LOG(INFO) << "Removing existing socket at " << path;
    ::unlink(path.c_str());
  }

  Try<process::network::unix::Socket> socket =
    process::network::unix::Socket::create();

  if (socket.isError()) {
    return Error(
        "Failed to create unix domain socket: " + socket.error());
  }

  Try<process::network::unix::Address> addr =
    process::network::unix::Address::create(path);

  if (addr.isError()) {
    return Error(
        "Failed to parse path " + path + ": " + addr.error());
  }

  Try<process::network::unix::Address> bound = socket->bind(addr.get());
  if (bound.isError()) {
    return Error(
        "Failed to bind domain socket to path " + path + ": " +
        bound.error());
  }

  Try<Nothing> chmod = os::chmod(path, mode);
  if (chmod.isError()) {
    return Error("Couldn't change domain socket permissions: " +
                 chmod.error());
  }

  return socket;
}

} // namespace common {
} // namespace internal {
} // namespace mesos {

#endif // __COMMON_DOMAIN_SOCKETS_HPP__
