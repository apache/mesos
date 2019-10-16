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

#ifndef __PROCESS_SSL_TLS_CONFIG_HPP__
#define __PROCESS_SSL_TLS_CONFIG_HPP__

#ifdef USE_SSL_SOCKET

#ifdef __WINDOWS__
// NOTE: This must be included before the OpenSSL headers as it includes
// `WinSock2.h` and `Windows.h` in the correct order.
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <openssl/ssl.h>

#include <stout/option.hpp>

namespace process {
namespace network {
namespace openssl {

struct TLSClientConfig {
  // Callback that will be called before the TLS handshake is started.
  typedef Try<Nothing> (*ConfigureSocketCallback)(
      SSL* ssl,
      const Address& peer,
      const Option<std::string>& servername);

  // Callback that will be called after the TLS handshake has been
  // completed successfully.
  typedef Try<Nothing> (*VerifyCallback)(
      const SSL* const ssl,
      const Option<std::string>& servername,
      const Option<net::IP>& ip);

  // The `ConfigureSocketCallback` and `VerifyCallback` arguments can be set
  // to nullptr, in that case they will not be called.
  TLSClientConfig(
      const Option<std::string>& servername,
      SSL_CTX *ctx,
      ConfigureSocketCallback,
      VerifyCallback);

  // Context from which the `SSL` object for this connection is created.
  SSL_CTX *ctx;

  // Server hostname to be used for hostname validation, if any.
  // This will be passed as the `servername` argument to both
  // callbacks.
  //
  // TODO(bevers): Use this for SNI as well when the linked OpenSSL
  // supports it.
  Option<std::string> servername;

  // User-specified callbacks.
  VerifyCallback verify;
  ConfigureSocketCallback configure_socket;
};


// Returns a `TLSClientConfig` object that is configured with the
// provided `servername` and the global libprocess SSL context. The
// callbacks `verify` and `configure_socket` are setup with a pair
// default functions that implement the SSL behaviour configured
// via the `LIBPROCESS_SSL_*` environment variables.
//
// NOTE: Callers must _NOT_ modify the `ctx` in the returned `TLSClientConfig`.
// Doing so would mutate global libprocess state.
//
// NOTE: The returned `ctx`, `verify` and `configure_socket` values all
// implement parts of the libprocess default behaviour and rely on each other
// for working correctly. It is not recommended to change one of them while
// keeping the others, unless you know *exactly* what you're doing.
//
// NOTE: The passed `servername` will be ignored and a reverse DNS lookup will
// be done instead if `LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME=legacy`.
TLSClientConfig create_tls_client_config(const Option<std::string>& servername);

} // namespace openssl {
} // namespace network {
} // namespace process {

#endif // USE_SSL_SOCKET

#endif // __PROCESS_SSL_TLS_CONFIG_HPP__
