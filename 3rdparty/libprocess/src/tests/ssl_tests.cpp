#include <stdio.h>

#include <openssl/rsa.h>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/io.hpp>
#include <process/socket.hpp>
#include <process/subprocess.hpp>

#include <stout/gtest.hpp>
#include <stout/os.hpp>

#include "openssl.hpp"
#include "openssl_util.hpp"

using std::map;
using std::string;
using std::vector;

// We only run these tests if we have configured with '--enable-ssl'.
#ifdef USE_SSL_SOCKET

namespace process {
namespace network {
namespace openssl {

// Forward declare the `reinitialize()` function since we want to
// programatically change SSL flags during tests.
void reinitialize();

} // namespace openssl {
} // namespace network {
} // namespace process {

using namespace process;
using namespace process::network;


// Wait for a subprocess and test the status code for the following
// conditions of 'expected_status':
//   1. 'None' = Anything but '0'.
//   2. 'Some' = the value of 'expected_status'.
// Returns Nothing if the resulting status code matches the
// expectation otherwise a Failure with the output of the subprocess.
// TODO(jmlvanre): Turn this into a generally useful abstraction for
// gtest where we can have a more straigtforward 'expected_status'.
Future<Nothing> await_subprocess(
    const Subprocess& subprocess,
    const Option<int>& expected_status = None())
{
  // Dup the pipe fd of the subprocess so we can read the output if
  // needed.
  int out = dup(subprocess.out().get());

  // Once we get the status of the process.
  return subprocess.status()
    .then([=](const Option<int>& status) -> Future<Nothing> {
      // If the status is not set, fail out.
      if (status.isNone()) {
        return Failure("Subprocess status is none");
      }

      // If the status is not what we expect then fail out with the
      // output of the subprocess. The failure message will include
      // the assertion failures of the subprocess.
      if ((expected_status.isSome() && status.get() != expected_status.get()) ||
          (expected_status.isNone() && status.get() == 0)) {
        return io::read(out)
          .then([](const string& output) -> Future<Nothing> {
            return Failure("\n[++++++++++] Subprocess output.\n" + output +
                           "[++++++++++]\n");
          });
      }

      // If the subprocess ran successfully then return nothing.
      return Nothing();
    }).onAny([=]() {
      os::close(out);
    });
}


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
        os::rm(key_path().value);
        os::rm(certificate_path().value);
        os::rm(scrap_key_path().value);
        os::rm(scrap_certificate_path().value);
      }
    };

    // Generate the authority key.
    private_key = openssl::generate_private_rsa_key();
    if (private_key.isError()) {
      ABORT("Could not generate private key: " + private_key.error());
    }

    // Figure out the hostname that 'INADDR_LOOPBACK' will bind to.
    // Set the hostname of the certificate to this hostname so that
    // hostname verification of the certificate will pass.
    Try<string> hostname = net::getHostname(net::IP(INADDR_LOOPBACK));
    if (hostname.isError()) {
      cleanup();
      ABORT("Could not determine hostname of 'INADDR_LOOPBACK': " +
            hostname.error());
    }

    // Generate an authorized certificate.
    certificate = openssl::generate_x509(
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
      openssl::write_key_file(private_key.get(), key_path());

    if (key_write.isError()) {
      cleanup();
      ABORT("Could not write private key to disk: " + key_write.error());
    }

    // Write the authorized certificate to disk.
    Try<Nothing> certificate_write =
      openssl::write_certificate_file(certificate.get(), certificate_path());

    if (certificate_write.isError()) {
      cleanup();
      ABORT("Could not write certificate to disk: " +
            certificate_write.error());
    }

    // Generate a scrap key.
    scrap_key = openssl::generate_private_rsa_key();
    if (scrap_key.isError()) {
      cleanup();
      ABORT("Could not generate a scrap private key: " + scrap_key.error());
    }

    // Write the scrap key to disk.
    key_write = openssl::write_key_file(scrap_key.get(), scrap_key_path());

    if (key_write.isError()) {
      cleanup();
      ABORT("Could not write scrap key to disk: " + key_write.error());
    }

    // Generate a scrap certificate.
    scrap_certificate =
      openssl::generate_x509(scrap_key.get(), scrap_key.get());

    if (scrap_certificate.isError()) {
      cleanup();
      ABORT("Could not generate a scrap certificate: " +
            scrap_certificate.error());
    }

    // Write the scrap certificate to disk.
    certificate_write = openssl::write_certificate_file(
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
    os::rm(key_path().value);
    os::rm(certificate_path().value);
    os::rm(scrap_key_path().value);
    os::rm(scrap_certificate_path().value);
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
    os::unsetenv("SSL_ENABLE_SSL_V2");
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
  Try<Socket> setup_server(const map<string, string>& environment)
  {
    foreachpair (const string& name, const string& value, environment) {
      os::setenv(name, value);
    }
    openssl::reinitialize();

    const Try<Socket> create = Socket::create(Socket::SSL);
    if (create.isError()) {
      return Error(create.error());
    }

    Socket server = create.get();

    // We need to explicitly bind to INADDR_LOOPBACK so the
    // certificate we create in this test fixture can be verified.
    Try<Address> bind = server.bind(Address(net::IP(INADDR_LOOPBACK), 0));
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
  Try<Subprocess> launch_client(
      const map<string, string>& environment,
      const Socket& server,
      bool use_ssl_socket)
  {
    const Try<Address> address = server.address();
    if (address.isError()) {
      return Error(address.error());
    }

    // Set up arguments to be passed to the 'client-ssl' binary.
    const vector<string> argv = {
      "ssl-client",
      "--use_ssl=" + stringify(use_ssl_socket),
      "--server=127.0.0.1",
      "--port=" + stringify(address.get().port),
      "--data=" + data};

    Result<string> path = os::realpath(BUILD_DIR);
    if (!path.isSome()) {
      return Error("Could not establish build directory path");
    }

    return subprocess(
        path::join(path.get(), "ssl-client"),
        argv,
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        Subprocess::FD(STDERR_FILENO),
        None(),
        environment);
  }

  static constexpr size_t BACKLOG = 5;

  const string data;
};


// Ensure that we can't create an SSL socket when SSL is not enabled.
TEST(SSL, Disabled)
{
  os::setenv("SSL_ENABLED", "false");
  openssl::reinitialize();
  EXPECT_ERROR(Socket::create(Socket::SSL));
}


// Test a basic back-and-forth communication within the same OS
// process.
TEST_F(SSLTest, BasicSameProcess)
{
  os::setenv("SSL_ENABLED", "true");
  os::setenv("SSL_KEY_FILE", key_path().value);
  os::setenv("SSL_CERT_FILE", certificate_path().value);
  os::setenv("SSL_REQUIRE_CERT", "true");
  os::setenv("SSL_CA_DIR", os::getcwd());
  os::setenv("SSL_CA_FILE", certificate_path().value);

  openssl::reinitialize();

  const Try<Socket> server_create = Socket::create(Socket::SSL);
  ASSERT_SOME(server_create);

  const Try<Socket> client_create = Socket::create(Socket::SSL);
  ASSERT_SOME(client_create);

  Socket server = server_create.get();
  Socket client = client_create.get();

  // We need to explicitly bind to INADDR_LOOPBACK so the certificate
  // we create in this test fixture can be verified.
  ASSERT_SOME(server.bind(Address(net::IP(INADDR_LOOPBACK), 0)));

  const Try<Nothing> listen = server.listen(BACKLOG);
  ASSERT_SOME(listen);

  const Try<Address> server_address = server.address();
  ASSERT_SOME(server_address);

  const Future<Socket> _socket = server.accept();

  const Future<Nothing> connect = client.connect(server_address.get());

  // Wait for the server to have accepted the client connection.
  AWAIT_ASSERT_READY(_socket);
  Socket socket = _socket.get(); // TODO(jmlvanre): Remove const copy.

  // Verify that the client also views the connection as established.
  AWAIT_ASSERT_READY(connect);

  // Send a message from the client to the server.
  const string data = "Hello World!";
  AWAIT_ASSERT_READY(client.send(data));

  // Verify the server received the message.
  AWAIT_ASSERT_EQ(data, socket.recv());

  // Send the message back from the server to the client.
  AWAIT_ASSERT_READY(socket.send(data));

  // Verify the client received the message.
  AWAIT_ASSERT_EQ(data, client.recv());
}


// Test a basic back-and-forth communication using the 'ssl-client'
// subprocess.
TEST_F(SSLTest, SSLSocket)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}},
      server.get(),
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  // TODO(jmlvanre): Remove const copy.
  AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
  AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

  AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
}


