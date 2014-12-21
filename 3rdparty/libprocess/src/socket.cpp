#include <process/socket.hpp>

namespace process {
namespace network {

const Socket::Kind& Socket::DEFAULT_KIND()
{
  // TODO(jmlvanre): Change the default based on configure or
  // environment flags.
  static const Kind DEFAULT = POLL;
  return DEFAULT;
}

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
      return Socket(std::make_shared<Socket::Impl>(s));
    }
    // By not setting a default we leverage the compiler errors when
    // the enumeration is augmented to find all the cases we need to
    // provide.
  }
}

} // namespace network {
} // namespace process {
