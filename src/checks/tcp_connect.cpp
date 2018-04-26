// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __WINDOWS__
#include <unistd.h>

#include <arpa/inet.h> // for `inet_pton()`

#include <netinet/in.h> // for `sockaddr_in`

#include <sys/types.h>
#include <sys/socket.h>
#endif // __WINDOWS__

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

#include <process/address.hpp>
#include <process/network.hpp>

#include <stout/flags.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>

#include <stout/os/close.hpp>
#include <stout/os/socket.hpp>

using std::cerr;
using std::cout;
using std::endl;
using std::string;

// This binary tries to establish a TCP connection to given <ip>:<port>
// and can be used as a TCP health checker. It returns `EXIT_SUCCESS` iff
// the TCP handshake was successful and `EXIT_FAILURE` otherwise.
//
// TODO(alexr): Support TCP half-open, see MESOS-6116.
//
// TODO(alexr): Add support for Windows, see MESOS-6117.
//
// NOTE: Consider using stout network abstractions instead of raw system
// sockets. Once stout supports IPv6 migrating to it will buy us implicit
// Windows and IPv6 compatibilities.


class Flags : public virtual flags::FlagsBase
{
public:
  Flags()
  {
    add(&Flags::ip,
        "ip",
        "IP of the target host.");

    add(&Flags::port,
        "port",
        "Port to connect to.");
  }

  Option<string> ip;
  Option<int> port;
};


// Tries to establish a TCP connection to `ip`:`port`.
// If the TCP handshake is successful, returns `EXIT_SUCCESS`.
int testTCPConnect(const string& ip, int port)
{
  Try<net::IP> parse = net::IP::parse(ip);
  if (parse.isError()){
    cerr << "Cannot convert '" << ip << "' into a network address" << endl;
    return EXIT_FAILURE;
  }

  // Create a TCP socket.
  Try<int_fd> socket = net::socket(parse->family(), SOCK_STREAM, 0);
  if (socket.isError()) {
    cerr << "Failed to create socket: " << socket.error() << endl;
    return EXIT_FAILURE;
  }

  // Try to connect to socket. If the connection succeeds,
  // zero is returned, indicating the remote port is open.
  cout << "Connecting to " << ip << ":" << port << endl;
  Try<Nothing, SocketError> connect = process::network::connect(
      socket.get(),
      process::network::inet::Address(parse.get(), port));

  if (connect.isError()) {
    cerr << connect.error().message << endl;
    os::close(socket.get());
    return EXIT_FAILURE;
  }

  cout << "Successfully established TCP connection" << endl;

  shutdown(socket.get(), SHUT_RDWR);
  os::close(socket.get());

  return EXIT_SUCCESS;
}


int main(int argc, char *argv[])
{
  Flags flags;

  Try<flags::Warnings> load = flags.load(None(), argc, argv);

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    cerr << warning.message << endl;
  }

  if (flags.ip.isNone()) {
    cerr << flags.usage("Missing required option --ip") << endl;
    return EXIT_FAILURE;
  }

  if (flags.port.isNone()) {
    cerr << flags.usage("Missing required option --port") << endl;
    return EXIT_FAILURE;
  }

#ifdef __WINDOWS__
  if (!net::wsa_initialize()) {
    cerr << "WSA failed to initialize" << endl;
    return EXIT_FAILURE;
  }
#endif // __WINDOWS__

  int result = testTCPConnect(flags.ip.get(), flags.port.get());

#ifdef __WINDOWS__
  if (!net::wsa_cleanup()) {
    cerr << "Failed to finalize the WSA socket stack" << endl;
    return EXIT_FAILURE;
  }
#endif // __WINDOWS__

  return result;
}
