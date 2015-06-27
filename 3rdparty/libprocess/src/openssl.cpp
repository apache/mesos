#include "openssl.hpp"

#include <sys/param.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <mutex>
#include <string>

#include <process/once.hpp>

#include <stout/flags.hpp>

using std::ostringstream;
using std::string;

// Must be defined by us for OpenSSL in order to capture the necessary
// data for doing locking. Note, this needs to be defined in the
// global namespace as well.
struct CRYPTO_dynlock_value
{
  std::mutex mutex;
};


namespace process {
namespace network {
namespace openssl {

// _Global_ OpenSSL context, initialized via 'initialize'.
static SSL_CTX* ctx = NULL;


Flags::Flags()
{
  add(&Flags::enabled,
      "enabled",
      "Whether SSL is enabled.",
      false);

  add(&Flags::cert_file,
      "cert_file",
      "Path to certifcate.");

  add(&Flags::key_file,
      "key_file",
      "Path to key.");

  add(&Flags::verify_cert,
      "verify_cert",
      "Whether or not to verify peer certificates.",
      false);

  add(&Flags::require_cert,
      "require_cert",
      "Whether or not to require peer certificates. Requiring a peer "
      "certificate implies verifying it.",
      false);

  add(&Flags::verification_depth,
      "verification_depth",
      "Maximum depth for the certificate chain verification that shall be "
      "allowed.",
      4);

  add(&Flags::ca_dir,
      "ca_dir",
      "Path to certifcate authority (CA) directory.");

  add(&Flags::ca_file,
      "ca_file",
      "Path to certifcate authority (CA) file.");

  add(&Flags::ciphers,
      "ciphers",
      "Cryptographic ciphers to use.",
      // Default TLSv1 ciphers chosen based on Amazon's security
      // policy, see:
      // http://docs.aws.amazon.com/ElasticLoadBalancing/latest/
      // DeveloperGuide/elb-security-policy-table.html
      "AES128-SHA:AES256-SHA:RC4-SHA:DHE-RSA-AES128-SHA:DHE-DSS-AES128-SHA:"
      "DHE-RSA-AES256-SHA:DHE-DSS-AES256-SHA");

  add(&Flags::enable_ssl_v2,
      "enable_ssl_v2",
      "Enable SSLV2.",
      false);

  add(&Flags::enable_ssl_v3,
      "enable_ssl_v3",
      "Enable SSLV3.",
      false);

  add(&Flags::enable_tls_v1_0,
      "enable_tls_v1_0",
      "Enable SSLV1.0.",
      false);

  add(&Flags::enable_tls_v1_1,
      "enable_tls_v1_1",
      "Enable SSLV1.1.",
      false);

  add(&Flags::enable_tls_v1_2,
      "enable_tls_v1_2",
      "Enable SSLV1.2.",
      true);
}


static Flags* ssl_flags = new Flags();


const Flags& flags()
{
  openssl::initialize();
  return *ssl_flags;
}


// Mutexes necessary to support OpenSSL locking on shared data
// structures. See 'locking_function' for more information.
static std::mutex* mutexes = NULL;


// Callback needed to perform locking on shared data structures. From
// the OpenSSL documentation:
//
// OpenSSL uses a number of global data structures that will be
// implicitly shared whenever multiple threads use OpenSSL.
// Multi-threaded applications will crash at random if [the locking
// function] is not set.
void locking_function(int mode, int n, const char* /*file*/, int /*line*/)
{
  if (mode & CRYPTO_LOCK) {
    mutexes[n].lock();
  } else {
    mutexes[n].unlock();
  }
}


// OpenSSL callback that returns the current thread ID, necessary for
// OpenSSL threading.
unsigned long id_function()
{
  pthread_t pthread = pthread_self();
#ifdef __APPLE__
  mach_port_t id = pthread_mach_thread_np(pthread);
#else
  pthread_t id = pthread;
#endif // __APPLE__
  return static_cast<unsigned long>(id);
}


// OpenSSL callback for creating new dynamic "locks", abstracted by
// the CRYPTO_dynlock_value structure.
CRYPTO_dynlock_value* dyn_create_function(const char* /*file*/, int /*line*/)
{
  CRYPTO_dynlock_value* value = new CRYPTO_dynlock_value();

  if (value == NULL) {
    return NULL;
  }

  return value;
}


// OpenSSL callback for locking and unlocking dynamic "locks",
// abstracted by the CRYPTO_dynlock_value structure.
void dyn_lock_function(
    int mode,
    CRYPTO_dynlock_value* value,
    const char* /*file*/,
    int /*line*/)
{
  if (mode & CRYPTO_LOCK) {
    value->mutex.lock();
  } else {
    value->mutex.unlock();
  }
}


// OpenSSL callback for destroying dynamic "locks", abstracted by the
// CRYPTO_dynlock_value structure.
void dyn_destroy_function(
    CRYPTO_dynlock_value* value,
    const char* /*file*/,
    int /*line*/)
{
  delete value;
}


// Callback for OpenSSL peer certificate verification.
int verify_callback(int ok, X509_STORE_CTX* store)
{
  if (ok != 1) {
    // Construct and log a warning message.
    ostringstream message;

    X509* cert = X509_STORE_CTX_get_current_cert(store);
    int error = X509_STORE_CTX_get_error(store);
    int depth = X509_STORE_CTX_get_error_depth(store);

    message << "Error with certificate at depth: " << stringify(depth) << "\n";

    char buffer[256] {};

    // TODO(jmlvanre): use X509_NAME_print_ex instead.
    X509_NAME_oneline(X509_get_issuer_name(cert), buffer, sizeof(buffer) - 1);

    message << "Issuer: " << stringify(buffer) << "\n";

    // TODO(jmlvanre): use X509_NAME_print_ex instead.
    memset(buffer, 0, sizeof(buffer));
    X509_NAME_oneline(X509_get_subject_name(cert), buffer, sizeof(buffer) - 1);

    message << "Subject: " << stringify(buffer) << "\n";

    message << "Error (" << stringify(error) << "): " <<
      stringify(X509_verify_cert_error_string(error));

    LOG(WARNING) << message.str();
  }

  return ok;
}


string error_string(unsigned long code)
{
  // SSL library guarantees to stay within 120 bytes.
  char buffer[128];

  ERR_error_string_n(code, buffer, sizeof(buffer));
  string s(buffer);

  if (code == SSL_ERROR_SYSCALL) {
    s += error_string(ERR_get_error());
  }

  return s;
}


// Tests can declare this function and use it to re-configure the SSL
// environment variables programatically. Without explicitly declaring
// this function, it is not visible. This is the preferred behavior as
// we do not want applications changing these settings while they are
// running (this would be undefined behavior).
void reinitialize()
{
  // Load all the flags prefixed by SSL_ from the environment. See
  // comment at top of openssl.hpp for a full list.
  Try<Nothing> load = ssl_flags->load("SSL_");
  if (load.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to load flags from environment variables (prefixed by SSL_):"
      << load.error();
  }

  // Exit early if SSL is not enabled.
  if (!ssl_flags->enabled) {
    return;
  }

  static Once* initialized_single_entry = new Once();

  // We don't want to initialize everything multiple times, as we
  // don't clean up some of these structures. The things we DO tend
  // to re-initialize are things that are overwrites of settings,
  // rather than allocations of new data structures.
  if (!initialized_single_entry->once()) {
    // We MUST have entropy, or else there's no point to crypto.
    if (!RAND_poll()) {
      EXIT(EXIT_FAILURE) << "SSL socket requires entropy";
    }

    // Initialize the OpenSSL library.
    SSL_library_init();
    SSL_load_error_strings();

    // Prepare mutexes for threading callbacks.
    mutexes = new std::mutex[CRYPTO_num_locks()];

    // Install SSL threading callbacks.
    // TODO(jmlvanre): the id mechanism is deprecated in OpenSSL.
    CRYPTO_set_id_callback(&id_function);
    CRYPTO_set_locking_callback(&locking_function);
    CRYPTO_set_dynlock_create_callback(&dyn_create_function);
    CRYPTO_set_dynlock_lock_callback(&dyn_lock_function);
    CRYPTO_set_dynlock_destroy_callback(&dyn_destroy_function);

    ctx = SSL_CTX_new(SSLv23_method());
    CHECK(ctx) << "Failed to create SSL context: "
                << ERR_error_string(ERR_get_error(), NULL);

    initialized_single_entry->done();
  }

  // Disable SSL session caching.
  SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);

