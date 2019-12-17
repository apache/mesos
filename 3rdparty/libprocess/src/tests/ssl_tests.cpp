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

#include <stdio.h>

#include <map>
#include <string>
#include <vector>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/network.hpp>
#include <process/socket.hpp>
#include <process/subprocess.hpp>

#include <process/ssl/gtest.hpp>
#include <process/ssl/utilities.hpp>

#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "openssl.hpp"

using std::map;
using std::string;
using std::vector;

// We only run these tests if we have configured with '--enable-ssl'.
#ifdef USE_SSL_SOCKET

#if OPENSSL_VERSION_NUMBER >= 0x10002000L
#define HOSTNAME_VALIDATION_OPENSSL
#endif

namespace http = process::http;
namespace io = process::io;
namespace network = process::network;
namespace openssl = network::openssl;
#ifndef __WINDOWS__
namespace unix = process::network::unix;
#endif // __WINDOWS__

using network::inet::Address;
using network::inet::Socket;

using network::internal::SocketImpl;

using process::Clock;
using process::Failure;
using process::Future;
using process::Subprocess;


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
  Try<int_fd> dup = os::dup(subprocess.out().get());
  if (dup.isError()) {
    return Failure(dup.error());
  }

  int_fd out = dup.get();

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


// The SSL protocols that we support through configuration flags.
static const vector<string> protocols = {
  // OpenSSL can be compiled with SSLV3 disabled completely, so we
  // conditionally test for this protocol.
#ifndef OPENSSL_NO_SSL3
  "LIBPROCESS_SSL_ENABLE_SSL_V3",
#endif
  "LIBPROCESS_SSL_ENABLE_TLS_V1_0",
  "LIBPROCESS_SSL_ENABLE_TLS_V1_1",
  "LIBPROCESS_SSL_ENABLE_TLS_V1_2",
// On some platforms, we need to build against OpenSSL versions that
// do not support TLS 1.3 yet.
#ifdef SSL_OP_NO_TLSv1_3
  "LIBPROCESS_SSL_ENABLE_TLS_V1_3",
#endif
};

static const vector<string> hostname_validation_schemes = {
  "legacy",
#ifdef HOSTNAME_VALIDATION_OPENSSL
  "openssl",
#endif
};


// Ensure that we can't create an SSL socket when SSL is not enabled.
TEST(SSL, Disabled)
{
  os::setenv("LIBPROCESS_SSL_ENABLED", "false");
  openssl::reinitialize();
  EXPECT_ERROR(Socket::create(SocketImpl::Kind::SSL));
}


// Test a basic back-and-forth communication using the 'ssl-client'
// subprocess.
TEST_F(SSLTest, SSLSocket)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()}},
      server.get(),
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server->accept();
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
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()}},
      server.get(),
      false);
  ASSERT_SOME(client);

  Future<Socket> socket = server->accept();
  AWAIT_ASSERT_FAILED(socket);

  AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
}


class SSLTestStringParameter
  : public SSLTest,
    public ::testing::WithParamInterface<std::string> {};


INSTANTIATE_TEST_CASE_P(HostnameValidationScheme,
                        SSLTestStringParameter,
                        ::testing::ValuesIn(hostname_validation_schemes));


// Ensure that a certificate that was not generated using the
// certificate authority is still allowed to communicate as long as
// the LIBPROCESS_SSL_VERIFY_SERVER_CERT and LIBPROCESS_SSL_REQUIRE_CLIENT_CERT
// flags are disabled.
TEST_P(SSLTestStringParameter, NoVerifyBadCA)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_REQUIRE_CLIENT_CERT", "false"},
      {"LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME", GetParam()}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", scrap_key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", scrap_certificate_path().string()},
      {"LIBPROCESS_SSL_VERIFY_SERVER_CERT", "false"},
      {"LIBPROCESS_SSL_CA_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME", GetParam()}},
      server.get(),
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server->accept();
  AWAIT_ASSERT_READY(socket);

  // TODO(jmlvanre): Remove const copy.
  AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
  AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

  AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
}