// Ensure that a POLL based socket can't connect to an SSL based
// socket, even when SSL is enabled.
TEST_F(SSLTest, NonSSLSocket)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}},
      server.get(),
      false);
  ASSERT_SOME(client);

  Future<Socket> socket = server.get().accept();
  AWAIT_ASSERT_FAILED(socket);

  AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
}


// Ensure that a certificate that was not generated using the
// certificate authority is still allowed to communicate as long as
// the SSL_VERIFY_CERT and SSL_REQUIRE_CERT flags are disabled.
TEST_F(SSLTest, NoVerifyBadCA)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value},
      {"SSL_VERIFY_CERT", "false"},
      {"SSL_REQUIRE_CERT", "false"}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", scrap_key_path().value},
      {"SSL_CERT_FILE", scrap_certificate_path().value},
      {"SSL_REQUIRE_CERT", "true"},
      {"SSL_CA_FILE", certificate_path().value}},
      server.get(),
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  // TODO(jmlvanre): Remove const copy.
  AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
  AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

  AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
}


// Ensure that a certificate that was not generated using the
// certificate authority is NOT allowed to communicate when the
// SSL_REQUIRE_CERT flag is enabled.
TEST_F(SSLTest, RequireBadCA)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value},
      {"SSL_REQUIRE_CERT", "true"}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", scrap_key_path().value},
      {"SSL_CERT_FILE", scrap_certificate_path().value},
      {"SSL_REQUIRE_CERT", "false"}},
      server.get(),
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server.get().accept();
  AWAIT_ASSERT_FAILED(socket);

  AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
}


