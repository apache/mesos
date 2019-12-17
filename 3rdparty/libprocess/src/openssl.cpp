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

#include "openssl.hpp"

#ifndef __WINDOWS__
#include <sys/param.h>
#endif // __WINDOWS__

#ifdef USE_LIBEVENT
#include <event2/event-config.h>
#endif // USE_LIBEVENT

#ifdef __WINDOWS__
// NOTE: This must be included before the OpenSSL headers as it includes
// `WinSock2.h` and `Windows.h` in the correct order.
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <process/once.hpp>

#include <process/ssl/flags.hpp>
#include <process/ssl/tls_config.hpp>

#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/stopwatch.hpp>
#include <stout/try.hpp>

#ifdef __WINDOWS__
// OpenSSL on Windows requires this adapter module to be compiled as part of the
// consuming project to deal with Windows runtime library differences. Not doing
// so manifests itself as the "no OPENSSL_Applink" runtime error.
//
// https://www.openssl.org/docs/faq.html
#include <openssl/applink.c>
#endif // __WINDOWS__

// Smallest OpenSSL version number to which the `X509_VERIFY_PARAM_*()`
// family of functions was backported. (OpenSSL 1.0.2)
#define MIN_VERSION_X509_VERIFY_PARAM 0x10002000L

// Smallest OpenSSL version number that deprecated the `ASN1_STRING_data()`
// function in favor of `ASN1_STRING_get0_data()`. (OpenSSL 1.1.0)
#define MIN_VERSION_ASN1_STRING_GET0 0x10100000L

#if OPENSSL_VERSION_NUMBER < MIN_VERSION_ASN1_STRING_GET0
#  define ASN1_STRING_get0_data ASN1_STRING_data
#endif

using std::map;
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

// A warning is printed when a reverse DNS lookup takes longer
// than this threshold value. According to [1], average DNS query
// times vary between 5ms up to 50ms depending on what the network
// is doing, but most should be down in the < 20ms range.
//
// [1] https://blogs.akamai.com/2017/06/why-you-should-care-about-dns-latency.html
static constexpr Duration SLOW_DNS_WARN_THRESHOLD = Milliseconds(100);

// _Global_ OpenSSL context, initialized via 'initialize'.
static SSL_CTX* ctx = nullptr;