// Ensure that a client certificate that was not generated using the
// certificate authority is NOT allowed to communicate when the
// LIBPROCESS_SSL_REQUIRE_CLIENT_CERT flag is enabled.
//
// NOTE: We cannot run this test with the 'legacy' hostname
// validation scheme due to MESOS-9867.
#ifdef HOSTNAME_VALIDATION_OPENSSL
TEST_F(SSLTest, RequireBadCA)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_REQUIRE_CLIENT_CERT", "true"},
      {"LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME", "openssl"}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", scrap_key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", scrap_certificate_path().string()},
      {"LIBPROCESS_SSL_VERIFY_SERVER_CERT", "false"},
      {"LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME", "openssl"}},
      server.get(),
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server->accept();
  AWAIT_ASSERT_FAILED(socket);

  AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
}
#endif // HOSTNAME_VALIDATION_OPENSSL


// Ensure that a server certificate that was not generated using the
// certificate authority is NOT allowed to communicate when the
// LIBPROCESS_SSL_VERIFY_SERVER_CERT flag is enabled.
TEST_P(SSLTestStringParameter, VerifyBadCA)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", scrap_key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", scrap_certificate_path().string()},
      {"LIBPROCESS_SSL_REQUIRE_CLIENT_CERT", "false"},
      {"LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME", GetParam()}});
  ASSERT_SOME(server);

  Try<std::string> hostname = net::getHostname(process::address().ip);
  ASSERT_SOME(hostname);

  Try<Address> address = server->address();
  ASSERT_SOME(address);

  Try<Subprocess> client = launch_client({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_VERIFY_SERVER_CERT", "true"},
      {"LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME", GetParam()}},
      *hostname,
      address->ip,
      address->port,
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server->accept();
  AWAIT_ASSERT_FAILED(socket);

  AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
}


// Ensure that a certificate that WAS generated using the certificate
// authority IS allowed to communicate when the
// LIBPROCESS_SSL_VERIFY_SERVER_CERT and LIBPROCESS_SSL_REQUIRE_CLIENT_CERT
// flags are enabled.
//
// NOTE: If this test is failing for the 'legacy' scheme, subsequent
// tests may be affected due to MESOS-9867.
TEST_P(SSLTestStringParameter, VerifyCertificate)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_CA_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_REQUIRE_CLIENT_CERT", "true"},
      {"LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME", GetParam()}});
  ASSERT_SOME(server);

  Try<std::string> hostname = net::getHostname(process::address().ip);
  ASSERT_SOME(hostname);

  Try<Address> address = server->address();
  ASSERT_SOME(address);

  Try<Subprocess> client = launch_client({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_CA_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_VERIFY_SERVER_CERT", "true"},
      {"LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME", GetParam()}},
      *hostname,
      address->ip,
      address->port,
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server->accept();
  AWAIT_ASSERT_READY(socket);

  // TODO(jmlvanre): Remove const copy.
  AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
  AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

  AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
}


// Ensure that a server presenting a valid certificate with a not matching
// hostname is NOT allowed to communicate when the
// LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME flag is set to 'openssl'.
#ifdef HOSTNAME_VALIDATION_OPENSSL
TEST_F(SSLTest, HostnameMismatch)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_CA_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME", "openssl"}});
  ASSERT_SOME(server);

  Try<Address> address = server->address();
  ASSERT_SOME(address);

  Try<Subprocess> client = launch_client({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_CA_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_VERIFY_SERVER_CERT", "true"},
      {"LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME", "openssl"}},
      "invalid.example.org",
      address->ip,
      address->port,
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server->accept();
  AWAIT_ASSERT_FAILED(socket);

  AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
}
#endif // HOSTNAME_VALIDATION_OPENSSL


// Ensure that a server that attempts to present no certificate at all
// is NOT allowed to communicate when the LIBPROCESS_SSL_VERIFY_SERVER_CERT
// flag is enabled in the client.
TEST_F(SSLTest, NoAnonymousCipherIfVerify)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      // ADH stands for "Anonymous Diffie-Hellman", and is the only
      // anonymous cipher supported by OpenSSL out of the box.
      {"LIBPROCESS_SSL_CIPHERS", "ADH-AES256-SHA"}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_VERIFY_SERVER_CERT", "true"},
      {"LIBPROCESS_SSL_CIPHERS", "ADH-AES256-SHA"}},
      server.get(),
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server->accept();
  AWAIT_ASSERT_FAILED(socket);

  AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
}


