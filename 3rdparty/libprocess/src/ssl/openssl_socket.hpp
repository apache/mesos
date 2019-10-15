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

#ifndef __SSL_SOCKET_WRAPPER__
#define __SSL_SOCKET_WRAPPER__

#include <process/loop.hpp>
#include <process/once.hpp>
#include <process/queue.hpp>
#include <process/socket.hpp>

#include "poll_socket.hpp"

namespace process {
namespace network {
namespace internal {

class OpenSSLSocketImpl : public PollSocketImpl
{
public:
  // See 'Socket::create()'.
  static Try<std::shared_ptr<SocketImpl>> create(int_fd s);

  OpenSSLSocketImpl(int_fd _s);
  ~OpenSSLSocketImpl() override;

  // Implement 'SocketImpl' interface.
  Future<Nothing> connect(const Address& address) override;
  Future<Nothing> connect(
      const Address& address,
      const openssl::TLSClientConfig& config) override;

  Future<size_t> recv(char* data, size_t size) override;
  Future<size_t> send(const char* data, size_t size) override;
  Future<size_t> sendfile(int_fd fd, off_t offset, size_t size) override;
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

protected:
  // Takes ownership of the given SSL object and performs an SSL handshake
  // with the context of the SSL object. Either `SSL_set_connect_state`
  // or `SSL_set_accept_state` must be called on the context beforehand,
  // so that the handshake is done from the correct perspective.
  Future<size_t> set_ssl_and_do_handshake(SSL* _ssl);

  // Used to check the result of `SSL_do_handshake`, `SSL_read`,
  // or `SSL_write` in a `process::loop`.
  // `handle_as_read` should be set to `true` when this helper is called
  // from `SSL_read` to handle the EOF event differently. Our socket
  // API expects a return value of `0` when reading EOF, and a failure
  // otherwise.
  Future<ControlFlow<size_t>> handle_ssl_return_result(
      int result,
      bool handle_as_read);

private:
  SSL* ssl;
  Option<net::IP> peer_ip;
  Option<openssl::TLSClientConfig> client_config;

  Once accept_loop_started;

  // This queue stores accepted sockets that are considered connected
  // (either the SSL handshake has completed or the socket has been
  // downgraded). The 'accept()' call returns sockets from this queue.
  // We wrap the socket in a 'Future' so that we can pass failures or
  // discards through.
  Queue<Future<std::shared_ptr<SocketImpl>>> accept_queue;
};

} // namespace internal {
} // namespace network {
} // namespace process {

#endif // __SSL_SOCKET_WRAPPER__
