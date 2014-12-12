#ifndef __PROCESS_SOCKET_HPP__
#define __PROCESS_SOCKET_HPP__

#include <assert.h>

#include <memory>

#include <process/future.hpp>
#include <process/node.hpp>

#include <stout/abort.hpp>
#include <stout/nothing.hpp>
#include <stout/net.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>


namespace process {

// Returns a socket fd for the specified options. Note that on OS X,
// the returned socket will have the SO_NOSIGPIPE option set.
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

  int error  = ::bind(s, (sockaddr*) &addr, sizeof(addr));
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

// An abstraction around a socket (file descriptor) that provides
// reference counting such that the socket is only closed (and thus,
// has the possiblity of being reused) after there are no more
// references.

class Socket
{
public:
  // Each socket is a reference counted, shared by default, concurrent
  // object. However, since we want to support multiple
  // implementations we use the Pimpl pattern (often called the
  // compilation firewall) rather than forcing each Socket
  // implementation to do this themselves.
  class Impl : public std::enable_shared_from_this<Impl>
  {
  public:
    Impl() : s(-1) {}

    explicit Impl(int _s) : s(_s) {}

    ~Impl()
    {
      if (s >= 0) {
        Try<Nothing> close = os::close(s);
        if (close.isError()) {
          ABORT("Failed to close socket " +
                stringify(s) + ": " + close.error());
        }
      }
    }

    int get() const
    {
      return s >= 0 ? s : create().get();
    }

    Future<Nothing> connect(const Node& node);

    Future<size_t> recv(char* data, size_t size);

    Future<size_t> send(const char* data, size_t size);

    Future<size_t> sendfile(int fd, off_t offset, size_t size);

    Try<Node> bind(const Node& node);

    Try<Nothing> listen(int backlog);

    Future<Socket> accept();

  private:
    const Impl& create() const
    {
      CHECK(s < 0);

      // Supported in Linux >= 2.6.27.
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
      Try<int> fd =
        process::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

      if (fd.isError()) {
        ABORT("Failed to create socket: " + fd.error());
      }
#else
      Try<int> fd = process::socket(AF_INET, SOCK_STREAM, 0);
      if (fd.isError()) {
        ABORT("Failed to create socket: " + fd.error());
      }

      Try<Nothing> nonblock = os::nonblock(fd.get());
      if (nonblock.isError()) {
        ABORT("Failed to create socket, nonblock: " + nonblock.error());
      }

      Try<Nothing> cloexec = os::cloexec(fd.get());
      if (cloexec.isError()) {
        ABORT("Failed to create socket, cloexec: " + cloexec.error());
      }
#endif

      s = fd.get();
      return *this;
    }

    // Mutable so that the socket can be lazily created.
    //
    // TODO(benh): Create a factory for sockets and don't lazily
    // create but instead return a Try<Socket> from the factory in
    // order to eliminate the need for a mutable member or the call to
    // ABORT above.
    mutable int s;
  };

  Socket() : impl(std::make_shared<Impl>()) {}

  explicit Socket(int s) : impl(std::make_shared<Impl>(s)) {}

  bool operator == (const Socket& that) const
  {
    return impl == that.impl;
  }

  operator int () const
  {
    return impl->get();
  }

  int get() const
  {
    return impl->get();
  }

  Future<Nothing> connect(const Node& node)
  {
    return impl->connect(node);
  }

  Future<size_t> recv(char* data, size_t size) const
  {
    return impl->recv(data, size);
  }

  Future<size_t> send(const char* data, size_t size) const
  {
    return impl->send(data, size);
  }

  Future<size_t> sendfile(int fd, off_t offset, size_t size) const
  {
    return impl->sendfile(fd, offset, size);
  }

  Try<Node> bind(const Node& node)
  {
    return impl->bind(node);
  }

  Try<Nothing> listen(int backlog)
  {
    return impl->listen(backlog);
  }

  Future<Socket> accept()
  {
    return impl->accept();
  }

private:
  explicit Socket(std::shared_ptr<Impl>&& that) : impl(std::move(that)) {}

  std::shared_ptr<Impl> impl;
};

} // namespace process {

#endif // __PROCESS_SOCKET_HPP__