// Ensure that key exchange using ECDHE algorithm works.
TEST_F(SSLTest, ECDHESupport)
{
  // Set up the default server environment variables.
  map<string, string> server_environment = {
    {"LIBPROCESS_SSL_ENABLED", "true"},
    {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
    {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
    {"LIBPROCESS_SSL_CIPHERS",
     "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-SHA256:ECDHE-RSA-AES128-SHA:"
     "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-SHA384:ECDHE-RSA-AES256-SHA"}
  };

  // Set up the default client environment variables.
  map<string, string> client_environment = {
    {"LIBPROCESS_SSL_ENABLED", "true"},
    {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
    {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
    {"LIBPROCESS_SSL_CIPHERS",
     "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-SHA256:ECDHE-RSA-AES128-SHA:"
     "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-SHA384:ECDHE-RSA-AES256-SHA"}
  };

  // Set up the server.
  Try<Socket> server = setup_server(server_environment);
  ASSERT_SOME(server);

  // Launch the client.
  Try<Subprocess> client =
      launch_client(client_environment, server.get(), true);
  ASSERT_SOME(client);

  Future<Socket> socket = server->accept();
  AWAIT_ASSERT_READY(socket);

  // TODO(jmlvanre): Remove const copy.
  AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
  AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

  AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
}


// TODO(josephw): Support downgrades on the native OpenSSL socket (MESOS-10073).
#ifdef USE_LIBEVENT
// Ensure we can communicate between a POLL based socket and an SSL
// socket if 'SSL_SUPPORT_DOWNGRADE' is enabled.
TEST_F(SSLTest, ValidDowngrade)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_SUPPORT_DOWNGRADE", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_REQUIRE_CLIENT_CERT", "true"}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"LIBPROCESS_SSL_ENABLED", "false"}},
      server.get(),
      false);
  ASSERT_SOME(client);

  Future<Socket> socket = server->accept();
  AWAIT_ASSERT_READY(socket);

  // TODO(jmlvanre): Remove const copy.
  AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
  AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

  AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
}


// Ensure we CANNOT communicate between a POLL based socket and an
// SSL socket if 'SSL_SUPPORT_DOWNGRADE' is not enabled.
TEST_F(SSLTest, NoValidDowngrade)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_SUPPORT_DOWNGRADE", "false"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_REQUIRE_CLIENT_CERT", "true"}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"LIBPROCESS_SSL_ENABLED", "false"}},
      server.get(),
      false);
  ASSERT_SOME(client);

  Future<Socket> socket = server->accept();
  AWAIT_ASSERT_FAILED(socket);

  AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
}


// For each protocol: ensure we can communicate between a POLL based
// socket and an SSL socket if 'SSL_SUPPORT_DOWNGRADE' is enabled.
TEST_F(SSLTest, ValidDowngradeEachProtocol)
{
  // For each protocol.
  foreach (const string& server_protocol, protocols) {
    LOG(INFO) << "Testing server protocol '" << server_protocol << "'\n";

    // Set up the default server environment variables.
    map<string, string> server_environment = {
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_SUPPORT_DOWNGRADE", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()}
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
        {"LIBPROCESS_SSL_ENABLED", "false"}},
        server.get(),
        false);
    ASSERT_SOME(client);

    Future<Socket> socket = server->accept();
    AWAIT_ASSERT_READY(socket);

    // TODO(jmlvanre): Remove const copy.
    AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
    AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

    AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
  }
}
#endif // USE_LIBEVENT


// For each protocol: ensure we CANNOT communicate between a POLL
// based socket and an SSL socket if 'SSL_SUPPORT_DOWNGRADE' is not
// enabled.
TEST_F(SSLTest, NoValidDowngradeEachProtocol)
{
  // For each protocol.
  foreach (const string& server_protocol, protocols) {
    LOG(INFO) << "Testing server protocol '" << server_protocol << "'\n";

    // Set up the default server environment variables.
    map<string, string> server_environment = {
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_SUPPORT_DOWNGRADE", "false"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()}
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
        {"LIBPROCESS_SSL_ENABLED", "false"}},
        server.get(),
        false);
    ASSERT_SOME(client);

    Future<Socket> socket = server->accept();
    AWAIT_ASSERT_FAILED(socket);

    AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
  }
}


