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

#include <memory>

#include <process/socket.hpp>

#include <stout/try.hpp>

namespace process {
namespace network {
namespace internal {

class PollSocketImpl : public SocketImpl
{
public:
  static Try<std::shared_ptr<SocketImpl>> create(int_fd s);

  PollSocketImpl(int_fd s) : SocketImpl(s) {}

  virtual ~PollSocketImpl() {}

  // Implementation of the SocketImpl interface.
  virtual Try<Nothing> listen(int backlog);
  virtual Future<std::shared_ptr<SocketImpl>> accept();
  virtual Future<Nothing> connect(const Address& address);
  virtual Future<size_t> recv(char* data, size_t size);
  virtual Future<size_t> send(const char* data, size_t size);
  virtual Future<size_t> sendfile(int_fd fd, off_t offset, size_t size);
  virtual Kind kind() const { return SocketImpl::Kind::POLL; }
};

} // namespace internal {
} // namespace network {
} // namespace process {