// Ensure that a certificate that was not generated using the
// certificate authority is NOT allowed to communicate when the
// SSL_VERIFY_CERT flag is enabled.
TEST_F(SSLTest, VerifyBadCA)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value},
      {"SSL_VERIFY_CERT", "true"}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", scrap_key_path().value},
      {"SSL_CERT_FILE", scrap_certificate_path().value},
      {"SSL_REQUIRE_CERT", "false"}},
      server.get(),
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server.get().accept();
  AWAIT_ASSERT_FAILED(socket);

  AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
}


// Ensure that a certificate that WAS generated using the certificate
// authority is NOT allowed to communicate when the SSL_VERIFY_CERT
// flag is enabled.
TEST_F(SSLTest, VerifyCertificate)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value},
      {"SSL_VERIFY_CERT", "true"}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value},
      {"SSL_REQUIRE_CERT", "true"}},
      server.get(),
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  // TODO(jmlvanre): Remove const copy.
  AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
  AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

  AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
}


// Ensure that a certificate that WAS generated using the certificate
// authority is NOT allowed to communicate when the SSL_REQUIRE_CERT
// flag is enabled.
TEST_F(SSLTest, RequireCertificate)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value},
      {"SSL_REQUIRE_CERT", "true"}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value},
      {"SSL_REQUIRE_CERT", "true"}},
      server.get(),
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  // TODO(jmlvanre): Remove const copy.
  AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
  AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

  AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
}


