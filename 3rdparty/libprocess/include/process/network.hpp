#ifndef __PROCESS_NETWORK_HPP__
#define __PROCESS_NETWORK_HPP__

#include <process/address.hpp>

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


// TODO(benh): Remove and defer to Socket::accept.
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


// TODO(benh): Remove and defer to Socket::bind.
inline Try<int> bind(int s, const Address& address)
{
  sockaddr_in addr = net::createSockaddrIn(address.ip, address.port);

  int error = ::bind(s, (sockaddr*) &addr, sizeof(addr));
  if (error < 0) {
    return ErrnoError("Failed to bind on " + stringify(address));
  }

  return error;
}


// TODO(benh): Remove and defer to Socket::connect.
inline Try<int> connect(int s, const Address& address)
{
  sockaddr_in addr = net::createSockaddrIn(address.ip, address.port);

  int error = ::connect(s, (sockaddr*) &addr, sizeof(addr));
  if (error < 0) {
    return ErrnoError("Failed to connect to " + stringify(address));
  }

  return error;
}


inline Try<Address> address(int s)
{
  union {
    struct sockaddr s;
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
  } addr;

  socklen_t addrlen = sizeof(addr);

  if (::getsockname(s, (sockaddr*) &addr, &addrlen) < 0) {
    return ErrnoError("Failed to getsockname");
  }

  if (addr.s.sa_family == AF_INET) {
    return Address(addr.v4.sin_addr.s_addr, ntohs(addr.v4.sin_port));
  }

  return Error("Unsupported IP address family '" +
               stringify(addr.s.sa_family) + "'");
}

} // namespace network {
} // namespace process {

#endif // __PROCESS_NETWORK_HPP__