Flags::Flags()
{
  add(&Flags::enabled,
      "enabled",
      "Whether SSL is enabled.",
      false);

  add(&Flags::support_downgrade,
      "support_downgrade",
      "Enable downgrading SSL accepting sockets to non-SSL traffic. When this "
      "is enabled, no protocol may be used on non-SSL connections that "
      "conflics with the protocol headers for SSL.",
      false);

  add(&Flags::cert_file,
      "cert_file",
      "Path to certifcate.");

  add(&Flags::key_file,
      "key_file",
      "Path to key.");

  // NOTE: We're not using the libprocess built-in `DeprecatedName` mechanism
  // for these aliases. This is to prevent the situation where a task
  // configuration specifies the old value and the agent configuration
  // specifies the new value, causing a program crash at startup when
  // libprocess parses the environment for flags.
  add(&Flags::verify_cert,
      "verify_cert",
      "Legacy alias for `verify_server_cert`.",
      false);

  add(&Flags::verify_server_cert,
      "verify_server_cert",
      "Whether or not to require and verify server certificates for "
      "connections in client mode.",
      false);

  add(&Flags::require_cert,
      "require_cert",
      "Legacy alias for `require_client_cert",
      false);

  add(&Flags::require_client_cert,
      "require_client_cert",
      "Whether or not to require and verify client certificates for "
      "connections in server mode.",
      false);

  add(&Flags::verify_ipadd,
      "verify_ipadd",
      "Enable IP address verification in subject alternative name certificate "
      "extension.",
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

  add(&Flags::ecdh_curves,
      "ecdh_curves",
      "Colon separated list of curve NID or names, e.g. 'P-521:P-384:P-256'. "
      "The curves are in preference order. If no list is provided, the most "
      "appropriate curve for a client will be selected. This behavior can be "
      "explicitly enabled by setting this flag to 'auto'."
      "NOTE: Old versions of OpenSSL support only one curve, check "
      "the documentation of your OpenSSL.",
      "auto");

  add(&Flags::hostname_validation_scheme,
      "hostname_validation_scheme",
      "Select the scheme used to perform hostname validation when"
      " verifying certificates.\n"
      "Possible values: 'legacy', 'openssl'\n"
      "See `docs/ssl.md` for details on the individual algorithms.\n"
#if OPENSSL_VERSION_NUMBER < MIN_VERSION_X509_VERIFY_PARAM
      "NOTE: The currently linked version of OpenSSL is too old to support"
      " the 'openssl' scheme, version 1.0.2 or higher is required.\n"
#endif
      , "legacy");

  // We purposely don't have a flag for SSLv2. We do this because most
  // systems have disabled SSLv2 at compilation due to having so many
  // security vulnerabilities.

  add(&Flags::enable_ssl_v3,
      "enable_ssl_v3",
      "Enable SSLV3.",
      false);

  add(&Flags::enable_tls_v1_0,
      "enable_tls_v1_0",
      "Enable TLSv1.0.",
      false);

  add(&Flags::enable_tls_v1_1,
      "enable_tls_v1_1",
      "Enable TLSv1.1.",
      false);

  add(&Flags::enable_tls_v1_2,
      "enable_tls_v1_2",
      "Enable TLSv1.2.",
      true);

  add(&Flags::enable_tls_v1_3,
      "enable_tls_v1_3",
      "Enable TLSv1.3.",
      false);
}


static Flags* ssl_flags = new Flags();


const Flags& flags()
{
  openssl::initialize();
  return *ssl_flags;
}


// Mutexes necessary to support OpenSSL locking on shared data
// structures. See 'locking_function' for more information.
static std::mutex* mutexes = nullptr;


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
  static_assert(sizeof(std::thread::id) == sizeof(unsigned long),
                "sizeof(std::thread::id) must be equal to sizeof(unsigned long)"
                " for std::thread::id to be used as a function for determining "
                "a thread id");

  // We use the std::thread id and convert it to an unsigned long.
  const std::thread::id id = std::this_thread::get_id();
  return *reinterpret_cast<const unsigned long*>(&id);
}