// Verify that the 'peer()' address call works correctly.
TEST_F(SSLTest, PeerAddress)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()}});
  ASSERT_SOME(server);

  const Try<Socket> client_create = Socket::create(SocketImpl::Kind::SSL);
  ASSERT_SOME(client_create);

  Socket client = client_create.get();

  Future<Socket> socket = server->accept();

  const Try<Address> server_address = server->address();
  ASSERT_SOME(server_address);

  // Pass `None()` as hostname because this test is still
  // using the 'legacy' hostname validation scheme.
  const Future<Nothing> connect = client.connect(
      server_address.get(),
      openssl::create_tls_client_config(None()));

  AWAIT_ASSERT_READY(socket);
  AWAIT_ASSERT_READY(connect);

  const Try<Address> socket_address = socket->address();
  ASSERT_SOME(socket_address);

  // Ensure the client thinks its peer is the server.
  ASSERT_SOME_EQ(socket_address.get(), client.peer());

  // Ensure the client has an address, and that the server thinks its
  // peer is the client.
  ASSERT_SOME(client.address());
  ASSERT_SOME_EQ(client.address().get(), socket->peer());
}


// Basic Https GET test.
TEST_F(SSLTest, HTTPSGet)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()}});

  ASSERT_SOME(server);
  ASSERT_SOME(server->address());

  Try<std::string> serverHostname = server->address()->lookup_hostname();
  ASSERT_SOME(serverHostname);

  Future<Socket> socket = server->accept();

  // Create URL from server hostname and port.
  const http::URL url(
      "https", serverHostname.get(), server->address()->port);

  // Send GET request.
  Future<http::Response> response = http::get(url);

  AWAIT_ASSERT_READY(socket);

  // Construct response and send(server side).
  const string buffer =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(data.length()) + "\r\n" +
    "\r\n" +
    data;
  AWAIT_ASSERT_READY(Socket(socket.get()).send(buffer));

  AWAIT_ASSERT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  ASSERT_EQ(data, response->body);
}


// Basic Https POST test.
TEST_F(SSLTest, HTTPSPost)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()}});

  ASSERT_SOME(server);
  ASSERT_SOME(server->address());

  Try<std::string> serverHostname = server->address()->lookup_hostname();
  ASSERT_SOME(serverHostname);

  Future<Socket> socket = server->accept();

  // Create URL from server hostname and port.
  const http::URL url(
      "https", serverHostname.get(), server->address()->port);

  // Send POST request.
  Future<http::Response> response =
    http::post(url, None(), "payload", "text/plain");

  AWAIT_ASSERT_READY(socket);

  // Construct response and send(server side).
  const string buffer =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(data.length()) + "\r\n" +
    "\r\n" +
    data;
  AWAIT_ASSERT_READY(Socket(socket.get()).send(buffer));

  AWAIT_ASSERT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  ASSERT_EQ(data, response->body);
}


// This test ensures that if an SSL connection never sends any
// data (i.e. no handshake or downgrade completes), it will not
// impact our ability to accept additional connections.
// This test was added due to MESOS-5340.
TEST_F(SSLTest, SilentSocket)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()}});

  ASSERT_SOME(server);
  ASSERT_SOME(server->address());

  Try<std::string> serverHostname = server->address()->lookup_hostname();
  ASSERT_SOME(serverHostname);

  Future<Socket> socket = server->accept();

  // We initiate a connection on which we will not send
  // any data. This means the socket on the server will
  // not complete the SSL handshake, nor be downgraded.
  // As a result, we expect that the server will not see
  // an accepted socket for this connection.
  Try<Socket> connection = Socket::create(SocketImpl::Kind::POLL);
  ASSERT_SOME(connection);
  connection->connect(server->address().get());

  // Note that settling libprocess is not sufficient
  // for ensuring socket events are processed.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  ASSERT_TRUE(socket.isPending());

  // Now send an HTTP GET request, it should complete
  // without getting blocked by the socket above
  // undergoing the SSL handshake.
  const http::URL url(
      "https",
      serverHostname.get(),
      server->address()->port);

  Future<http::Response> response = http::get(url);

  AWAIT_READY(socket);

  // Send the response from the server.
  const string buffer = string() +
    "HTTP/1.1 200 OK\r\n" +
    "Content-Length: " + stringify(data.length()) + "\r\n" +
    "\r\n" +
    data;
  AWAIT_READY(Socket(socket.get()).send(buffer));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  EXPECT_EQ(data, response->body);
}