  // Set a session id to avoid connection termination upon
  // re-connect. We can use something more relevant when we care
  // about session caching.
  const uint64_t session_ctx = 7;

  const unsigned char* session_id =
    reinterpret_cast<const unsigned char*>(&session_ctx);

  if (SSL_CTX_set_session_id_context(
          ctx,
          session_id,
          sizeof(session_ctx)) != 1) {
    LOG(FATAL) << "Session id context size exceeds maximum";
  }

  // Now do some validation of the flags/environment variables.
  if (ssl_flags->key_file.isNone()) {
    EXIT(EXIT_FAILURE) << "SSL requires key! NOTE: Set path with SSL_KEY_FILE";
  }

  if (ssl_flags->cert_file.isNone()) {
    EXIT(EXIT_FAILURE)
      << "SSL requires certificate! NOTE: Set path with SSL_CERT_FILE";
  }

  if (ssl_flags->ca_file.isNone()) {
    VLOG(2) << "CA file path is unspecified! NOTE: "
            << "Set CA file path with SSL_CA_FILE=<filepath>";
  }

  if (ssl_flags->ca_dir.isNone()) {
    VLOG(2) << "CA directory path unspecified! NOTE: "
            << "Set CA directory path with SSL_CA_DIR=<dirpath>";
  }

