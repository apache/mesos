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

#include <string>
#include <map>

#include <process/ssl/utilities.hpp>

#include <stout/try.hpp>

namespace process {
namespace http {
namespace authentication {

/**
 * Convert a JWK Set into a map of RSA key by kid.
 * @see <a href="https://tools.ietf.org/html/rfc7517"
 * @see <a href="https://tools.ietf.org/html/rfc7518"
 *
 * This implementation only supports 'RSA' key types for the moment.
 * It fails if any of the RSA keys in the key set is invalid.
 *
 * @param jwk The string representing the JWK Set containing keys to convert
 *   into RSA (public or private) keys.
 *
 * @return A map of RSA keys by kid if successful otherwise an Error.
 */
Try<std::map<std::string, network::openssl::RSA_shared_ptr>> jwkSetToRSAKeys(
    const std::string& jwk);

} // namespace authentication {
} // namespace http {
} // namespace process {