// OpenSSL callback for creating new dynamic "locks", abstracted by
// the CRYPTO_dynlock_value structure.
CRYPTO_dynlock_value* dyn_create_function(const char* /*file*/, int /*line*/)
{
  CRYPTO_dynlock_value* value = new CRYPTO_dynlock_value();

  if (value == nullptr) {
    return nullptr;
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


#if OPENSSL_VERSION_NUMBER >= 0x0090800fL && !defined(OPENSSL_NO_ECDH)
// Sets the elliptic curve parameters for the given context in order
// to enable ECDH ciphers.
// Adapted from NGINX SSL initialization code:
// https://github.com/nginx/nginx/blob/bfe36ba3185a477d2f8ce120577308646173b736/
// src/event/ngx_event_openssl.c#L1080-L1161
static Try<Nothing> initialize_ecdh_curve(SSL_CTX* ctx, const Flags& ssl_flags)
{
#if defined(SSL_OP_SINGLE_ECDH_USE)
  // Let OpenSSL compute new ECDH parameters for each new handshake.
  // In newer versions (1.0.2+) of OpenSSL this is the default, and
  // this call has no effect.
  SSL_CTX_set_options(ctx, SSL_OP_SINGLE_ECDH_USE);
#endif // SSL_OP_SINGLE_ECDH_USE

#if (defined SSL_CTX_set1_curves_list || defined SSL_CTRL_SET_CURVES_LIST)
  // If `SSL_CTX_set_ecdh_auto` is not defined, OpenSSL will ignore the
  // preference order of the curve list and use its own algorithm to chose
  // the right curve for a connection.
#if defined(SSL_CTX_set_ecdh_auto)
  SSL_CTX_set_ecdh_auto(ctx, 1);
#endif // SSL_CTX_set_ecdh_auto

  if (ssl_flags.ecdh_curves == "auto") {
    return Nothing();
  }

  if (SSL_CTX_set1_curves_list(ctx, ssl_flags.ecdh_curves.c_str()) != 1) {
    unsigned long error = ERR_get_error();
    return Error(
        "Could not load ECDH curves '" + ssl_flags.ecdh_curves + "' " +
        "(OpenSSL error #" + stringify(error) + "): " + error_string(error));
  }

  VLOG(2) << "Using ecdh curves: " << ssl_flags.ecdh_curves;
#else // SSL_CTX_set1_curves_list || SSL_CTRL_SET_CURVES_LIST
  string curve =
      ssl_flags.ecdh_curves == "auto" ? "prime256v1" : ssl_flags.ecdh_curves;

  int nid = OBJ_sn2nid(curve.c_str());
  if (nid == 0) {
    unsigned long error = ERR_get_error();
    return Error(
        "Unknown curve '" + curve + "' (OpenSSL error #" + stringify(error) +
        "): " + error_string(error));
  }

  EC_KEY* ecdh = EC_KEY_new_by_curve_name(nid);
  if (ecdh == nullptr) {
    unsigned long error = ERR_get_error();
    return Error(
        "Error generating key from curve " + curve + "' (OpenSSL error #" +
        stringify(error) + "): " + error_string(error));
  }

  SSL_CTX_set_tmp_ecdh(ctx, ecdh);
  EC_KEY_free(ecdh);

  VLOG(2) << "Using ecdh curve: " << ssl_flags.ecdh_curves;
#endif // SSL_CTX_set1_curves_list || SSL_CTRL_SET_CURVES_LIST
  return Nothing();
}
#endif // OPENSSL_VERSION_NUMBER >= 0x0090800fL && !OPENSSL_NO_ECDH

// Tests can declare this function and use it to re-configure the SSL
// environment variables programatically. Without explicitly declaring
// this function, it is not visible. This is the preferred behavior as
// we do not want applications changing these settings while they are
// running (this would be undefined behavior).
// NOTE: This does not change the configuration of existing sockets, such
// as the server socket spawned during libprocess initialization.
// See `reinitialize` in `process.cpp`.
void reinitialize()
{
  // Wipe out and recreate the default flags.
  // This is especially important for tests, which might repeatedly
  // change environment variables and call `reinitialize`.
  *ssl_flags = Flags();

  // Load all the flags prefixed by LIBPROCESS_SSL_ from the
  // environment. See comment at top of openssl.hpp for a full list.
  //
  // NOTE: We used to look for environment variables prefixed by SSL_.
  // To be backward compatible, we interpret environment variables
  // prefixed with either SSL_ and LIBPROCESS_SSL_ where the latter
  // one takes precedence. See details in MESOS-5863.
  map<string, Option<string>> environment_ssl =
      ssl_flags->extract("SSL_");
  map<string, Option<string>> environments =
      ssl_flags->extract("LIBPROCESS_SSL_");
  foreachpair (
      const string& key, const Option<string>& value, environment_ssl) {
    if (environments.count(key) > 0 && environments.at(key) != value) {
      LOG(WARNING) << "Mismatched values for SSL environment variables "
                   << "SSL_" << key << " and "
                   << "LIBPROCESS_SSL_" << key;
    }
  }
  environments.insert(environment_ssl.begin(), environment_ssl.end());

  Try<flags::Warnings> load = ssl_flags->load(environments);
  if (load.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to load flags from environment variables "
      << "prefixed by LIBPROCESS_SSL_ or SSL_ (deprecated): "
      << load.error();
  }

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
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

    initialized_single_entry->done();
  }

  // Clean up if we had a previous SSL context object. We want to
  // re-initialize this to get rid of any non-default settings.
  if (ctx != nullptr) {
    SSL_CTX_free(ctx);
    ctx = nullptr;
  }

  // Replace with `TLS_method` once our minimum OpenSSL version
  // supports it.
  ctx = SSL_CTX_new(SSLv23_method());
  CHECK(ctx) << "Failed to create SSL context: "
             << ERR_error_string(ERR_get_error(), nullptr);

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

  // Notify users of the 'SSL_SUPPORT_DOWNGRADE' flag that this
  // setting allows insecure connections.
  if (ssl_flags->support_downgrade) {
#ifdef USE_LIBEVENT
    LOG(WARNING) <<
      "Failed SSL connections will be downgraded to a non-SSL socket";
#else
    EXIT(EXIT_FAILURE)
      << "Non-libevent SSL sockets do not support downgrade yet,"
      << " see MESOS-10073";
#endif // USE_LIBEVENT
  }

  // TODO(bevers): Remove the deprecated names for these flags after an
  // appropriate amount of time. (MESOS-9973)
  if (ssl_flags->verify_cert) {
    LOG(WARNING) << "Usage of LIBPROCESS_SSL_VERIFY_CERT is deprecated; "
                    "it was renamed to LIBPROCESS_SSL_VERIFY_SERVER_CERT";
    ssl_flags->verify_server_cert = true;
  }

  if (ssl_flags->require_cert) {
    LOG(WARNING) << "Usage of LIBPROCESS_SSL_REQUIRE_CERT is deprecated; "
                    "it was renamed to LIBPROCESS_SSL_REQUIRE_CLIENT_CERT";
    ssl_flags->require_client_cert = true;
  }

  // Print an additional warning if certificate verification is enabled while
  // supporting downgrades, since this is most likely a misconfiguration.
  if ((ssl_flags->require_client_cert || ssl_flags->verify_server_cert) &&
      ssl_flags->support_downgrade) {
    LOG(WARNING)
      << "TLS certificate verification was enabled by setting one of"
      << " LIBPROCESS_SSL_VERIFY_CERT or LIBPROCESS_SSL_REQUIRE_CERT, but"
      << " can be bypassed because TLS downgrades are enabled.";
  }

  // Now do some validation of the flags/environment variables.
  if (ssl_flags->key_file.isNone()) {
    EXIT(EXIT_FAILURE)
      << "SSL requires key! NOTE: Set path with LIBPROCESS_SSL_KEY_FILE";
  }

  if (ssl_flags->cert_file.isNone()) {
    EXIT(EXIT_FAILURE)
      << "SSL requires certificate! NOTE: Set path with "
      << "LIBPROCESS_SSL_CERT_FILE";
  }

  if (ssl_flags->ca_file.isNone()) {
    LOG(INFO) << "CA file path is unspecified! NOTE: "
              << "Set CA file path with LIBPROCESS_SSL_CA_FILE=<filepath>";
  }

  if (ssl_flags->ca_dir.isNone()) {
    LOG(INFO) << "CA directory path unspecified! NOTE: "
              << "Set CA directory path with LIBPROCESS_SSL_CA_DIR=<dirpath>";
  }

  if (ssl_flags->require_client_cert) {
    LOG(INFO) << "Will require client certificates for incoming TLS "
              << "connections.";
  }

// NOTE: Newer versions of libevent call these macros `EVENT__NUMERIC_VERSION`
// and `EVENT__HAVE_POLL`.
#if defined(_EVENT_HAVE_EPOLL) && \
    defined(_EVENT_NUMERIC_VERSION) && \
    _EVENT_NUMERIC_VERSION < 0x02010400L
  if (ssl_flags->require_client_cert &&
      ssl_flags->hostname_validation_scheme == "legacy") {
    LOG(WARNING) << "Enabling client certificate validation with the "
                 << "'legacy' hostname validation scheme is known to "
                 << "cause sporadic hangs with older versions of libevent. "
                 << "See https://issues.apache.org/jira/browse/MESOS-9867.";
  }
#endif

  if (ssl_flags->verify_ipadd) {
    LOG(INFO) << "Will use IP address verification in subject alternative name "
              << "certificate extension.";
  }

  if (ssl_flags->require_client_cert && !ssl_flags->verify_server_cert) {
    // For backwards compatibility, `require_cert` implies `verify_cert`.
    //
    // NOTE: Even without backwards compatibility considerations, this is
    // a reasonable requirement on the configuration so we apply the
    // same logic even when the modern names `require_client_cert` and
    // `verify_server_cert` are used.
    ssl_flags->verify_server_cert = true;

    LOG(INFO) << "LIBPROCESS_SSL_REQUIRE_CERT implies "
              << "server certificate verification.\n"
              << "LIBPROCESS_SSL_VERIFY_CERT set to true";
  }

  if (ssl_flags->verify_server_cert) {
    LOG(INFO) << "Will verify server certificates for outgoing TLS "
              << "connections.";
  } else {
    LOG(INFO) << "Will not verify server certificates!\n"
              << "NOTE: Set LIBPROCESS_SSL_VERIFY_SERVER_CERT=1 to enable "
              << "peer certificate verification";
  }

  if (ssl_flags->hostname_validation_scheme != "legacy" &&
      ssl_flags->hostname_validation_scheme != "openssl") {
    EXIT(EXIT_FAILURE) << "Unknown value for hostname_validation_scheme: "
                       << ssl_flags->hostname_validation_scheme;
  }

  if (ssl_flags->hostname_validation_scheme == "openssl" &&
      OPENSSL_VERSION_NUMBER < MIN_VERSION_X509_VERIFY_PARAM) {
    EXIT(EXIT_FAILURE)
      << "The 'openssl' hostname validation scheme requires OpenSSL"
         " version 1.0.2 or higher";
  }

  LOG(INFO) << "Using '" << ssl_flags->hostname_validation_scheme
            << "' scheme for hostname validation";

  // Initialize OpenSSL if we've been asked to do verification of peer
  // certificates.
  if (ssl_flags->verify_server_cert) {
    // Set CA locations.
    if (ssl_flags->ca_file.isSome() || ssl_flags->ca_dir.isSome()) {
      const char* ca_file =
        ssl_flags->ca_file.isSome() ? ssl_flags->ca_file->c_str() : nullptr;

      const char* ca_dir =
        ssl_flags->ca_dir.isSome() ? ssl_flags->ca_dir->c_str() : nullptr;

      if (SSL_CTX_load_verify_locations(ctx, ca_file, ca_dir) != 1) {
        unsigned long error = ERR_get_error();
        EXIT(EXIT_FAILURE)
          << "Could not load CA file and/or directory (OpenSSL error #"
          << stringify(error) << "): "
          << error_string(error) << " -> "
          << (ca_file != nullptr ? (stringify("FILE: ") + ca_file) : "")
          << (ca_dir != nullptr ? (stringify("DIR: ") + ca_dir) : "");
      }

      if (ca_file != nullptr) {
        LOG(INFO) << "Using CA file: " << ca_file;
      }
      if (ca_dir != nullptr) {
        LOG(INFO) << "Using CA dir: " << ca_dir;
      }
    } else {
      if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        EXIT(EXIT_FAILURE) << "Could not load default CA file and/or directory";
      }

      // For getting the defaults for ca-directory and/or ca-file from
      // openssl, we have to mimic parts of its logic; if the user has
      // set the openssl-specific environment variable, use that one -
      // if the user has not set that variable, use the compiled in
      // defaults.
      string ca_dir;

      const map<string, string> environment = os::environment();

      if (environment.count(X509_get_default_cert_dir_env()) > 0) {
        ca_dir = environment.at(X509_get_default_cert_dir_env());
      } else {
        ca_dir = X509_get_default_cert_dir();
      }

      string ca_file;

      if (environment.count(X509_get_default_cert_file_env()) > 0) {
        ca_file = environment.at(X509_get_default_cert_file_env());
      } else {
        ca_file = X509_get_default_cert_file();
      }

      LOG(INFO) << "Using default CA file '" << ca_file
                << "' and/or directory '" << ca_dir << "'";
    }

    SSL_CTX_set_verify_depth(ctx, ssl_flags->verification_depth);
  }

  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

  // Set certificate chain.
  if (SSL_CTX_use_certificate_chain_file(
          ctx,
          ssl_flags->cert_file->c_str()) != 1) {
    unsigned long error = ERR_get_error();
    EXIT(EXIT_FAILURE)
      << "Could not load cert file '" << ssl_flags->cert_file.get() << "' "
      << "(OpenSSL error #" << stringify(error) << "): " << error_string(error);
  }

  // Set private key.
  if (SSL_CTX_use_PrivateKey_file(
          ctx, ssl_flags->key_file->c_str(), SSL_FILETYPE_PEM) != 1) {
    unsigned long error = ERR_get_error();
    EXIT(EXIT_FAILURE)
      << "Could not load key file '" << ssl_flags->key_file.get() << "' "
      << "(OpenSSL error #" << stringify(error) << "): " << error_string(error);
  }

  // Validate key.
  if (SSL_CTX_check_private_key(ctx) != 1) {
    unsigned long error = ERR_get_error();
    EXIT(EXIT_FAILURE)
      << "Private key does not match the certificate public key "
      << "(OpenSSL error #" << stringify(error) << "): " << error_string(error);
  }

  VLOG(2) << "Using ciphers: " << ssl_flags->ciphers;

  if (SSL_CTX_set_cipher_list(ctx, ssl_flags->ciphers.c_str()) == 0) {
    unsigned long error = ERR_get_error();
    EXIT(EXIT_FAILURE)
      << "Could not set ciphers '" << ssl_flags->ciphers << "' "
      << "(OpenSSL error #" << stringify(error) << "): " << error_string(error);
  }

  long ssl_options =
    SSL_OP_NO_SSLv2 |
    SSL_OP_NO_SSLv3 |
    SSL_OP_NO_TLSv1 |
    SSL_OP_NO_TLSv1_1 |
#if defined(SSL_OP_NO_TLSv1_3)
    SSL_OP_NO_TLSv1_3 |
#endif
    SSL_OP_NO_TLSv1_2;

  // Clear all the protocol options. They will be reset if needed
  // below. We do this because 'SSL_CTX_set_options' only augments, it
  // does not do an overwrite.
  SSL_CTX_clear_options(ctx, ssl_options);

  // Use server preference for cipher.
  ssl_options = SSL_OP_CIPHER_SERVER_PREFERENCE;

  // Always disable SSLv2. We do this because most systems have
  // disabled SSLv2 at compilation due to having so many security
  // vulnerabilities.
  ssl_options |= SSL_OP_NO_SSLv2;

  // Disable SSLv3.
  if (!ssl_flags->enable_ssl_v3) { ssl_options |= SSL_OP_NO_SSLv3; }
  // Disable TLSv1.
  if (!ssl_flags->enable_tls_v1_0) { ssl_options |= SSL_OP_NO_TLSv1; }
  // Disable TLSv1.1.
  if (!ssl_flags->enable_tls_v1_1) { ssl_options |= SSL_OP_NO_TLSv1_1; }
  // Disable TLSv1.2.
  if (!ssl_flags->enable_tls_v1_2) { ssl_options |= SSL_OP_NO_TLSv1_2; }
#if defined(SSL_OP_NO_TLSv1_3)
  // Disable TLSv1.3.
  if (!ssl_flags->enable_tls_v1_3) { ssl_options |= SSL_OP_NO_TLSv1_3; }
#endif

  SSL_CTX_set_options(ctx, ssl_options);

#if OPENSSL_VERSION_NUMBER >= 0x0090800fL && !defined(OPENSSL_NO_ECDH)
  Try<Nothing> ecdh_initialized = initialize_ecdh_curve(ctx, *ssl_flags);
  if (ecdh_initialized.isError()) {
    EXIT(EXIT_FAILURE) << ecdh_initialized.error();
  }
#endif // OPENSSL_VERSION_NUMBER >= 0x0090800fL && !OPENSSL_NO_ECDH
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


Try<Nothing> verify(
    const SSL* const ssl,
    Mode mode,
    const Option<string>& hostname,
    const Option<net::IP>& ip)
{
  // Return early if we don't need to verify.
  if (mode == Mode::CLIENT && !ssl_flags->verify_server_cert) {
    return Nothing();
  }

  if (mode == Mode::SERVER && !ssl_flags->require_client_cert) {
    return Nothing();
  }

  // The X509 object must be freed if this call succeeds.
  std::unique_ptr<X509, decltype(&X509_free)> cert(
      SSL_get_peer_certificate(ssl),
      X509_free);

  // NOTE: Even without this check, the OpenSSL handshake will not complete
  // when connecting to servers that do not present a certificate, unless an
  // anonymous cipher is used.
  if (cert == nullptr) {
    return Error("Peer did not provide certificate");
  }

  if (SSL_get_verify_result(ssl) != X509_V_OK) {
    return Error("Could not verify peer certificate");
  }

  // When using the 'openssl' scheme, hostname validation was already
  // performed during the TLS handshake so we don't have to do it again
  // here.
  //
  // NOTE: When using the 'openssl' scheme, we technically dont need
  // to call the `openssl::verify()` function *at all*.
  if (ssl_flags->hostname_validation_scheme == "openssl") {
    return Try<Nothing>(Nothing());
  }

  // NOTE: For backwards compatibility, we ignore the passed hostname here,
  // i.e. the 'legacy' hostname validation scheme will always attempt to get
  // the peer hostname using a reverse DNS lookup.
  Option<std::string> peer_hostname = hostname;
  if (ip.isSome()) {
    VLOG(1) << "Doing rDNS lookup for 'legacy' hostname validation";
    Stopwatch watch;

    watch.start();
    Try<string> lookup = net::getHostname(ip.get());
    watch.stop();

    // Due to MESOS-9339, a slow reverse DNS lookup will cause
    // serious issues as it blocks the event loop thread.
    if (watch.elapsed() > SLOW_DNS_WARN_THRESHOLD) {
      LOG(WARNING) << "Reverse DNS lookup for '" << ip.get() << "'"
                   << " took " << watch.elapsed().ms() << "ms"
                   << ", slowness is problematic (see MESOS-9339)";
    }

    if (lookup.isError()) {
      LOG(WARNING) << "Reverse DNS lookup for '" << ip.get() << "'"
                   << " failed: " << lookup.error();
    } else {
      VLOG(2) << "Accepting from " << lookup.get();
      peer_hostname = lookup.get();
    }
  }

  if (!ssl_flags->verify_ipadd && peer_hostname.isNone()) {
    return ssl_flags->require_client_cert
      ? Error("Cannot verify peer certificate: peer hostname unknown")
      : Try<Nothing>(Nothing());
  }

  // From https://wiki.openssl.org/index.php/Hostname_validation.
  // Check the Subject Alternate Name extension (SAN). This is useful
  // for certificates where multiple domains are served from the same
  // physical host.
  STACK_OF(GENERAL_NAME)* san_names =
    reinterpret_cast<STACK_OF(GENERAL_NAME)*>(X509_get_ext_d2i(
        cert.get(),
        NID_subject_alt_name,
        nullptr,
        nullptr));

  if (san_names != nullptr) {
    int san_names_num = sk_GENERAL_NAME_num(san_names);

    // Check each name within the extension.
    for (int i = 0; i < san_names_num; i++) {
      const GENERAL_NAME* current_name = sk_GENERAL_NAME_value(san_names, i);

      switch(current_name->type) {
        case GEN_DNS: {
          if (peer_hostname.isSome()) {
            // Current name is a DNS name, let's check it.
            const string dns_name = reinterpret_cast<const char*>(
                ASN1_STRING_get0_data(current_name->d.dNSName));

            // Make sure there isn't an embedded NUL character in the DNS name.
            const size_t length = ASN1_STRING_length(current_name->d.dNSName);
            if (length != dns_name.length()) {
              sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);
              return Error(
                  "X509 certificate malformed: "
                  "embedded NUL character in DNS name");
            } else {
              VLOG(2) << "Matching dNSName(" << i << "): " << dns_name;

              // Compare expected hostname with the DNS name.
              if (peer_hostname.get() == dns_name) {
                sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);

                VLOG(2) << "dNSName match found for " << peer_hostname.get();

                return Nothing();
              }
            }
          }
          break;
        }
        case GEN_IPADD: {
          if (ssl_flags->verify_ipadd && ip.isSome()) {
            // Current name is an IPAdd, let's check it.
            const ASN1_OCTET_STRING* current_ipadd = current_name->d.iPAddress;

            if (current_ipadd->type == V_ASN1_OCTET_STRING &&
                current_ipadd->data != nullptr &&
                current_ipadd->length == sizeof(uint32_t)) {
              const net::IP ip_add(ntohl(
                  *reinterpret_cast<uint32_t*>(current_ipadd->data)));

              VLOG(2) << "Matching iPAddress(" << i << "): " << ip_add;

              if (ip.get() == ip_add) {
                sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);

                VLOG(2) << "iPAddress match found for " << ip.get();

                return Nothing();
              }
            }
          }
          break;
        }
      }
    }

    sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);
  }

  if (peer_hostname.isSome()) {
    // If we still haven't verified the hostname, try doing it via
    // the certificate subject name.
    X509_NAME* name = X509_get_subject_name(cert.get());

    if (name != nullptr) {
      char text[MAXHOSTNAMELEN] {};

      if (X509_NAME_get_text_by_NID(
              name,
              NID_commonName,
              text,
              sizeof(text)) > 0) {
        VLOG(2) << "Matching common name: " << text;

        if (peer_hostname.get() != text) {
          return Error(
            "Presented Certificate Name: " + stringify(text) +
            " does not match peer hostname name: " + peer_hostname.get());
        }

        VLOG(2) << "Common name match found for " << peer_hostname.get();

        return Nothing();
      }
    }
  }

  // If we still haven't exited, we haven't verified it, and we give up.
  std::vector<string> details;

  if (peer_hostname.isSome()) {
    details.push_back("hostname " + peer_hostname.get());
  }

  if (ip.isSome()) {
    details.push_back("IP " + stringify(ip.get()));
  }

  return Error(
      "Could not verify presented certificate with " +
      strings::join(", ", details));
}


