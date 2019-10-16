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

#include <process/ssl/utilities.hpp>

#include <memory>
#include <string>
#include <vector>

#ifdef __WINDOWS__
// NOTE: This must be included before the OpenSSL headers as it includes
// `WinSock2.h` and `Windows.h` in the correct order.
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
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

using std::shared_ptr;
using std::string;
using std::vector;

Try<EVP_PKEY*> generate_private_rsa_key(int bits, unsigned long _exponent)
{
  // Allocate the in-memory structure for the private key.
  EVP_PKEY* private_key = EVP_PKEY_new();
  if (private_key == nullptr) {
    return Error("Failed to allocate key: EVP_PKEY_new");
  }

  // Allocate space for the exponent.
  BIGNUM* exponent = BN_new();
  if (exponent == nullptr) {
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
  if (rsa == nullptr) {
    BN_free(exponent);
    EVP_PKEY_free(private_key);
    return Error("Failed to allocate RSA: RSA_new");
  }

  // Generate the RSA key pair.
  if (RSA_generate_key_ex(rsa, bits, exponent, nullptr) != 1) {
    RSA_free(rsa);
    BN_free(exponent);
    EVP_PKEY_free(private_key);
    return Error(ERR_error_string(ERR_get_error(), nullptr));
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
    Option<string> hostname,
    const Option<net::IP>& ip)
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

    if (issuer_name.get() == nullptr) {
      return Error("Failed to get subject name of parent certificate: "
        "X509_get_subject_name");
    }
  }

  // Allocate the in-memory structure for the certificate.
  X509* x509 = X509_new();
  if (x509 == nullptr) {
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
  if (X509_gmtime_adj(X509_get_notBefore(x509), 0) == nullptr ||
      X509_gmtime_adj(X509_get_notAfter(x509),
                      60L * 60L * 24L * days) == nullptr) {
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
    const Try<string> _hostname = net::hostname();
    if (_hostname.isError()) {
      X509_free(x509);
      return Error("Failed to determine hostname");
    }

    hostname = _hostname.get();
  }

  // Grab the subject name of the new certificate.
  X509_NAME* name = X509_get_subject_name(x509);
  if (name == nullptr) {
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
          reinterpret_cast<const unsigned char*>(hostname->c_str()),
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

  if (ip.isSome()) {
    // Add an X509 extension with an IP for subject alternative name.

    STACK_OF(GENERAL_NAME)* alt_name_stack = sk_GENERAL_NAME_new_null();
    if (alt_name_stack == nullptr) {
      X509_free(x509);
      return Error("Failed to create a stack: sk_GENERAL_NAME_new_null");
    }

    GENERAL_NAME* alt_name = GENERAL_NAME_new();
    if (alt_name == nullptr) {
      sk_GENERAL_NAME_pop_free(alt_name_stack, GENERAL_NAME_free);
      X509_free(x509);
      return Error("Failed to create GENERAL_NAME: GENERAL_NAME_new");
    }

    alt_name->type = GEN_IPADD;

    ASN1_STRING* alt_name_str = ASN1_STRING_new();
    if (alt_name_str == nullptr) {
      GENERAL_NAME_free(alt_name);
      sk_GENERAL_NAME_pop_free(alt_name_stack, GENERAL_NAME_free);
      X509_free(x509);
      return Error("Failed to create alternative name: ASN1_STRING_new");
    }

    Try<in_addr> in = ip->in();

    if (in.isError()) {
      ASN1_STRING_free(alt_name_str);
      GENERAL_NAME_free(alt_name);
      sk_GENERAL_NAME_pop_free(alt_name_stack, GENERAL_NAME_free);
      X509_free(x509);
      return Error("Failed to get IP/4 address");
    }

#ifdef __WINDOWS__
    // cURL defines `in_addr_t` as `unsigned long` for Windows,
    // so we do too for consistency.
    typedef unsigned long in_addr_t;
#endif // __WINDOWS__

    // For `iPAddress` we hand over a binary value as part of the
    // specification.
    if (ASN1_STRING_set(alt_name_str, &in->s_addr, sizeof(in_addr_t)) == 0) {
      ASN1_STRING_free(alt_name_str);
      GENERAL_NAME_free(alt_name);
      sk_GENERAL_NAME_pop_free(alt_name_stack, GENERAL_NAME_free);
      X509_free(x509);
      return Error("Failed to set alternative name: ASN1_STRING_set");
    }

    // We are transferring ownership of 'alt_name_str` towards the
    // `ASN1_OCTET_STRING` here.
    alt_name->d.iPAddress = alt_name_str;

    // We try to transfer ownership of 'alt_name` towards the
    // `STACK_OF(GENERAL_NAME)` here.
    if (sk_GENERAL_NAME_push(alt_name_stack, alt_name) == 0) {
      GENERAL_NAME_free(alt_name);
      sk_GENERAL_NAME_pop_free(alt_name_stack, GENERAL_NAME_free);
      X509_free(x509);
      return Error("Failed to push alternative name: sk_GENERAL_NAME_push");
    }

    // We try to transfer the ownership of `alt_name_stack` towards the
    // `X509` here.
    if (X509_add1_ext_i2d(
            x509,
            NID_subject_alt_name,
            alt_name_stack,
            0,
            0) == 0) {
      sk_GENERAL_NAME_pop_free(alt_name_stack, GENERAL_NAME_free);
      X509_free(x509);
      return Error("Failed to set subject alternative name: X509_add1_ext_i2d");
    }

    sk_GENERAL_NAME_pop_free(alt_name_stack, GENERAL_NAME_free);
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
  FILE* file = fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return Error("Failed to open file '" + stringify(path) + "' for writing");
  }

  if (PEM_write_PrivateKey(
          file, private_key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
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
  FILE* file = fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
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


Try<string> generate_hmac_sha256(
  const string& message,
  const string& key)
{
  unsigned int md_len = 0;
  unsigned char buffer[EVP_MAX_MD_SIZE] = {0};

  unsigned char* rc = HMAC(
      EVP_sha256(),
      key.data(),
      key.size(),
      reinterpret_cast<const unsigned char*>(message.data()),
      message.size(),
      buffer,
      &md_len);

  if (rc == nullptr) {
    const char* reason = ERR_reason_error_string(ERR_get_error());

    return Error(
        "HMAC failed" + (reason == nullptr ? "" : ": " + string(reason)));
  }

  return string(reinterpret_cast<char*>(buffer), md_len);
}


template<typename Reader>
Try<shared_ptr<RSA>> pem_to_rsa(const string& pem, Reader reader)
{
  // We cast away constness from `pem`'s data since in older SSL versions
  // `BIO_new_mem_buf` took a non-const `char*` which was semantically `const`.
  BIO *bio = BIO_new_mem_buf(const_cast<char*>(pem.c_str()), -1);
  if (bio == nullptr) {
    return Error("Failed to create RSA key bio");
  }
  RSA *rsa = reader(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (rsa == nullptr) {
    return Error("Failed to create RSA from key bio");
  }
  return shared_ptr<RSA>(rsa, RSA_free);
}


Try<shared_ptr<RSA>> pem_to_rsa_private_key(const string& pem)
{
  return pem_to_rsa(pem, PEM_read_bio_RSAPrivateKey);
}


Try<shared_ptr<RSA>> pem_to_rsa_public_key(const string& pem)
{
  return pem_to_rsa(pem, PEM_read_bio_RSA_PUBKEY);
}


Try<string> sign_rsa_sha256(
    const string& message,
    shared_ptr<RSA> private_key)
{
  vector<unsigned char> signatureData;
  signatureData.reserve(RSA_size(private_key.get()));
  unsigned int signatureLength;
  unsigned char hash[SHA256_DIGEST_LENGTH];

  SHA256(
    reinterpret_cast<const unsigned char*>(message.c_str()),
    message.size(),
    hash);

  int success = RSA_sign(
    NID_sha256,
    hash,
    SHA256_DIGEST_LENGTH,
    signatureData.data(),
    &signatureLength,
    private_key.get());

  if (success == 0) {
    const char* reason = ERR_reason_error_string(ERR_get_error());
    return Error("Failed to sign the message" +
      (reason == nullptr ? "" : ": " + string(reason)));
  }

  return string(
    reinterpret_cast<char*>(signatureData.data()),
    signatureLength);
}


Try<Nothing> verify_rsa_sha256(
    const string& message,
    const string& signature,
    shared_ptr<RSA> public_key)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];

  SHA256(
    reinterpret_cast<const unsigned char*>(message.c_str()),
    message.size(),
    hash);

  int success = RSA_verify(
    NID_sha256,
    hash,
    SHA256_DIGEST_LENGTH,
    reinterpret_cast<const unsigned char*>(signature.data()),
    signature.size(),
    public_key.get());

  if (success == 0) {
    const char* reason = ERR_reason_error_string(ERR_get_error());
    return Error("Failed to verify message signature" +
      (reason == nullptr ? "" : ": " + string(reason)));
  }
  return Nothing();
}

} // namespace openssl {
} // namespace network {
} // namespace process {
