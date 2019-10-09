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

#ifdef __WINDOWS__
// NOTE: This must be included before the OpenSSL headers as it includes
// `WinSock2.h` and `Windows.h` in the correct order.
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <process/socket.hpp>

#include <stout/unimplemented.hpp>

#include "openssl.hpp"
#include "ssl/openssl_socket.hpp"

namespace process {
namespace network {
namespace internal {

Try<std::shared_ptr<SocketImpl>> OpenSSLSocketImpl::create(int_fd s)
{
  UNIMPLEMENTED;
}


OpenSSLSocketImpl::OpenSSLSocketImpl(int_fd _s)
  : PollSocketImpl(_s) {}


OpenSSLSocketImpl::~OpenSSLSocketImpl()
{
  UNIMPLEMENTED;
}


Future<Nothing> OpenSSLSocketImpl::connect(
    const Address& address)
{
  LOG(FATAL) << "No TLS config was passed to a SSL socket.";
}


Future<Nothing> OpenSSLSocketImpl::connect(
    const Address& address,
    const openssl::TLSClientConfig& config)
{
  UNIMPLEMENTED;
}


Future<size_t> OpenSSLSocketImpl::recv(char* data, size_t size)
{
  UNIMPLEMENTED;
}


Future<size_t> OpenSSLSocketImpl::send(const char* data, size_t size)
{
  UNIMPLEMENTED;
}


Future<size_t> OpenSSLSocketImpl::sendfile(
    int_fd fd, off_t offset, size_t size)
{
  UNIMPLEMENTED;
}


Future<std::shared_ptr<SocketImpl>> OpenSSLSocketImpl::accept()
{
  UNIMPLEMENTED;
}


Try<Nothing, SocketError> OpenSSLSocketImpl::shutdown(int how)
{
  UNIMPLEMENTED;
}

} // namespace internal {
} // namespace network {
} // namespace process {
