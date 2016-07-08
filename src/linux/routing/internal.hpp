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

#ifndef __LINUX_ROUTING_INTERNAL_HPP__
#define __LINUX_ROUTING_INTERNAL_HPP__

#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>

#include <memory>
#include <string>

#include <stout/error.hpp>
#include <stout/try.hpp>

namespace routing {

// Customized deallocation functions for netlink objects.
template <typename T>
void cleanup(T* t);


template <>
inline void cleanup(struct nl_cache* cache)
{
  nl_cache_free(cache);
}


template <>
inline void cleanup(struct nl_sock* sock)
{
  nl_socket_free(sock);
}


// A helper class for managing netlink objects (e.g., rtnl_link,
// nl_sock, etc.). It manages the life cycle of a netlink object. It
// is copyable and assignable, and multiple copies share the same
// underlying netlink object. A netlink object specific cleanup
// function will be invoked when the last copy of this wrapper is
// being deleted (similar to Future<T>). We use this class to simplify
// our code, especially for error handling.
template <typename T>
class Netlink
{
public:
  explicit Netlink(T* object) : data(new Data(object)) {}

  T* get() const { return data->object; }

private:
  struct Data
  {
    explicit Data(T* _object) : object(_object) {}

    ~Data()
    {
      if (object != nullptr) {
        cleanup(object);
      }
    }

    T* object;
  };

  std::shared_ptr<Data> data;
};


// Returns a netlink socket for communicating with the kernel. This
// socket is needed for most of the operations. The default protocol
// of the netlink socket is NETLINK_ROUTE, but you can optionally
// provide a different one.
// TODO(chzhcn): Consider renaming 'routing' to 'netlink'.
inline Try<Netlink<struct nl_sock>> socket(int protocol = NETLINK_ROUTE)
{
  struct nl_sock* s = nl_socket_alloc();
  if (s == nullptr) {
    return Error("Failed to allocate netlink socket");
  }

  Netlink<struct nl_sock> sock(s);

  int error = nl_connect(sock.get(), protocol);
  if (error != 0) {
    return Error(
        "Failed to connect to netlink protocol: " +
        std::string(nl_geterror(error)));
  }

  return sock;
}

} // namespace routing {

#endif // __LINUX_ROUTING_INTERNAL_HPP__
