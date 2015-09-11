/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __PROCESS_SSL_TEST_HPP__
#define __PROCESS_SSL_TEST_HPP__

#ifdef USE_SSL_SOCKET

#include <openssl/rsa.h>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <process/io.hpp>
#include <process/socket.hpp>

#include <process/ssl/utilities.hpp>

namespace process {
namespace network {
namespace openssl {

// Forward declare the `reinitialize()` function since we want to
// programatically change SSL flags during tests.
void reinitialize();

} // namespace openssl {
} // namespace network {
} // namespace process {


/**
 * A Test fixture that sets up SSL keys and certificates.
 *
 * This class sets up a private key and certificate pair at
 * SSLTest::key_path and SSLTest::certificate_path.
 * It also sets up an independent 'scrap' pair that can be used to
 * test an invalid certificate authority chain. These can be found at
 * SSLTest::scrap_key_path and SSLTest::scrap_certificate_path.
 *
 * There are some helper functions like SSLTest::setup_server and
 * SSLTest::launch_client that factor out common behavior used in
 * tests.
 */
class SSLTest : public ::testing::Test
{
protected:
  SSLTest() : data("Hello World!") {}

  /**
   * @return The path to the authorized private key.
   */
  static const Path& key_path()
  {
    static Path path(path::join(os::getcwd(), "key.pem"));
    return path;
  }

  /**
   * @return The path to the authorized certificate.
   */
  static const Path& certificate_path()
  {
    static Path path(path::join(os::getcwd(), "cert.pem"));
    return path;
  }

  /**
   * @return The path to the unauthorized private key.
   */
  static const Path& scrap_key_path()
  {
    static Path path(path::join(os::getcwd(), "scrap_key.pem"));
    return path;
  }

  /**
   * @return The path to the unauthorized certificate.
   */
  static const Path& scrap_certificate_path()
  {
    static Path path(path::join(os::getcwd(), "scrap_cert.pem"));
    return path;
  }

  static void cleanup_directories()
  {
    os::rm(key_path().value);
    os::rm(certificate_path().value);
    os::rm(scrap_key_path().value);
    os::rm(scrap_certificate_path().value);
  }

  static void SetUpTestCase()
  {
    // We store the allocated objects in these results so that we can
    // have a consolidated 'cleanup()' function. This makes all the
    // 'EXIT()' calls more readable and less error prone.
    Result<EVP_PKEY*> private_key = None();
    Result<X509*> certificate = None();
    Result<EVP_PKEY*> scrap_key = None();
    Result<X509*> scrap_certificate = None();

    auto cleanup = [&private_key, &certificate, &scrap_key, &scrap_certificate](
        bool failure = true) {
      if (private_key.isSome()) { EVP_PKEY_free(private_key.get()); }
      if (certificate.isSome()) { X509_free(certificate.get()); }
      if (scrap_key.isSome()) { EVP_PKEY_free(scrap_key.get()); }
      if (scrap_certificate.isSome()) { X509_free(scrap_certificate.get()); }

      // If we are under a failure condition, clean up any files we
      // already generated. The expected behavior is that they will be
      // cleaned up in 'TearDownTestCase()'; however, we call ABORT
      // during 'SetUpTestCase()' failures.
      if (failure) {
        cleanup_directories();
      }
    };

    // Generate the authority key.
    private_key = process::network::openssl::generate_private_rsa_key();
    if (private_key.isError()) {
      ABORT("Could not generate private key: " + private_key.error());
    }

    // Figure out the hostname that 'INADDR_LOOPBACK' will bind to.
    // Set the hostname of the certificate to this hostname so that
    // hostname verification of the certificate will pass.
    Try<std::string> hostname = net::getHostname(net::IP(INADDR_LOOPBACK));
    if (hostname.isError()) {
      cleanup();
      ABORT("Could not determine hostname of 'INADDR_LOOPBACK': " +
            hostname.error());
    }

    // Generate an authorized certificate.
    certificate = process::network::openssl::generate_x509(
        private_key.get(),
        private_key.get(),
        None(),
        1,
        365,
        hostname.get());

    if (certificate.isError()) {
      cleanup();
      ABORT("Could not generate certificate: " + certificate.error());
    }

    // Write the authority key to disk.
    Try<Nothing> key_write =
      process::network::openssl::write_key_file(private_key.get(), key_path());

    if (key_write.isError()) {
      cleanup();
      ABORT("Could not write private key to disk: " + key_write.error());
    }

    // Write the authorized certificate to disk.
    Try<Nothing> certificate_write =
      process::network::openssl::write_certificate_file(
          certificate.get(),
          certificate_path());

    if (certificate_write.isError()) {
      cleanup();
      ABORT("Could not write certificate to disk: " +
            certificate_write.error());
    }

    // Generate a scrap key.
    scrap_key = process::network::openssl::generate_private_rsa_key();
    if (scrap_key.isError()) {
      cleanup();
      ABORT("Could not generate a scrap private key: " + scrap_key.error());
    }

    // Write the scrap key to disk.
    key_write = process::network::openssl::write_key_file(
        scrap_key.get(),
        scrap_key_path());

    if (key_write.isError()) {
      cleanup();
      ABORT("Could not write scrap key to disk: " + key_write.error());
    }

    // Generate a scrap certificate.
    scrap_certificate = process::network::openssl::generate_x509(
        scrap_key.get(),
        scrap_key.get());

    if (scrap_certificate.isError()) {
      cleanup();
      ABORT("Could not generate a scrap certificate: " +
            scrap_certificate.error());
    }

    // Write the scrap certificate to disk.
    certificate_write = process::network::openssl::write_certificate_file(
        scrap_certificate.get(),
        scrap_certificate_path());

    if (certificate_write.isError()) {
      cleanup();
      ABORT("Could not write scrap certificate to disk: " +
            certificate_write.error());
    }

    // Since we successfully set up all our state, we call cleanup
    // with failure set to 'false'.
    cleanup(false);
  }