// Test all the combinations of protocols. Ensure that they can only
// communicate if the opposing end allows the given protocol, and not
// otherwise.
TEST_F(SSLTest, ProtocolMismatch)
{
  const vector<string> protocols = {
    // Openssl can be compiled with SSLV2 and/or SSLV3 disabled
    // completely, so we conditionally test these protocol.
#ifndef OPENSSL_NO_SSL2
    "SSL_ENABLE_SSL_V2",
#endif
#ifndef OPENSSL_NO_SSL3
    "SSL_ENABLE_SSL_V3",
#endif
    "SSL_ENABLE_TLS_V1_0",
    "SSL_ENABLE_TLS_V1_1",
    "SSL_ENABLE_TLS_V1_2"
  };

  // For each server protocol.
  foreach (const string& server_protocol, protocols) {
    // For each client protocol.
    foreach (const string& client_protocol, protocols) {
      LOG(INFO) << "Testing server protocol '" << server_protocol
                << "' with client protocol '" << client_protocol << "'\n";

      // Set up the default server environment variables.
      map<string, string> server_environment = {
        {"SSL_ENABLED", "true"},
        {"SSL_KEY_FILE", key_path().value},
        {"SSL_CERT_FILE", certificate_path().value}
      };

      // Set up the default client environment variables.
      map<string, string> client_environment = {
        {"SSL_ENABLED", "true"},
        {"SSL_KEY_FILE", key_path().value},
        {"SSL_CERT_FILE", certificate_path().value},
      };

      // Disable all protocols except for the one we're testing.
      foreach (const string& protocol, protocols) {
        server_environment.emplace(
            protocol,
            stringify(protocol == server_protocol));

        client_environment.emplace(
            protocol,
            stringify(protocol == client_protocol));
      }

      // Set up the server.
      Try<Socket> server = setup_server(server_environment);
      ASSERT_SOME(server);

      // Launch the client.
      Try<Subprocess> client =
        launch_client(client_environment, server.get(), true);
      ASSERT_SOME(client);

      if (server_protocol == client_protocol) {
        // If the protocols are the same, it is valid.
        Future<Socket> socket = server.get().accept();
        AWAIT_ASSERT_READY(socket);

        // TODO(jmlvanre): Remove const copy.
        AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
        AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

        AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
      } else {
        // If the protocols are NOT the same, it is invalid.
        Future<Socket> socket = server.get().accept();
        AWAIT_ASSERT_FAILED(socket);

        AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
      }
    }
  }
}


// Ensure we can communicate between a POLL based socket and an SSL
// socket if 'SSL_SUPPORT_DOWNGRADE' is enabled.
TEST_F(SSLTest, ValidDowngrade)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_SUPPORT_DOWNGRADE", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value},
      {"SSL_REQUIRE_CERT", "true"}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"SSL_ENABLED", "false"}},
      server.get(),
      false);
  ASSERT_SOME(client);

  Future<Socket> socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  // TODO(jmlvanre): Remove const copy.
  AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
  AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

  AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
}


// Ensure we can NOT communicate between a POLL based socket and an
// SSL socket if 'SSL_SUPPORT_DOWNGRADE' is not enabled.
TEST_F(SSLTest, NoValidDowngrade)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_SUPPORT_DOWNGRADE", "false"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value},
      {"SSL_REQUIRE_CERT", "true"}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"SSL_ENABLED", "false"}},
      server.get(),
      false);
  ASSERT_SOME(client);

  Future<Socket> socket = server.get().accept();
  AWAIT_ASSERT_FAILED(socket);

  AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
}


