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

#include <stout/flags.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>

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
// TODO(alexr): Support IPv6, see MESOS-6120.
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
        "IP of the target host. Only IPv4 is supported.");

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
  // Set up destination address.
  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) {
    cerr << "Cannot convert '" << ip << "' into a network address" << endl;
    return EXIT_FAILURE;
  }

  // Create a TCP socket.
  int socket = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket < 0) {
    cerr << "Failed to create socket: " << strerror(errno) << endl;
    return EXIT_FAILURE;
  }

  // Try to connect to socket. If the connection succeeds,
  // zero is returned, indicating the remote port is open.
  cout << "Connecting to " << ip << ":" << port << endl;
  if (connect(socket, reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0) {
    cerr << "Connection failed: " << strerror(errno) << endl;
    close(socket);
    return EXIT_FAILURE;
  }

  cout << "Successfully established TCP connection" << endl;

  shutdown(socket, SHUT_RDWR);
  close(socket);

  return EXIT_SUCCESS;
}


int main(int argc, char *argv[])
{
#ifdef __WINDOWS__
  if (!net::wsa_initialize()) {
    EXIT(EXIT_FAILURE) << "WSA failed to initialize";
  }
#endif // __WINDOWS__

  Flags flags;

  Try<flags::Warnings> load = flags.load(None(), argc, argv);

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  foreach (const flags::Warning& warning, load->warnings) {
    cerr << warning.message << endl;
  }

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (flags.ip.isNone()) {
    cerr << flags.usage("Missing required option --ip") << endl;
    return EXIT_FAILURE;
  }

  if (flags.port.isNone()) {
    cerr << flags.usage("Missing required option --port") << endl;
    return EXIT_FAILURE;
  }

  int result = testTCPConnect(flags.ip.get(), flags.port.get());

#ifdef __WINDOWS__
  if (!net::wsa_cleanup()) {
    EXIT(EXIT_FAILURE) << "Failed to finalize the WSA socket stack";
  }
#endif // __WINDOWS__

  return result;
}
