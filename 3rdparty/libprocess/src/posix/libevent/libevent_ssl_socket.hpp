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

#ifndef __LIBEVENT_SSL_SOCKET_HPP__
#define __LIBEVENT_SSL_SOCKET_HPP__

#include <event2/buffer.h>
#include <event2/bufferevent_ssl.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>

#include <atomic>
#include <memory>

#include <process/queue.hpp>
#include <process/socket.hpp>

namespace process {
namespace network {
namespace internal {

class LibeventSSLSocketImpl : public SocketImpl
{
public:
  // See 'Socket::create()'.
  static Try<std::shared_ptr<SocketImpl>> create(int_fd s);

  LibeventSSLSocketImpl(int_fd _s);

  ~LibeventSSLSocketImpl() override;

  // Implement 'SocketImpl' interface.
  Future<Nothing> connect(const Address& address) override;
  Future<Nothing> connect(
      const Address& address,
      const openssl::TLSClientConfig& config) override;

  Future<size_t> recv(char* data, size_t size) override;
  // Send does not currently support discard. See implementation.
  Future<size_t> send(const char* data, size_t size) override;
  Future<size_t> sendfile(int_fd fd, off_t offset, size_t size) override;
  Try<Nothing> listen(int backlog) override;
  Future<std::shared_ptr<SocketImpl>> accept() override;
  SocketImpl::Kind kind() const override { return SocketImpl::Kind::SSL; }

  // Shuts down the socket.
  //
  // NOTE: Although this method accepts an integer which specifies the
  // shutdown mode, this parameter is ignored because SSL connections
  // do not have a concept of read/write-only shutdown. If either end
  // of the socket is closed, then the futures of any outstanding read
  // requests will be completed (possibly as failures).
  Try<Nothing, SocketError> shutdown(int how) override;

  // We need a post-initializer because 'shared_from_this()' is not
  // valid until the constructor has finished.
  void initialize();

private:
  // A set of helper functions that transitions an accepted socket to
  // an SSL connected socket. With the libevent-openssl library, once
  // we return from the 'accept_callback()' which is scheduled by
  // 'listen' then we still need to wait for the 'BEV_EVENT_CONNECTED'
  // state before we know the SSL connection has been established.
  struct AcceptRequest
  {
    AcceptRequest(
        int_fd _socket,
        evconnlistener* _listener,
        const Address& _address)
      : peek_event(nullptr),
        listener(_listener),
        socket(_socket),
        address(_address) {}
    event* peek_event;
    Promise<std::shared_ptr<SocketImpl>> promise;
    evconnlistener* listener;
    int_fd socket;
    Address address;
  };

  struct RecvRequest
  {
    RecvRequest(char* _data, size_t _size)
      : data(_data), size(_size) {}
    Promise<size_t> promise;
    char* data;
    size_t size;
  };

  struct SendRequest
  {
    SendRequest(size_t _size)
      : size(_size) {}
    Promise<size_t> promise;
    size_t size;
  };

  struct ConnectRequest
  {
    Promise<Nothing> promise;
  };

  // This is a private constructor used by the accept helper
  // functions.
  LibeventSSLSocketImpl(
      int_fd _s,
      bufferevent* bev);

  // This is called when the equivalent of 'accept' returns. The role
  // of this function is to set up the SSL object and bev. If we
  // support both SSL and non-SSL traffic simultaneously then we first
  // wait for data to be ready and test the hello handshake to
  // disambiguate between the kinds of traffic.
  void accept_callback(AcceptRequest* request);

  // This is the continuation of 'accept_callback' that handles an SSL
  // connection.
  static void accept_SSL_callback(AcceptRequest* request);

  // This function peeks at the data on an accepted socket to see if
  // there is an SSL handshake or not. It then dispatches to the
  // SSL handling function or creates a non-SSL socket.
  static void peek_callback(evutil_socket_t fd, short what, void* arg);

  // The following are function pairs of static functions to member
  // functions. The static functions test and hold the weak pointer to
  // the socket before calling the member functions. This protects
  // against the socket being destroyed before the event loop calls
  // the callbacks.
  static void recv_callback(bufferevent* bev, void* arg);
  void recv_callback();

  static void send_callback(bufferevent* bev, void* arg);
  void send_callback();

  static void event_callback(bufferevent* bev, short events, void* arg);
  void event_callback(short events);

  bufferevent* bev;

  evconnlistener* listener;

  // Protects the following instance variables.
  std::atomic_flag lock = ATOMIC_FLAG_INIT;
  Owned<RecvRequest> recv_request;
  Owned<SendRequest> send_request;
  Owned<ConnectRequest> connect_request;

  // Indicates whether or not an EOF has been received on this socket.
  // Our accesses to this member are not synchronized because they all
  // occur within the event loop, which runs on a single thread.
  bool received_eof = false;

  // This is a weak pointer to 'this', i.e., ourselves, this class
  // instance. We need this for our event loop callbacks because it's
  // possible that we'll actually want to cleanup this socket impl
  // before the event loop callback gets executed ... and we'll check
  // in each event loop callback whether or not this weak_ptr is valid
  // by attempting to upgrade it to shared_ptr. It is the
  // responsibility of the event loop through the deferred lambda in
  // the destructor to clean up this pointer.
  // 1) It is a 'weak_ptr' as opposed to a 'shared_ptr' because we
  // want to test whether the object is still around from within the
  // event loop. If it was a 'shared_ptr' then we would be
  // contributing to the lifetime of the object and would no longer be
  // able to test the lifetime correctly.
  // 2) This is a pointer to a 'weak_ptr' so that we can pass this
  // through to the event loop through the C-interface. We need access
  // to the 'weak_ptr' from outside the object (in the event loop) to
  // test if the object is still alive. By maintaining this 'weak_ptr'
  // on the heap we can be sure it is safe to access from the
  // event loop until it is destroyed.
  std::weak_ptr<LibeventSSLSocketImpl>* event_loop_handle;

  // This queue stores accepted sockets that are considered connected
  // (either the SSL handshake has completed or the socket has been
  // downgraded). The 'accept()' call returns sockets from this queue.
  // We wrap the socket in a 'Future' so that we can pass failures or
  // discards through.
  Queue<Future<std::shared_ptr<SocketImpl>>> accept_queue;

  Option<net::IP> peer_ip;
  Option<openssl::TLSClientConfig> client_config;
};

} // namespace internal {
} // namespace network {
} // namespace process {

#endif // __LIBEVENT_SSL_SOCKET_HPP__
