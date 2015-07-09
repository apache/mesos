/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#include <memory>

#include <process/socket.hpp>

#include <stout/try.hpp>

namespace process {
namespace network {

class PollSocketImpl : public Socket::Impl
{
public:
  static Try<std::shared_ptr<Socket::Impl>> create(int s);

  PollSocketImpl(int s) : Socket::Impl(s) {}

  virtual ~PollSocketImpl() {}

  // Implementation of the Socket::Impl interface.
  virtual Try<Nothing> listen(int backlog);
  virtual Future<Socket> accept();
  virtual Future<Nothing> connect(const Address& address);
  virtual Future<size_t> recv(char* data, size_t size);
  virtual Future<size_t> send(const char* data, size_t size);
  virtual Future<size_t> sendfile(int fd, off_t offset, size_t size);

  virtual Socket::Kind kind() const { return Socket::POLL; }
};

} // namespace network {
} // namespace process {