// A callback to configure the `SSL` object before the connection is
// established.
Try<Nothing> configure_socket(
    SSL* ssl,
    openssl::Mode mode,
    const Address& peer_address,
    const Option<std::string>& peer_hostname)
{
  if (mode == Mode::CLIENT && ssl_flags->verify_server_cert) {
    SSL_set_verify(
        ssl,
        SSL_VERIFY_PEER,
        &verify_callback);
  }

  if (mode == Mode::SERVER && ssl_flags->require_client_cert) {
    SSL_set_verify(
        ssl,
        SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
        &verify_callback);
  }

  if (ssl_flags->hostname_validation_scheme == "openssl") {
#if OPENSSL_VERSION_NUMBER < MIN_VERSION_X509_VERIFY_PARAM
    // We should have already checked this during startup.
    EXIT(EXIT_FAILURE) <<
        "The linked OpenSSL library does not support `X509_VERIFY_PARAM` for"
        " hostname validation. OpenSSL >= 1.0.2 is required.";
#else
    if (mode == openssl::Mode::SERVER) {
      // We don't do client hostname validation, because the application layer
      // should set the policy on which certificate fields are considered a
      // valid proof of identity.
      //
      // TODO(bevers): Provide hooks to the application code to make these
      // policy decisions, for example via a Mesos module.
      return Nothing();
    }

    if (mode == openssl::Mode::CLIENT && !ssl_flags->verify_server_cert) {
      return Nothing();
    }

    // Decide whether we want to verify the peer's IP or DNS name.
    X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
    if (peer_hostname.isSome()) {
      if (!X509_VERIFY_PARAM_set1_host(param, peer_hostname->c_str(), 0)) {
        return Error("Could not enable x509 hostname check.");
      }
    } else {
      if (!ssl_flags->verify_ipadd) {
        return Error("No DNS name given and IP address verification is "
                     " disabled. I cannot work like this :(");
      }

      if (peer_address.family() != Address::Family::INET4 &&
          peer_address.family() != Address::Family::INET6) {
        return Error("Can only use IPv4 or IPv6 addresses for IP address"
                     " validation.");
      }

      Try<inet::Address> inetAddress =
        network::convert<inet::Address>(peer_address);

      string ip = stringify(inetAddress->ip);
      if (!X509_VERIFY_PARAM_set1_ip_asc(param, ip.c_str())) {
        return Error("Could not enable x509 IP check.");
      }
    }
#endif
  }

  return Nothing();
}


// Wrappers to be able to use the above `verify()` and `configure_socket()`
// inside a `TLSClientConfig` struct.
Try<Nothing> client_verify(
    const SSL* const ssl,
    const Option<std::string>& hostname,
    const Option<net::IP>& ip)
{
  return verify(ssl, Mode::CLIENT, hostname, ip);
}


Try<Nothing> client_configure_socket(
    SSL* ssl,
    const Address& peer,
    const Option<std::string>& peer_hostname)
{
  return configure_socket(ssl, Mode::CLIENT, peer, peer_hostname);
}


TLSClientConfig::TLSClientConfig(
    const Option<std::string>& servername,
    SSL_CTX *ctx,
    ConfigureSocketCallback configure_socket,
    VerifyCallback verify)
  : ctx(ctx),
    servername(servername),
    verify(verify),
    configure_socket(configure_socket)
{}


TLSClientConfig create_tls_client_config(
    const Option<std::string>& servername)
{
  return TLSClientConfig(
      servername,
      openssl::ctx,
      &client_configure_socket,
      &client_verify);
}

} // namespace openssl {
} // namespace network {
} // namespace process {
