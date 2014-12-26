#ifndef __PROCESS_NETWORK_HPP__
#define __PROCESS_NETWORK_HPP__

#include <process/node.hpp>

#include <stout/net.hpp>
#include <stout/try.hpp>

namespace process {
namespace network {

// Returns a socket file descriptor for the specified options. Note
// that on OS X, the returned socket will have the SO_NOSIGPIPE option
// set.
inline Try<int> socket(int family, int type, int protocol)
{
  int s;
  if ((s = ::socket(family, type, protocol)) == -1) {
    return ErrnoError();
  }

#ifdef __APPLE__
  // Disable SIGPIPE via setsockopt because OS X does not support
  // the MSG_NOSIGNAL flag on send(2).
  const int enable = 1;
  if (setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(int)) == -1) {
    return ErrnoError();
  }
#endif // __APPLE__

  return s;
}

// accept, bind, connect, getsockname wrappers for different protocol families
inline Try<int> accept(int s, sa_family_t family)
{
  switch (family) {
    case AF_INET: {
      sockaddr_in addr = net::createSockaddrIn(0, 0);
      socklen_t addrlen = sizeof(addr);

      int accepted = ::accept(s, (sockaddr*) &addr, &addrlen);
      if (accepted < 0) {
        return ErrnoError("Failed to accept");
      }

      return accepted;
    }
    default:
      return Error("Unsupported family type: " + stringify(family));
  }
}


inline Try<int> bind(int s, const Node& node)
{
  sockaddr_in addr = net::createSockaddrIn(node.ip, node.port);

  int error = ::bind(s, (sockaddr*) &addr, sizeof(addr));
  if (error < 0) {
    return ErrnoError("Failed to bind on " + stringify(node));
  }

  return error;
}


inline Try<int> connect(int s, const Node& node)
{
  sockaddr_in addr = net::createSockaddrIn(node.ip, node.port);

  int error = ::connect(s, (sockaddr*) &addr, sizeof(addr));
  if (error < 0) {
    return ErrnoError("Failed to connect to " + stringify(node));
  }

  return error;
}


inline Try<Node> getsockname(int s, sa_family_t family)
{
  switch (family) {
    case AF_INET: {
      sockaddr_in addr = net::createSockaddrIn(0, 0);
      socklen_t addrlen = sizeof(addr);

      if(::getsockname(s, (sockaddr*) &addr, &addrlen) < 0) {
        return ErrnoError("Failed to getsockname");
      }

      return Node(addr.sin_addr.s_addr, ntohs(addr.sin_port));
    }
    default:
      return Error("Unsupported family type: " + stringify(family));
  }
}

} // namespace network {
} // namespace process {

#endif // __PROCESS_NETWORK_HPP__