  if (!ssl_flags->verify_cert) {
    VLOG(2) << "Will not verify peer certificate!\n"
            << "NOTE: Set SSL_VERIFY_CERT=1 to enable peer certificate "
            << "verification";
  }

  if (!ssl_flags->require_cert) {
    VLOG(2) << "Will only verify peer certificate if presented!\n"
            << "NOTE: Set SSL_REQUIRE_CERT=1 to require peer certificate "
            << "verification";
  }

  if (ssl_flags->require_cert && !ssl_flags->verify_cert) {
    // Requiring a certificate implies that is should be verified.
    ssl_flags->verify_cert = true;

    VLOG(2) << "SSL_REQUIRE_CERT implies peer certificate verification.\n"
            << "SSL_VERIFY_CERT set to true";
  }

  // Initialize OpenSSL if we've been asked to do verification of peer
  // certificates.
  if (ssl_flags->verify_cert) {
    // Set CA locations.
    if (ssl_flags->ca_file.isSome() || ssl_flags->ca_dir.isSome()) {
      const char* ca_file = ssl_flags->ca_file.isSome()
        ? ssl_flags->ca_file.get().c_str()
        : NULL;

      const char* ca_dir = ssl_flags->ca_dir.isSome()
        ? ssl_flags->ca_dir.get().c_str()
        : NULL;

      if (SSL_CTX_load_verify_locations(ctx, ca_file, ca_dir) != 1) {
        unsigned long error = ERR_get_error();
        EXIT(EXIT_FAILURE)
          << "Could not load CA file and/or directory ("
          << stringify(error)  << "): "
          << error_string(error) << " -> "
          << (ca_file != NULL ? (stringify("FILE: ") + ca_file) : "")
          << (ca_dir != NULL ? (stringify("DIR: ") + ca_dir) : "");
      }

      if (ca_file != NULL) {
        VLOG(2) << "Using CA file: " << ca_file;
      }
      if (ca_dir != NULL) {
        VLOG(2) << "Using CA dir: " << ca_dir;
      }
    } else {
      if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        EXIT(EXIT_FAILURE) << "Could not load default CA file and/or directory";
      }

      VLOG(2) << "Using default CA file '" << X509_get_default_cert_file()
              << "' and/or directory '" << X509_get_default_cert_dir() << "'";
    }

    // Set SSL peer verification callback.
    int mode = SSL_VERIFY_PEER;

    if (ssl_flags->require_cert) {
      mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    }

    SSL_CTX_set_verify(ctx, mode, &verify_callback);

    SSL_CTX_set_verify_depth(ctx, ssl_flags->verification_depth);
  } else {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  }

  // Set certificate chain.
  if (SSL_CTX_use_certificate_chain_file(
          ctx,
          ssl_flags->cert_file.get().c_str()) != 1) {
    EXIT(EXIT_FAILURE) << "Could not load cert file";
  }

  // Set private key.
  if (SSL_CTX_use_PrivateKey_file(
          ctx,
          ssl_flags->key_file.get().c_str(),
          SSL_FILETYPE_PEM) != 1) {
    EXIT(EXIT_FAILURE) << "Could not load key file";
  }

  // Validate key.
  if (SSL_CTX_check_private_key(ctx) != 1) {
    EXIT(EXIT_FAILURE)
      << "Private key does not match the certificate public key";
  }

  VLOG(2) << "Using ciphers: " << ssl_flags->ciphers;

  if (SSL_CTX_set_cipher_list(ctx, ssl_flags->ciphers.c_str()) == 0) {
    EXIT(EXIT_FAILURE) << "Could not set ciphers: " << ssl_flags->ciphers;
  }

  // Clear all the protocol options. They will be reset if needed
  // below. We do this because 'SSL_CTX_set_options' only augments, it
  // does not do an overwrite.
  SSL_CTX_clear_options(
      ctx,
      SSL_OP_NO_SSLv2 |
      SSL_OP_NO_SSLv3 |
      SSL_OP_NO_TLSv1 |
      SSL_OP_NO_TLSv1_1 |
      SSL_OP_NO_TLSv1_2);

