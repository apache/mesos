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
};

} // namespace internal {
} // namespace network {
} // namespace process {

#endif // __SSL_SOCKET_WRAPPER__
