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

#include <process/address.hpp>
#include <process/future.hpp>
#include <process/message.hpp>
#include <process/pid.hpp>
#include <process/socket.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>

#include "encoder.hpp"

using process::Future;
using process::Message;
using process::MessageEncoder;
using process::UPID;

using process::network::inet::Address;
using process::network::inet::Socket;


static const int LISTEN_BACKLOG = 10;
static Socket* __s__;


// Handler for the server socket.
//
// NOTE: This keeps all incoming sockets open forever because the link
// tests need "half-open" connections for some tests. A connection
// becomes "half-open" when either the read or write direction of the
// socket is closed via a `::shutdown()`. Normally, the peer will
// discover a "half-open" connection if a read/write returns data of
// length zero.
//
// The test-linkee does not act on this information in order to give
// the link tests full control over the lifetime of sockets.
void on_accept(const Future<Socket>& incoming)
{
  if (incoming.isReady()) {
    // NOTE: We copy and explicitly leak the socket here.
    // `Socket` is a shared pointer that closes itself after the
    // reference count goes to zero. By leaking the socket here, we
    // ensure that each incoming socket is never closed by the linkee.
    new Socket(incoming.get());

    const size_t size = 1024;
    char* data = new char[size];

    incoming->recv(data, size)
      .then([data](const size_t size) -> Future<Nothing> {
        delete[] data;

        // If there was any content at all, assume this is a message
        // telling the linkee to terminate.
        if (size > 0) {
          EXIT(EXIT_SUCCESS);
        }

        return Nothing();
      });
  }

  __s__->accept()
    .onAny(lambda::bind(&on_accept, lambda::_1));
}


/**
 * This process provides a target for testing remote link semantics
 * in libprocess.
 *
 * When this process starts up, it sends a message to the provided UPID.
 * This message is a notification that the test-linkee is ready to be
 * linked against and also allows the parent to discover the linkee's UPID.
 *
 * In order to test "stale" links, this process will exit upon receiving
 * a message. This gives a clear signal that a message was received.
 */
int main(int argc, char** argv)
{
  if (argc <= 1) {
    EXIT(EXIT_FAILURE) << "Usage: test-linkee <UPID>";
  }

  // Create a server socket.
  Try<Socket> create = Socket::create();
  if (create.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create server socket: " << create.error();
  }
  __s__ = new Socket(create.get());

  // Allow address reuse.
  // NOTE: We cast to `char*` here because the function prototypes on Windows
  // use `char*` instead of `void*`.
  int on = 1;
  if (::setsockopt(
          __s__->get(),
          SOL_SOCKET,
          SO_REUSEADDR,
          reinterpret_cast<char*>(&on),
          sizeof(on)) < 0) {
    EXIT(EXIT_FAILURE)
      << "Failed to setsockopt(SO_REUSEADDR): " << ErrnoError().message;
  }

  // Bind to some random port.
  Try<Address> bind = __s__->bind(Address::ANY_ANY());
  if (bind.isError()) {
    EXIT(EXIT_FAILURE) << "Failed to bind: " << bind.error();
  }

  Address address = bind.get();

  // Start listening for incoming sockets.
  Try<Nothing> listen = __s__->listen(LISTEN_BACKLOG);
  if (listen.isError()) {
    EXIT(EXIT_FAILURE) << "Failed to listen: " << listen.error();
  }

  __s__->accept()
    .onAny(lambda::bind(&on_accept, lambda::_1));

  // Send a message to the parent to say the linkee is ready to be linked
  // against, and to broadcast the linkee's UPID.
  Try<Socket> outgoing = Socket::create();
  if (outgoing.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create outgoing socket: " << outgoing.error();
  }

  UPID parent(argv[1]);
  outgoing->connect(parent.address)
    .onAny([address, outgoing, parent](const Future<Nothing>& future) mutable {
      if (!future.isReady()) {
        EXIT(EXIT_FAILURE)
          << "Failed to connect to parent: "
          << (future.isFailed() ? future.failure() : "discarded");
      }

      // Pretend to be a proper libprocess process.
      Message message;
      message.name = "Alive";
      message.from = UPID("(1)", address);
      message.to = parent;

      outgoing->send(MessageEncoder::encode(&message));
    });

  // Now sit and accept links until the linkee is killed.
  while (true) {
    os::sleep(Seconds(1));
  }

  return EXIT_FAILURE;
}