  static void TearDownTestCase()
  {
    // Clean up all the pem files we generated.
    cleanup_directories();
  }

  virtual void SetUp()
  {
    // This unsets all the SSL environment variables. Necessary for
    // ensuring a clean starting slate between tests.
    os::unsetenv("SSL_ENABLED");
    os::unsetenv("SSL_SUPPORT_DOWNGRADE");
    os::unsetenv("SSL_CERT_FILE");
    os::unsetenv("SSL_KEY_FILE");
    os::unsetenv("SSL_VERIFY_CERT");
    os::unsetenv("SSL_REQUIRE_CERT");
    os::unsetenv("SSL_VERIFY_DEPTH");
    os::unsetenv("SSL_CA_DIR");
    os::unsetenv("SSL_CA_FILE");
    os::unsetenv("SSL_CIPHERS");
    os::unsetenv("SSL_ENABLE_SSL_V3");
    os::unsetenv("SSL_ENABLE_TLS_V1_0");
    os::unsetenv("SSL_ENABLE_TLS_V1_1");
    os::unsetenv("SSL_ENABLE_TLS_V1_2");
  }

  /**
   * Initializes a listening server.
   *
   * @param environment The SSL environment variables to launch the
   *     server socket with.
   *
   * @return Socket if successful otherwise an Error.
   */
  Try<process::network::Socket> setup_server(
      const std::map<std::string, std::string>& environment)
  {
    foreachpair (
        const std::string& name, const std::string& value, environment) {
      os::setenv(name, value);
    }

    process::network::openssl::reinitialize();

    const Try<process::network::Socket> create =
      process::network::Socket::create(process::network::Socket::SSL);

    if (create.isError()) {
      return Error(create.error());
    }

    process::network::Socket server = create.get();

    // We need to explicitly bind to INADDR_LOOPBACK so the
    // certificate we create in this test fixture can be verified.
    Try<process::network::Address> bind =
      server.bind(process::network::Address(net::IP(INADDR_LOOPBACK), 0));

    if (bind.isError()) {
      return Error(bind.error());
    }

    const Try<Nothing> listen = server.listen(BACKLOG);
    if (listen.isError()) {
      return Error(listen.error());
    }

    return server;
  }

  /**
   * Launches a test SSL client as a subprocess connecting to the
   * server.
   *
   * The subprocess calls the 'ssl-client' binary with the provided
   * environment.
   *
   * @param environment The SSL environment variables to launch the
   *     SSL client subprocess with.
   * @param use_ssl_socket Whether the SSL client will try to connect
   *     using an SSL socket or a POLL socket.
   *
   * @return Subprocess if successful otherwise an Error.
   */
  Try<process::Subprocess> launch_client(
      const std::map<std::string, std::string>& environment,
      const process::network::Socket& server,
      bool use_ssl_socket)
  {
    const Try<process::network::Address> address = server.address();
    if (address.isError()) {
      return Error(address.error());
    }

    // Set up arguments to be passed to the 'client-ssl' binary.
    const std::vector<std::string> argv = {
      "ssl-client",
      "--use_ssl=" + stringify(use_ssl_socket),
      "--server=127.0.0.1",
      "--port=" + stringify(address.get().port),
      "--data=" + data};

    Result<std::string> path = os::realpath(BUILD_DIR);
    if (!path.isSome()) {
      return Error("Could not establish build directory path");
    }

    return process::subprocess(
        path::join(path.get(), "ssl-client"),
        argv,
        process::Subprocess::PIPE(),
        process::Subprocess::PIPE(),
        process::Subprocess::FD(STDERR_FILENO),
        None(),
        environment);
  }

  static constexpr size_t BACKLOG = 5;

  const std::string data;
};

#endif // USE_SSL_SOCKET

#endif // __PROCESS_SSL_TEST_HPP__
