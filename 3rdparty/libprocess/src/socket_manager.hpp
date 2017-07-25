// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_SOCKET_MANAGER_HPP__
#define __PROCESS_SOCKET_MANAGER_HPP__

#include <mutex>
#include <queue>

#include <process/address.hpp>
#include <process/future.hpp>
#include <process/message.hpp>
#include <process/process.hpp>
#include <process/socket.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>

#include "encoder.hpp"

namespace process {

// Forward declaration.
class HttpProxy;


class SocketManager
{
public:
  SocketManager();
  ~SocketManager();

  // Closes all managed sockets and clears any associated metadata.
  // The `__s__` server socket must be closed and `ProcessManager`
  // must be finalized before calling this.
  void finalize();

  void accepted(const network::inet::Socket& socket);

  void link(
      ProcessBase* process,
      const UPID& to,
      const ProcessBase::RemoteConnection remote,
      const network::internal::SocketImpl::Kind& kind =
        network::internal::SocketImpl::DEFAULT_KIND());

  // Test-only method to fetch the file descriptor behind a
  // persistent socket.
  Option<int_fd> get_persistent_socket(const UPID& to);

  PID<HttpProxy> proxy(const network::inet::Socket& socket);

  // Used to clean up the pointer to an `HttpProxy` in case the
  // `HttpProxy` is killed outside the control of the `SocketManager`.
  // This generally happens when `process::finalize` is called.
  void unproxy(const network::inet::Socket& socket);

  void send(
      Encoder* encoder,
      bool persist,
      const network::inet::Socket& socket);

  void send(
      const http::Response& response,
      const http::Request& request,
      const network::inet::Socket& socket);

  void send(
      Message&& message,
      const network::internal::SocketImpl::Kind& kind =
        network::internal::SocketImpl::DEFAULT_KIND());

  Encoder* next(int_fd s);

  void close(int_fd s);

  void exited(const network::inet::Address& address);
  void exited(ProcessBase* process);

private:
  // TODO(bmahler): Leverage a bidirectional multimap instead, or
  // hide the complexity of manipulating 'links' through methods.
  struct
  {
    // For links, we maintain a bidirectional mapping between the
    // "linkers" (Processes) and the "linkees" (remote / local UPIDs).
    // For remote socket addresses, we also need a mapping to the
    // linkees for that socket address, because socket closure only
    // notifies at the address level.
    hashmap<UPID, hashset<ProcessBase*>> linkers;
    hashmap<ProcessBase*, hashset<UPID>> linkees;
    hashmap<network::inet::Address, hashset<UPID>> remotes;
  } links;

  // Switch the underlying socket that a remote end is talking to.
  // This manipulates the data structures below by swapping all data
  // mapped to 'from' to being mapped to 'to'. This is useful for
  // downgrading a socket from SSL to POLL based.
  void swap_implementing_socket(
      const network::inet::Socket& from,
      const network::inet::Socket& to);

  // Helper function for link().
  void link_connect(
      const Future<Nothing>& future,
      network::inet::Socket socket,
      const UPID& to);

  // Helper function for send().
  void send_connect(
      const Future<Nothing>& future,
      network::inet::Socket socket,
      Message&& message);

  // Collection of all active sockets (both inbound and outbound).
  hashmap<int_fd, network::inet::Socket> sockets;

  // Collection of sockets that should be disposed when they are
  // finished being used (e.g., when there is no more data to send on
  // them). Can contain both inbound and outbound sockets.
  hashset<int_fd> dispose;

  // Map from socket to socket address for outbound sockets.
  hashmap<int_fd, network::inet::Address> addresses;

  // Map from socket address to temporary sockets (outbound sockets
  // that will be closed once there is no more data to send on them).
  hashmap<network::inet::Address, int_fd> temps;

  // Map from socket address (ip, port) to persistent sockets
  // (outbound sockets that will remain open even if there is no more
  // data to send on them).  We distinguish these from the 'temps'
  // collection so we can tell when a persistent socket has been lost
  // (and thus generate ExitedEvents).
  hashmap<network::inet::Address, int_fd> persists;

  // Map from outbound socket to outgoing queue.
  hashmap<int_fd, std::queue<Encoder*>> outgoing;

  // HTTP proxies.
  hashmap<int_fd, HttpProxy*> proxies;

  // Protects instance variables.
  std::recursive_mutex mutex;
};


// Global instance of the socket manager.
extern SocketManager* socket_manager;

} // namespace process {

#endif // __PROCESS_SOCKET_MANAGER_HPP__
