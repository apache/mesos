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

#include <process/ssl/utilities.hpp>

#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <stout/check.hpp>
#include <stout/net.hpp>
#include <stout/stringify.hpp>

// TODO(jmlvanre): Add higher level abstractions for key and
// certificate generation.

namespace process {
namespace network {
namespace openssl {

Try<EVP_PKEY*> generate_private_rsa_key(int bits, unsigned long _exponent)
{
  // Allocate the in-memory structure for the private key.
  EVP_PKEY* private_key = EVP_PKEY_new();
  if (private_key == NULL) {
    return Error("Failed to allocate key: EVP_PKEY_new");
  }

  // Allocate space for the exponent.
  BIGNUM* exponent = BN_new();
  if (exponent == NULL) {
    EVP_PKEY_free(private_key);
    return Error("Failed to allocate exponent: BN_new");
  }

  // Assign the exponent.
  if (BN_set_word(exponent, _exponent) != 1) {
    BN_free(exponent);
    EVP_PKEY_free(private_key);
    return Error("Failed to set exponent: BN_set_word");
  }

  // Allocate the in-memory structure for the key pair.
  RSA* rsa = RSA_new();
  if (rsa == NULL) {
    BN_free(exponent);
    EVP_PKEY_free(private_key);
    return Error("Failed to allocate RSA: RSA_new");
  }

  // Generate the RSA key pair.
  if (RSA_generate_key_ex(rsa, bits, exponent, NULL) != 1) {
    RSA_free(rsa);
    BN_free(exponent);
    EVP_PKEY_free(private_key);
    return Error(ERR_error_string(ERR_get_error(), NULL));
  }

  // We no longer need the exponent, so let's free it.
  BN_free(exponent);

  // Associate the RSA key with the private key. If this association
  // is successful, then the RSA key will be freed when the private
  // key is freed.
  if (EVP_PKEY_assign_RSA(private_key, rsa) != 1) {
    RSA_free(rsa);
    EVP_PKEY_free(private_key);
    return Error("Failed to assign RSA key: EVP_PKEY_assign_RSA");
  }

  return private_key;
}


Try<X509*> generate_x509(
    EVP_PKEY* subject_key,
    EVP_PKEY* sign_key,
    const Option<X509*>& parent_certificate,
    int serial,
    int days,
    Option<std::string> hostname)
{
  Option<X509_NAME*> issuer_name = None();
  if (parent_certificate.isNone()) {
    // If there is no parent certificate, then the subject and
    // signing key must be the same.
    if (subject_key != sign_key) {
      return Error("Subject vs signing key mismatch");
    }
  } else {
    // If there is a parent certificate, then set the issuer name to
    // be that of the parent.
    issuer_name = X509_get_subject_name(parent_certificate.get());

    if (issuer_name.get() == NULL) {
      return Error("Failed to get subject name of parent certificate: "
        "X509_get_subject_name");
    }
  }

  // Allocate the in-memory structure for the certificate.
  X509* x509 = X509_new();
  if (x509 == NULL) {
    return Error("Failed to allocate certification: X509_new");
  }

  // Set the version to V3.
  if (X509_set_version(x509, 2) != 1) {
    X509_free(x509);
    return Error("Failed to set version: X509_set_version");
  }

  // Set the serial number.
  if (ASN1_INTEGER_set(X509_get_serialNumber(x509), serial) != 1) {
    X509_free(x509);
    return Error("Failed to set serial number: ASN1_INTEGER_set");
  }

  // Make this certificate valid for 'days' number of days from now.
  if (X509_gmtime_adj(X509_get_notBefore(x509), 0) == NULL ||
      X509_gmtime_adj(X509_get_notAfter(x509),
                      60L * 60L * 24L * days) == NULL) {
    X509_free(x509);
    return Error("Failed to set valid days of certificate: X509_gmtime_adj");
  }

  // Set the public key for our certificate based on the subject key.
  if (X509_set_pubkey(x509, subject_key) != 1) {
    X509_free(x509);
    return Error("Failed to set public key: X509_set_pubkey");
  }

  // Figure out our hostname if one was not provided.
  if (hostname.isNone()) {
    const Try<std::string> _hostname = net::hostname();
    if (_hostname.isError()) {
      X509_free(x509);
      return Error("Failed to determine hostname");
    }

    hostname = _hostname.get();
  }

  // Grab the subject name of the new certificate.
  X509_NAME* name = X509_get_subject_name(x509);
  if (name == NULL) {
    X509_free(x509);
    return Error("Failed to get subject name: X509_get_subject_name");
  }

  // Set the country code, organization, and common name.
  if (X509_NAME_add_entry_by_txt(
          name,
          "C",
          MBSTRING_ASC,
          reinterpret_cast<const unsigned char*>("US"),
          -1,
          -1,
          0) != 1) {
    X509_free(x509);
    return Error("Failed to set country code: X509_NAME_add_entry_by_txt");
  }

  if (X509_NAME_add_entry_by_txt(
          name,
          "O",
          MBSTRING_ASC,
          reinterpret_cast<const unsigned char*>("Test"),
          -1,
          -1,
          0) != 1) {
    X509_free(x509);
    return Error("Failed to set organization name: X509_NAME_add_entry_by_txt");
  }

  if (X509_NAME_add_entry_by_txt(
          name,
          "CN",
          MBSTRING_ASC,
          reinterpret_cast<const unsigned char*>(hostname.get().c_str()),
          -1,
          -1,
          0) != 1) {
    X509_free(x509);
    return Error("Failed to set common name: X509_NAME_add_entry_by_txt");
  }

  // Set the issuer name to be the same as the subject if it is not
  // already set (this is a self-signed certificate).
  if (issuer_name.isNone()) {
    issuer_name = name;
  }

  CHECK_SOME(issuer_name);
  if (X509_set_issuer_name(x509, issuer_name.get()) != 1) {
    X509_free(x509);
    return Error("Failed to set issuer name: X509_set_issuer_name");
  }

  // Sign the certificate with the sign key.
  if (X509_sign(x509, sign_key, EVP_sha1()) == 0) {
    X509_free(x509);
    return Error("Failed to sign certificate: X509_sign");
  }

  return x509;
}


Try<Nothing> write_key_file(EVP_PKEY* private_key, const Path& path)
{
  // We use 'FILE*' here because it is an API requirement by openssl.
  FILE* file = fopen(path.value.c_str(), "wb");
  if (file == NULL) {
    return Error("Failed to open file '" + stringify(path) + "' for writing");
  }

  if (PEM_write_PrivateKey(file, private_key, NULL, NULL, 0, NULL, NULL) != 1) {
    fclose(file);
    return Error("Failed to write private key to file '" + stringify(path) +
      "': PEM_write_PrivateKey");
  }

  fclose(file);

  return Nothing();
}


Try<Nothing> write_certificate_file(X509* x509, const Path& path)
{
  // We use 'FILE*' here because it is an API requirement by openssl.
  FILE* file = fopen(path.value.c_str(), "wb");
  if (file == NULL) {
    return Error("Failed to open file '" + stringify(path) + "' for writing");
  }

  if (PEM_write_X509(file, x509) != 1) {
    fclose(file);
    return Error("Failed to write certificate to file '" + stringify(path) +
      "': PEM_write_X509");
  }

  fclose(file);

  return Nothing();
}

} // namespace openssl {
} // namespace network {
} // namespace process {
