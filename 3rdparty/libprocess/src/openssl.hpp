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

#include <openssl/ssl.h>

#include <string>

#include <stout/flags.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace process {
namespace network {
namespace openssl {

// Capture the environment variables that influence how we initialize
// the OpenSSL library via flags.
class Flags : public virtual flags::FlagsBase
{
public:
  Flags();

  bool enabled;
  bool support_downgrade;
  Option<std::string> cert_file;
  Option<std::string> key_file;
  bool verify_cert;
  bool require_cert;
  unsigned int verification_depth;
  Option<std::string> ca_dir;
  Option<std::string> ca_file;
  std::string ciphers;
  bool enable_ssl_v3;
  bool enable_tls_v1_0;
  bool enable_tls_v1_1;
  bool enable_tls_v1_2;
};

const Flags& flags();

// Initializes the _global_ OpenSSL context (SSL_CTX) as well as the
// crypto library in order to support multi-threading. The global
// context gets initialized using the environment variables:
//
//    SSL_ENABLED=(false|0,true|1)
//    SSL_SUPPORT_DOWNGRADE=(false|0,true|1)
//    SSL_CERT_FILE=(path to certificate)
//    SSL_KEY_FILE=(path to key)
//    SSL_VERIFY_CERT=(false|0,true|1)
//    SSL_REQUIRE_CERT=(false|0,true|1)
//    SSL_VERIFY_DEPTH=(4)
//    SSL_CA_DIR=(path to CA directory)
//    SSL_CA_FILE=(path to CA file)
//    SSL_CIPHERS=(accepted ciphers separated by ':')
//    SSL_ENABLE_SSL_V3=(false|0,true|1)
//    SSL_ENABLE_TLS_V1_0=(false|0,true|1)
//    SSL_ENABLE_TLS_V1_1=(false|0,true|1)
//    SSL_ENABLE_TLS_V1_2=(false|0,true|1)
//
// TODO(benh): When/If we need to support multiple contexts in the
// same process, for example for Server Name Indication (SNI), then
// we'll add other functions for initializing an SSL_CTX based on
// these environment variables.
// TODO(nneilsen): Support certification revocation.
void initialize();

// Returns the _global_ OpenSSL context.
SSL_CTX* context();

// Verify that the hostname is properly associated with the peer
// certificate associated with the specified SSL connection.
Try<Nothing> verify(const SSL* const ssl, const Option<std::string>& hostname);

} // namespace openssl {
} // namespace network {
} // namespace process {

#endif // __OPENSSL_HPP__