// This test was added due to an OOM issue: MESOS-7934.
TEST_F(SSLTest, ShutdownThenSend)
{
  Clock::pause();

  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()}});

  ASSERT_SOME(server);
  ASSERT_SOME(server->address());

  Future<Socket> socket = server->accept();

  Clock::settle();
  EXPECT_TRUE(socket.isPending());

  Try<Socket> client = Socket::create(SocketImpl::Kind::SSL);
  ASSERT_SOME(client);

  // Pass `None()` as hostname because this test is still
  // using the 'legacy' hostname validation scheme.
  AWAIT_ASSERT_READY(client->connect(
      server->address().get(),
      openssl::create_tls_client_config(None())));

  AWAIT_ASSERT_READY(socket);

  EXPECT_SOME(Socket(socket.get()).shutdown());

  // This send should fail now that the socket is shut down.
  AWAIT_FAILED(Socket(socket.get()).send("Hello World"));
}


#endif // USE_SSL_SOCKET


class SSLVerifyIPAddTest : public SSLTest,
                           public ::testing::WithParamInterface<const char*> {};


INSTANTIATE_TEST_CASE_P(SSLVerifyIPAdd,
                        SSLVerifyIPAddTest,
                        ::testing::Values("false", "true"));


// Test a basic back-and-forth communication within the same OS
// process.
TEST_P(SSLVerifyIPAddTest, BasicSameProcess)
{
  os::setenv("LIBPROCESS_SSL_ENABLED", "true");
  os::setenv("LIBPROCESS_SSL_KEY_FILE", key_path().string());
  os::setenv("LIBPROCESS_SSL_CERT_FILE", certificate_path().string());
  os::setenv("LIBPROCESS_SSL_REQUIRE_CLIENT_CERT", "true");
  os::setenv("LIBPROCESS_SSL_CA_DIR", os::getcwd());
  os::setenv("LIBPROCESS_SSL_CA_FILE", certificate_path().string());
  os::setenv("LIBPROCESS_SSL_VERIFY_IPADD", GetParam());
  os::setenv("LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME", "legacy");

  openssl::reinitialize();

  Try<Socket> server = Socket::create(SocketImpl::Kind::SSL);
  ASSERT_SOME(server);

  Try<Socket> client = Socket::create(SocketImpl::Kind::SSL);
  ASSERT_SOME(client);

  // We need to explicitly bind to the address advertised by libprocess so the
  // certificate we create in this test fixture can be verified.
  ASSERT_SOME(server->bind(Address(net::IP(process::address().ip), 0)));
  ASSERT_SOME(server->listen(BACKLOG));

  Try<Address> address = server->address();
  ASSERT_SOME(address);

  Future<Socket> accept = server->accept();

  // Pass `None()` as hostname because this test is still
  // using the 'legacy' hostname validation scheme.
  AWAIT_ASSERT_READY(client->connect(
      address.get(),
      openssl::create_tls_client_config(None())));

  // Wait for the server to have accepted the client connection.
  AWAIT_ASSERT_READY(accept);

  Socket socket = accept.get();

  // Send a message from the client to the server.
  const string data = "Hello World!";
  AWAIT_ASSERT_READY(client->send(data));

  // Verify the server received the message.
  AWAIT_ASSERT_EQ(data, socket.recv(data.size()));

  // Send the message back from the server to the client.
  AWAIT_ASSERT_READY(socket.send(data));

  // Verify the client received the message.
  AWAIT_ASSERT_EQ(data, client->recv(data.size()));
}


