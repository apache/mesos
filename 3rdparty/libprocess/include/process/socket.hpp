#ifndef __PROCESS_SOCKET_HPP__
#define __PROCESS_SOCKET_HPP__

#include <assert.h>

#include <stout/abort.hpp>
#include <stout/memory.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>


namespace process {

// Returns a socket fd for the specified options. Note that on OS X,
// the returned socket will have the SO_NOSIGPIPE option set.
inline Try<int> socket(int family, int type, int protocol) {
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


// An abstraction around a socket (file descriptor) that provides
// reference counting such that the socket is only closed (and thus,
// has the possiblity of being reused) after there are no more
// references.

class Socket
{
public:
  class Impl
  {
  public:
    Impl() : s(-1) {}

    explicit Impl(int _s) : s(_s) {}

    ~Impl()
    {
      if (s >= 0) {
        Try<Nothing> close = os::close(s);
        if (close.isError()) {
          ABORT(
            "Failed to close socket " + stringify(s) + ": " + close.error());
        }
      }
    }

    int get() const
    {
      return s >= 0 ? s : create().get();
    }

  private:
    const Impl& create() const
    {
      CHECK(s < 0);
      Try<int> fd =
        process::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      if (fd.isError()) {
        ABORT("Failed to create socket: " + fd.error());
      }
      s = fd.get();
      return *this;
    }

    // Mutable so that the socket can be lazily created.
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

private:
  memory::shared_ptr<Impl> impl;
};

} // namespace process {

#endif // __PROCESS_SOCKET_HPP__
