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

#ifndef __PROCESS_SSL_FLAGS_HPP__
#define __PROCESS_SSL_FLAGS_HPP__

#ifdef USE_SSL_SOCKET

#include <string>

#include <stout/flags.hpp>
#include <stout/option.hpp>

namespace process {
namespace network {
namespace openssl {

/**
 * Defines the _global_ OpenSSL configuration loaded by libprocess.
 * These flags are captured from environment variables with the
 * prefix "LIBPROCESS_SSL_".
 */
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
  bool verify_ipadd;
  unsigned int verification_depth;
  Option<std::string> ca_dir;
  Option<std::string> ca_file;
  std::string ciphers;
  bool enable_ssl_v3;
  bool enable_tls_v1_0;
  bool enable_tls_v1_1;
  bool enable_tls_v1_2;
};


/**
 * Returns the _global_ OpenSSL configuration used by libprocess.
 */
const Flags& flags();

} // namespace openssl {
} // namespace network {
} // namespace process {

#endif // USE_SSL_SOCKET

#endif // __PROCESS_SSL_FLAGS_HPP__