#ifndef __WINDOWS__
TEST_P(SSLVerifyIPAddTest, BasicSameProcessUnix)
{
  os::setenv("LIBPROCESS_SSL_ENABLED", "true");
  os::setenv("LIBPROCESS_SSL_KEY_FILE", key_path().string());
  os::setenv("LIBPROCESS_SSL_CERT_FILE", certificate_path().string());
  // NOTE: we must set LIBPROCESS_SSL_REQUIRE_CLIENT_CERT to false because we
  // don't have a hostname or IP to verify!
  os::setenv("LIBPROCESS_SSL_REQUIRE_CLIENT_CERT", "false");
  os::setenv("LIBPROCESS_SSL_CA_DIR", os::getcwd());
  os::setenv("LIBPROCESS_SSL_CA_FILE", certificate_path().string());
  os::setenv("LIBPROCESS_SSL_VERIFY_IPADD", GetParam());

  openssl::reinitialize();

  Try<unix::Socket> server = unix::Socket::create(SocketImpl::Kind::SSL);
  ASSERT_SOME(server);

  Try<unix::Socket> client = unix::Socket::create(SocketImpl::Kind::SSL);
  ASSERT_SOME(client);

  // Use a path in the temporary directory so it gets cleaned up.
  string path = path::join(sandbox.get(), "socket");

  Try<unix::Address> address = unix::Address::create(path);
  ASSERT_SOME(address);

  ASSERT_SOME(server->bind(address.get()));
  ASSERT_SOME(server->listen(BACKLOG));

  Future<unix::Socket> accept = server->accept();

  // Pass `None()` as hostname because this test is still
  // using the 'legacy' hostname validation scheme.
  AWAIT_ASSERT_READY(client->connect(
      address.get(),
      openssl::create_tls_client_config(None())));

  // Wait for the server to have accepted the client connection.
  AWAIT_ASSERT_READY(accept);

  unix::Socket socket = accept.get();

  // Send a message from the client to the server.
  const string data = "Hello World!";
  AWAIT_ASSERT_READY(client->send(data));

  // Verify the server received the message.
  AWAIT_ASSERT_EQ(data, socket.recv(data.size()));

  // Send the message back from the server to the client.
  AWAIT_ASSERT_READY(socket.send(data));

  // Verify the client received the message.
  AWAIT_ASSERT_EQ(data, client->recv(data.size()));
}
#endif // __WINDOWS__


