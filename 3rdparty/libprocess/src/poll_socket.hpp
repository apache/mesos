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

#ifndef __PROCESS_POLL_SOCKET__
#define __PROCESS_POLL_SOCKET__
#include <memory>

#include <process/socket.hpp>

#include <process/ssl/tls_config.hpp>

#include <stout/try.hpp>

namespace process {
namespace network {
namespace internal {

class PollSocketImpl : public SocketImpl
{
public:
  static Try<std::shared_ptr<SocketImpl>> create(int_fd s);

  PollSocketImpl(int_fd s) : SocketImpl(s) {}

  ~PollSocketImpl() override {}

  // Implementation of the SocketImpl interface.
  Try<Nothing> listen(int backlog) override;
  Future<std::shared_ptr<SocketImpl>> accept() override;
  Future<Nothing> connect(
      const Address& address) override;
#ifdef USE_SSL_SOCKET
  Future<Nothing> connect(
      const Address& address,
      const openssl::TLSClientConfig& config) override;
#endif
  Future<size_t> recv(char* data, size_t size) override;
  Future<size_t> send(const char* data, size_t size) override;
  Future<size_t> sendfile(int_fd fd, off_t offset, size_t size) override;
  Kind kind() const override { return SocketImpl::Kind::POLL; }
};

} // namespace internal {
} // namespace network {
} // namespace process {

#endif // __PROCESS_POLL_SOCKET__
