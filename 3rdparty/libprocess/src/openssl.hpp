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

#ifndef __OPENSSL_HPP__
#define __OPENSSL_HPP__

#ifdef __WINDOWS__
// NOTE: This must be included before the OpenSSL headers as it includes
// `WinSock2.h` and `Windows.h` in the correct order.
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <openssl/ssl.h>

#include <string>

#include <stout/ip.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <process/network.hpp>

#include <process/ssl/tls_config.hpp>

namespace process {
namespace network {
namespace openssl {

// Initializes the _global_ OpenSSL context (SSL_CTX) as well as the
// crypto library in order to support multi-threading. The global
// context gets initialized using the environment variables:
//
//    LIBPROCESS_SSL_ENABLED=(false|0,true|1)
//    LIBPROCESS_SSL_SUPPORT_DOWNGRADE=(false|0,true|1)
//    LIBPROCESS_SSL_CERT_FILE=(path to certificate)
//    LIBPROCESS_SSL_KEY_FILE=(path to key)
//    LIBPROCESS_SSL_VERIFY_CERT=(false|0,true|1)
//    LIBPROCESS_SSL_VERIFY_SERVER_CERT=(false|0,true|1)
//    LIBPROCESS_SSL_REQUIRE_CERT=(false|0,true|1)
//    LIBPROCESS_SSL_REQUIRE_CLIENT_CERT=(false|0,true|1)
//    LIBPROCESS_SSL_VERIFY_IPADD=(false|0,true|1)
//    LIBPROCESS_SSL_VERIFY_DEPTH=(4)
//    LIBPROCESS_SSL_CA_DIR=(path to CA directory)
//    LIBPROCESS_SSL_CA_FILE=(path to CA file)
//    LIBPROCESS_SSL_CIPHERS=(accepted ciphers separated by ':')
//    LIBPROCESS_SSL_ENABLE_SSL_V3=(false|0,true|1)
//    LIBPROCESS_SSL_ENABLE_TLS_V1_0=(false|0,true|1)
//    LIBPROCESS_SSL_ENABLE_TLS_V1_1=(false|0,true|1)
//    LIBPROCESS_SSL_ENABLE_TLS_V1_2=(false|0,true|1)
//    LIBPROCESS_SSL_ENABLE_TLS_V1_3=(false|0,true|1)
//    LIBPROCESS_SSL_ECDH_CURVES=(auto|list of curves separated by ':')
//
// TODO(benh): When/If we need to support multiple contexts in the
// same process, for example for Server Name Indication (SNI), then
// we'll add other functions for initializing an SSL_CTX based on
// these environment variables.
// TODO(nneilsen): Support certification revocation.
void initialize();

// Returns the _global_ OpenSSL context.
SSL_CTX* context();

// An enum to track whether a given SSL object is in client or server mode.
//
// TODO(bevers): Once the minimum supported OpenSSL version is at least 1.1.1,
// we can remove this enum and use the `SSL_is_server(ssl)` function instead.
enum class Mode {
  CLIENT,
  SERVER,
};

// Verify that the hostname is properly associated with the peer
// certificate associated with the specified SSL connection.
Try<Nothing> verify(
    const SSL* const ssl,
    Mode mode,
    const Option<std::string>& hostname = None(),
    const Option<net::IP>& ip = None());

// Callback for setting SSL options after the TCP connection was
// established but before the TLS handshake has started.
Try<Nothing> configure_socket(
    SSL* ssl,
    Mode mode,
    const Address& peer,
    const Option<std::string>& peer_hostname);

} // namespace openssl {
} // namespace network {
} // namespace process {

#endif // __OPENSSL_HPP__