// Ensure that a certificate that WAS generated using the certificate
// authority IS allowed to communicate when the
// LIBPROCESS_SSL_REQUIRE_CLIENT_CERT flag is enabled.
TEST_P(SSLVerifyIPAddTest, RequireCertificate)
{
  Try<Socket> server = setup_server({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_CA_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_REQUIRE_CLIENT_CERT", "true"},
      {"LIBPROCESS_SSL_VERIFY_IPADD", GetParam()}});
  ASSERT_SOME(server);

  Try<Subprocess> client = launch_client({
      {"LIBPROCESS_SSL_ENABLED", "true"},
      {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
      {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_CA_FILE", certificate_path().string()},
      {"LIBPROCESS_SSL_REQUIRE_CLIENT_CERT", "true"},
      {"LIBPROCESS_SSL_VERIFY_IPADD", GetParam()}},
      server.get(),
      true);
  ASSERT_SOME(client);

  Future<Socket> socket = server->accept();
  AWAIT_ASSERT_READY(socket);

  // TODO(jmlvanre): Remove const copy.
  AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
  AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

  AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
}


class SSLProtocolTest
  : public SSLTest,
    public ::testing::WithParamInterface<std::tuple<string, string>> {};


INSTANTIATE_TEST_CASE_P(
    SSLProtocol,
    SSLProtocolTest,
    ::testing::Combine(
        ::testing::ValuesIn(protocols), ::testing::ValuesIn(protocols)));


// Test all the combinations of protocols. Ensure that they can only
// communicate if the opposing end allows the given protocol, and not
// otherwise.
TEST_P(SSLProtocolTest, Mismatch)
{
  const string& server_protocol = std::get<0>(GetParam());
  const string& client_protocol = std::get<1>(GetParam());

  LOG(INFO) << "Testing server protocol '" << server_protocol
    << "' with client protocol '" << client_protocol << "'\n";

  // Set up the default server environment variables.
  map<string, string> server_environment = {
    {"LIBPROCESS_SSL_ENABLED", "true"},
    {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
    {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()}
  };

  // Set up the default client environment variables.
  map<string, string> client_environment = {
    {"LIBPROCESS_SSL_ENABLED", "true"},
    {"LIBPROCESS_SSL_KEY_FILE", key_path().string()},
    {"LIBPROCESS_SSL_CERT_FILE", certificate_path().string()},
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
    Future<Socket> socket = server->accept();
    AWAIT_ASSERT_READY(socket);

    // TODO(jmlvanre): Remove const copy.
    AWAIT_ASSERT_EQ(data, Socket(socket.get()).recv());
    AWAIT_ASSERT_READY(Socket(socket.get()).send(data));

    AWAIT_ASSERT_READY(await_subprocess(client.get(), 0));
  } else {
    // If the protocols are NOT the same, it is invalid.
    Future<Socket> socket = server->accept();
    AWAIT_ASSERT_FAILED(socket);

    // Pass `None()` as hostname because this test is still
    // using the 'legacy' hostname validation scheme.
    AWAIT_ASSERT_READY(await_subprocess(client.get(), None()));
  }
}


// Verify that we can make a connection using a custom SSL context,
// and that the specified `verify` and `configure_socket` callbacks
// are called.
TEST_F(SSLTest, CustomSSLContext)
{
  static bool verify_called;
  static bool configure_socket_called;

  os::setenv("LIBPROCESS_SSL_ENABLED", "true");
  os::setenv("LIBPROCESS_SSL_KEY_FILE", key_path().string());
  os::setenv("LIBPROCESS_SSL_CERT_FILE", certificate_path().string());

  openssl::reinitialize();

  verify_called = false;
  configure_socket_called = false;

  SSL_CTX* ctx = SSL_CTX_new(SSLv23_client_method());
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

  openssl::TLSClientConfig config(
    None(),
    ctx,
    [](SSL*, const network::Address&, const Option<std::string>&)
      -> Try<Nothing>
    {
      configure_socket_called = true;
      return Nothing();
    },
    [](const SSL* const, const Option<std::string>&, const Option<net::IP>&)
      -> Try<Nothing>
    {
      verify_called = true;
      return Nothing();
    });

  Try<Socket> client = Socket::create(SocketImpl::Kind::SSL);
  ASSERT_SOME(client);

  Try<Socket> server = Socket::create(SocketImpl::Kind::SSL);
  ASSERT_SOME(server);

  server->listen(1);
  Try<Address> address = server->address();
  ASSERT_SOME(address);

  Future<Socket> socket = server->accept();
  Future<Nothing> connected = client->connect(*address, config);

  AWAIT_READY(socket);
  AWAIT_READY(connected);

  EXPECT_TRUE(verify_called);
  EXPECT_TRUE(configure_socket_called);
}


// Ensures that `connect()` fails if the passed
// `configure_socket` callback returns an error.
TEST_F(SSLTest, CustomSSLContextConfigureSocketFails)
{
  os::setenv("LIBPROCESS_SSL_ENABLED", "true");
  os::setenv("LIBPROCESS_SSL_KEY_FILE", key_path().string());
  os::setenv("LIBPROCESS_SSL_CERT_FILE", certificate_path().string());

  openssl::reinitialize();

  SSL_CTX* ctx = SSL_CTX_new(SSLv23_client_method());
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

  openssl::TLSClientConfig config(
    None(),
    ctx,
    [](SSL*, const network::Address&, const Option<std::string>&)
      -> Try<Nothing>
    {
      return Error("Configure socket.");
    },
    [](const SSL* const, const Option<std::string>&, const Option<net::IP>&)
      -> Try<Nothing>
    {
      return Nothing();
    });

  Try<Socket> client = Socket::create(SocketImpl::Kind::SSL);
  ASSERT_SOME(client);

  Try<Socket> server = Socket::create(SocketImpl::Kind::SSL);
  ASSERT_SOME(server);

  server->listen(1);
  Try<Address> address = server->address();
  ASSERT_SOME(address);

  Future<Socket> socket = server->accept();
  Future<Nothing> connected = client->connect(*address, config);

  AWAIT_ASSERT_FAILED(connected);
}


// Ensures that `connect()` fails if the passed
// `verify` callback returns an error.
TEST_F(SSLTest, CustomSSLContextVerifyFails)
{
  os::setenv("LIBPROCESS_SSL_ENABLED", "true");
  os::setenv("LIBPROCESS_SSL_KEY_FILE", key_path().string());
  os::setenv("LIBPROCESS_SSL_CERT_FILE", certificate_path().string());

  openssl::reinitialize();

  SSL_CTX* ctx = SSL_CTX_new(SSLv23_client_method());
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

  openssl::TLSClientConfig config(
    None(),
    ctx,
    [](SSL*, const network::Address&, const Option<std::string>&)
      -> Try<Nothing>
    {
      return Nothing();
    },
    [](const SSL* const, const Option<std::string>&, const Option<net::IP>&)
      -> Try<Nothing>
    {
      return Error("Verify failed.");
    });

  Try<Socket> client = Socket::create(SocketImpl::Kind::SSL);
  ASSERT_SOME(client);

  Try<Socket> server = Socket::create(SocketImpl::Kind::SSL);
  ASSERT_SOME(server);

  server->listen(1);
  Try<Address> address = server->address();
  ASSERT_SOME(address);

  Future<Socket> socket = server->accept();
  Future<Nothing> connected = client->connect(*address, config);

  AWAIT_ASSERT_FAILED(connected);
}
