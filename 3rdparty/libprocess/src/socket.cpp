#include <process/socket.hpp>

#include "poll_socket.hpp"

namespace process {
namespace network {

Try<Socket> Socket::create(Kind kind, int s)
{
  if (s < 0) {
    // Supported in Linux >= 2.6.27.
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    Try<int> fd =
      network::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if (fd.isError()) {
      return Error("Failed to create socket: " + fd.error());
    }
#else
    Try<int> fd = network::socket(AF_INET, SOCK_STREAM, 0);
    if (fd.isError()) {
      return Error("Failed to create socket: " + fd.error());
    }

    Try<Nothing> nonblock = os::nonblock(fd.get());
    if (nonblock.isError()) {
      return Error("Failed to create socket, nonblock: " + nonblock.error());
    }

    Try<Nothing> cloexec = os::cloexec(fd.get());
    if (cloexec.isError()) {
      return Error("Failed to create socket, cloexec: " + cloexec.error());
    }
#endif

    s = fd.get();
  }

  switch (kind) {
    case POLL: {
      Try<std::shared_ptr<Socket::Impl>> socket = PollSocketImpl::create(s);
      if (socket.isError()) {
        return Error(socket.error());
      }
      return Socket(socket.get());
    }
    // By not setting a default we leverage the compiler errors when
    // the enumeration is augmented to find all the cases we need to
    // provide.
  }
}


const Socket::Kind& Socket::DEFAULT_KIND()
{
  // TODO(jmlvanre): Change the default based on configure or
  // environment flags.
  static const Kind DEFAULT = POLL;
  return DEFAULT;
}


Try<Node> Socket::Impl::bind(const Node& node)
{
  Try<int> bind = network::bind(get(), node);
  if (bind.isError()) {
    return Error(bind.error());
  }

  // Lookup and store assigned IP and assigned port.
  return network::getsockname(get(), AF_INET);
}

} // namespace network {
} // namespace process {
