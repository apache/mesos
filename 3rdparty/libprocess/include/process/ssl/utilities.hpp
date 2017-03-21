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

#ifndef __OPENSSL_UTIL_HPP__
#define __OPENSSL_UTIL_HPP__

#ifdef USE_SSL_SOCKET

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <stout/ip.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

namespace process {
namespace network {
namespace openssl {

/**
 * Generates an RSA key.
 *
 * The caller is responsible for calling `EVP_PKEY_free` on the
 * returned EVP_PKEY.
 * @see <a href="https://www.openssl.org/docs/crypto/RSA_generate_key.html">RSA_generate_key_ex</a> // NOLINT
 *
 * @param bits The modulus size used to generate the key.
 * @param exponent The public exponent, an odd number.
 *
 * @return A pointer to an EVP_PKEY if successful otherwise an Error.
 */
Try<EVP_PKEY*> generate_private_rsa_key(
    int bits = 2048,
    unsigned long exponent = RSA_F4);


/**
 * Generates an X509 certificate.
 *
 * The X509 certificate is generated for the @param subject_key and
 * signed by the @param sign_key. The caller is responsible for
 * calling `X509_free` on the returned X509 object.
 * The common name of the certificate will be set to @param hostname
 * or that of the localhost if @param hostname is not provided.
 * If a @param parent_certificate is provided, then the issuer name of
 * the certificate will be set to the subject name of the
 * @param parent_certificate. Otherwise, it is assumed this is a
 * self-signed certificate in which case the @param subject_key must
 * be the same as the @param sign_key, and the issuer name will be the
 * same as the subject name. If @param ip is provided, then the
 * certificate will use the ip for a subject alternative name iPAddress
 * extension.
 *
 * @param subject_key The key that will be made public by the
 *     certificate.
 * @param sign_key The private key used to sign the certificate.
 * @param parent_certificate An optional parent certificate that will
 *     be used to set the issuer name.
 * @param serial The serial number of the certificate.
 * @param days The number of days from the current time that the
 *     certificate will be valid.
 * @param hostname An optional hostname used to set the common name of
 *     the certificate.
 * @param ip An optional IP used to set the subject alternative name
 *     iPAddress of the certificate extension.
 *
 * @return A pointer to an X509 certificate if successful otherwise an
 *     Error.
 */
Try<X509*> generate_x509(
    EVP_PKEY* subject_key,
    EVP_PKEY* sign_key,
    const Option<X509*>& parent_certificate = None(),
    int serial = 1,
    int days = 365,
    Option<std::string> hostname = None(),
    const Option<net::IP>& ip = None());


/**
 * Writes a private key (EVP_PKEY) to a file on disk.
 * @see <a href="https://www.openssl.org/docs/crypto/pem.html">PEM_write_PrivateKey</a> // NOLINT
 *
 * @param private_key The private key to write.
 * @param path The file location to create the file.
 *
 * @return Nothing if successful otherwise an Error.
 */
Try<Nothing> write_key_file(EVP_PKEY* private_key, const Path& path);


/**
 * Writes an X509 certificate (X509) to a file on disk.
 * @see <a href="https://www.openssl.org/docs/crypto/pem.html">PEM_write_X509</a> // NOLINT
 *
 * @param x509 The certificate to write.
 * @param path The file location to create the file.
 *
 * @return Nothing if successful otherwise an Error.
 */
Try<Nothing> write_certificate_file(X509* x509, const Path& path);


/**
 * Generates a keyed-hash message authentication code (HMAC) with SHA256.
 * @see <a href="https://www.openssl.org/docs/man1.1.0/crypto/HMAC.html">HMAC</a> // NOLINT
 *
 * @param message The message to be authenticated.
 * @param key The secret key.
 *
 * @return The HMAC if successful otherwise an Error.
 */
Try<std::string> generate_hmac_sha256(
    const std::string& message,
    const std::string& key);

} // namespace openssl {
} // namespace network {
} // namespace process {

#endif // USE_SSL_SOCKET

#endif // __OPENSSL_UTIL_HPP__