// For each protocol: ensure we can communicate between a POLL based
// socket and an SSL socket if 'SSL_SUPPORT_DOWNGRADE' is enabled.
TEST_F(SSLTest, ValidDowngradeEachProtocol)
{
  const vector<string> protocols = {
    // Openssl can be compiled with SSLV2 and/or SSLV3 disabled
    // completely, so we conditionally test these protocol.
#ifndef OPENSSL_NO_SSL2
    "SSL_ENABLE_SSL_V2",
#endif
#ifndef OPENSSL_NO_SSL3
    "SSL_ENABLE_SSL_V3",
#endif
    "SSL_ENABLE_TLS_V1_0",
    "SSL_ENABLE_TLS_V1_1",
    "SSL_ENABLE_TLS_V1_2"
  };

  // For each protocol.
  foreach (const string& server_protocol, protocols) {
    LOG(INFO) << "Testing server protocol '" << server_protocol << "'\n";

    // Set up the default server environment variables.
    map<string, string> server_environment = {
      {"SSL_ENABLED", "true"},
      {"SSL_SUPPORT_DOWNGRADE", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}
    };

    // Disable all protocols except for the one we're testing.
    foreach (const string& protocol, protocols) {
      server_environment.emplace(
          protocol,
          stringify(protocol == server_protocol));
    }

    // Set up the server.
    Try<Socket> server = setup_server(server_environment);
    ASSERT_SOME(server);

    // Launch the client with a POLL socket.
    Try<Subprocess> client = launch_client({
        {"SSL_ENABLED", "false"}},
        server.get(),
        false);
    ASSERT_SOME(client);

    Future<Socket> socket = server.get().accept();
    AWAIT_ASSERT_READY(socket);

    // TODO(jmlvanre): Remove const copy.
    AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
    AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

    AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
  }
}


// For each protocol: ensure we can NOT communicate between a POLL
// based socket and an SSL socket if 'SSL_SUPPORT_DOWNGRADE' is not
// enabled.
TEST_F(SSLTest, NoValidDowngradeEachProtocol)
{
  const vector<string> protocols = {
    // Openssl can be compiled with SSLV2 and/or SSLV3 disabled
    // completely, so we conditionally test these protocol.
#ifndef OPENSSL_NO_SSL2
    "SSL_ENABLE_SSL_V2",
#endif
#ifndef OPENSSL_NO_SSL3
    "SSL_ENABLE_SSL_V3",
#endif
    "SSL_ENABLE_TLS_V1_0",
    "SSL_ENABLE_TLS_V1_1",
    "SSL_ENABLE_TLS_V1_2"
  };

  // For each protocol.
  foreach (const string& server_protocol, protocols) {
    LOG(INFO) << "Testing server protocol '" << server_protocol << "'\n";

    // Set up the default server environment variables.
    map<string, string> server_environment = {
      {"SSL_ENABLED", "true"},
      {"SSL_SUPPORT_DOWNGRADE", "false"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}
    };

    // Disable all protocols except for the one we're testing.
    foreach (const string& protocol, protocols) {
      server_environment.emplace(
          protocol,
          stringify(protocol == server_protocol));
    }

    // Set up the server.
    Try<Socket> server = setup_server(server_environment);
    ASSERT_SOME(server);

    // Launch the client with a POLL socket.
    Try<Subprocess> client = launch_client({
        {"SSL_ENABLED", "false"}},
        server.get(),
        false);
    ASSERT_SOME(client);

    Future<Socket> socket = server.get().accept();
    AWAIT_ASSERT_FAILED(socket);

    AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
  }
}


// Verify that the 'peer()' address call works correctly.
TEST_F(SSLTest, PeerAddress)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}});
  ASSERT_SOME(server);

  const Try<Socket> client_create = Socket::create(Socket::SSL);
  ASSERT_SOME(client_create);

  Socket client = client_create.get();

  Future<Socket> socket = server.get().accept();

  const Try<Address> server_address = server.get().address();
  ASSERT_SOME(server_address);

  const Future<Nothing> connect = client.connect(server_address.get());

  AWAIT_ASSERT_READY(socket);
  AWAIT_ASSERT_READY(connect);

  const Try<Address> socket_address = socket.get().address();
  ASSERT_SOME(socket_address);

  // Ensure the client thinks its peer is the server.
  ASSERT_SOME_EQ(socket_address.get(), client.peer());

  // Ensure the client has an address, and that the server thinks its
  // peer is the client.
  ASSERT_SOME(client.address());
  ASSERT_SOME_EQ(client.address().get(), socket.get().peer());
}

#endif // USE_SSL_SOCKET