  // Use server preference for cipher.
  long ssl_options = SSL_OP_CIPHER_SERVER_PREFERENCE;
  // Disable SSLv2.
  if (!ssl_flags->enable_ssl_v2) { ssl_options |= SSL_OP_NO_SSLv2; }
  // Disable SSLv3.
  if (!ssl_flags->enable_ssl_v3) { ssl_options |= SSL_OP_NO_SSLv3; }
  // Disable TLSv1.
  if (!ssl_flags->enable_tls_v1_0) { ssl_options |= SSL_OP_NO_TLSv1; }
  // Disable TLSv1.1.
  if (!ssl_flags->enable_tls_v1_1) { ssl_options |= SSL_OP_NO_TLSv1_1; }
  // Disable TLSv1.2.
  if (!ssl_flags->enable_tls_v1_2) { ssl_options |= SSL_OP_NO_TLSv1_2; }

  SSL_CTX_set_options(ctx, ssl_options);
}


void initialize()
{
  static Once* initialized = new Once();

  if (initialized->once()) {
    return;
  }

  // We delegate to 'reinitialize()' so that tests can change the SSL
  // configuration programatically.
  reinitialize();

  initialized->done();
}


SSL_CTX* context()
{
  // TODO(benh): Always call 'initialize' just in case?
  return ctx;
}


Try<Nothing> verify(const SSL* const ssl, const Option<string>& hostname)
{
  // Return early if we don't need to verify.
  if (!ssl_flags->verify_cert) {
    return Nothing();
  }

  // The X509 object must be freed if this call succeeds.
  // TODO(jmlvanre): handle this better. How about RAII?
  X509* cert = SSL_get_peer_certificate(ssl);

  if (cert == NULL) {
    return ssl_flags->require_cert
      ? Error("Peer did not provide certificate")
      : Try<Nothing>(Nothing());
  }

  if (SSL_get_verify_result(ssl) != X509_V_OK) {
    X509_free(cert);
    return Error("Could not verify peer certificate");
  }

  if (hostname.isNone()) {
    X509_free(cert);
    return ssl_flags->require_cert
      ? Error("Cannot verify peer certificate: peer hostname unknown")
      : Try<Nothing>(Nothing());
  }

  // From https://wiki.openssl.org/index.php/Hostname_validation.
  // Check the Subject Alternate Name extension (SAN). This is useful
  // for certificates that serve multiple physical hosts.
  STACK_OF(GENERAL_NAME)* san_names =
    reinterpret_cast<STACK_OF(GENERAL_NAME)*>(X509_get_ext_d2i(
        reinterpret_cast<X509*>(cert),
        NID_subject_alt_name,
        NULL,
        NULL));

  if (san_names != NULL) {
    int san_names_num = sk_GENERAL_NAME_num(san_names);

    // Check each name within the extension.
    for (int i = 0; i < san_names_num; i++) {
      const GENERAL_NAME* current_name = sk_GENERAL_NAME_value(san_names, i);

      if (current_name->type == GEN_DNS) {
        // Current name is a DNS name, let's check it.
        const string dns_name =
          reinterpret_cast<char*>(ASN1_STRING_data(current_name->d.dNSName));

        // Make sure there isn't an embedded NUL character in the DNS name.
        const size_t length = ASN1_STRING_length(current_name->d.dNSName);
        if (length != dns_name.length()) {
          sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);
          X509_free(cert);
          return Error(
            "X509 certificate malformed: embedded NUL character in DNS name");
        } else { // Compare expected hostname with the DNS name.
          if (hostname.get() == dns_name) {
            sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);
            X509_free(cert);
            return Nothing();
          }
        }
      }
    }

    sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);
  }

  // If we still haven't verified the hostname, try doing it via
  // the certificate subject name.
  X509_NAME* name = X509_get_subject_name(cert);

  if (name != NULL) {
    char text[_POSIX_HOST_NAME_MAX] {};

    if (X509_NAME_get_text_by_NID(
            name,
            NID_commonName,
            text,
            sizeof(text)) > 0) {
      if (hostname.get() != text) {
        X509_free(cert);
        return Error(
          "Presented Certificate Name: " + stringify(text) +
          " does not match peer hostname name: " + hostname.get());
      }

      X509_free(cert);
      return Nothing();
    }
  }

  // If we still haven't exited, we haven't verified it, and we give up.
  X509_free(cert);
  return Error(
    "Could not verify presented certificate with hostname " + hostname.get());
}

} // namespace openssl {
} // namespace network {
} // namespace process {
