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

#ifndef __PROCESS_SOCKET_HPP__
#define __PROCESS_SOCKET_HPP__

#ifndef __WINDOWS__
#include <sys/socket.h>
#include <sys/wait.h>
#endif // __WINDOWS__

#include <memory>

#include <process/address.hpp>
#include <process/future.hpp>

#include <stout/abort.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <stout/os/int_fd.hpp>


namespace process {
namespace network {
namespace internal {

/**
 * Implementation interface for a `Socket`.
 *
 * Each socket is:
 *   - reference counted,
 *   - shared by default,
 *   - and a concurrent object.
 *
 * Multiple implementations are supported via the Pimpl pattern,
 * rather than forcing each Socket implementation to do this themselves.
 *
 * @see process::network::Socket
 * @see [Pimpl pattern](https://en.wikipedia.org/wiki/Opaque_pointer)
 */
class SocketImpl : public std::enable_shared_from_this<SocketImpl>
{
public:
  /**
   * Available kinds of implementations.
   *
   * @see process::network::internal::PollSocketImpl
   * @see process::network::internal::LibeventSSLSocketImpl
   */
  enum class Kind
  {
    POLL,
#ifdef USE_SSL_SOCKET
    SSL
#endif
  };

  /**
   * Returns the default `Kind` of implementation.
   */
  static Kind DEFAULT_KIND();

  /**
   * Returns an instance of a `SocketImpl` using the specified kind of
   * implementation.
   *
   * @param s. The existing file descriptor to use.
   * @param kind Optional. The desired implementation.
   *
   * @return An instance of a `SocketImpl`.
   */
  static Try<std::shared_ptr<SocketImpl>> create(
      int_fd s,
      Kind kind = DEFAULT_KIND());

  /**
   * Returns an instance of a `SocketImpl` using the specified kind of
   * implementation. The NONBLOCK and CLOEXEC options will be set on
   * the underlying file descriptor for the socket.
   *
   * @param kind Optional. The desired implementation.
   *
   * @return An instance of a `SocketImpl`.
   */
  // TODO(josephw): MESOS-5729: Consider making the CLOEXEC option
  // configurable by the caller of the interface.
  static Try<std::shared_ptr<SocketImpl>> create(
      Address::Family family,
      Kind kind = DEFAULT_KIND());

  virtual ~SocketImpl()
  {
    // Don't close if the socket was released.
    if (s >= 0) {
      CHECK_SOME(os::close(s)) << "Failed to close socket";
    }
  }

  /**
   * Returns the file descriptor wrapped by this implementation.
   */
  int_fd get() const
  {
    return s;
  }

  /**
   * @copydoc network::address
   */
  Try<Address> address() const;

  /**
   * @copydoc network::peer
   */
  Try<Address> peer() const;

  /**
   * Assigns the specified address to the socket.
   *
   * @return The assigned `Address` or an error if the bind system
   *     call fails.
   */
  Try<Address> bind(const Address& address);

  virtual Try<Nothing> listen(int backlog) = 0;

  /**
   * Returns an implementation corresponding to the next pending
   * connection for the listening socket. All implementations will set
   * the NONBLOCK and CLOEXEC options on the returned socket.
   *
   * TODO(josephw): MESOS-5729: Consider making the CLOEXEC option
   * configurable by the caller of the interface.
   */
  virtual Future<std::shared_ptr<SocketImpl>> accept() = 0;

  virtual Future<Nothing> connect(const Address& address) = 0;
  virtual Future<size_t> recv(char* data, size_t size) = 0;
  virtual Future<size_t> send(const char* data, size_t size) = 0;
  virtual Future<size_t> sendfile(int_fd fd, off_t offset, size_t size) = 0;

  /**
   * An overload of `recv`, which receives data based on the specified
   * 'size' parameter.
   *
   * @param size
   *       Value  | Semantics
   *     :-------:|-----------
   *        0     | Returns an empty string.
   *       -1     | Receives until EOF.
   *        N     | Returns a string of size N.
   *       'None' | Returns a string of the available data.
   *     If 'None' is specified, whenever data becomes available on the
   *     socket, that much data will be returned.
   */
  // TODO(benh): Consider returning Owned<std::string> or
  // Shared<std::string>, the latter enabling reuse of a pool of
  // preallocated strings/buffers.
  virtual Future<std::string> recv(const Option<ssize_t>& size = None());

  /**
   * An overload of `send`, which sends all of the specified data.
   *
   * @param data The specified data to send.
   *
   * @return Nothing or an error in case the sending fails.
   */
  // TODO(benh): Consider taking Shared<std::string>, the latter
  // enabling reuse of a pool of preallocated strings/buffers.
  virtual Future<Nothing> send(const std::string& data);

  /**
   * Shuts down the socket. Accepts an integer which specifies the
   * shutdown mode.
   */
  virtual Try<Nothing> shutdown(int how)
  {
    if (::shutdown(s, how) < 0) {
      return ErrnoError();
    }

    return Nothing();
  }

  virtual Kind kind() const = 0;

protected:
  explicit SocketImpl(int_fd _s) : s(_s) { CHECK(s >= 0); }

  /**
   * Releases ownership of the file descriptor. Not exposed
   * via the `Socket` interface as this is only intended to
   * support `Socket::Impl` implementations that need to
   * override the file descriptor ownership.
   */
  int_fd release()
  {
    int_fd released = s;
    s = -1;
    return released;
  }

  /**
   * Returns a `std::shared_ptr<T>` from this implementation.
   */
  template <typename T>
  static std::shared_ptr<T> shared(T* t)
  {
    std::shared_ptr<T> pointer =
      std::dynamic_pointer_cast<T>(CHECK_NOTNULL(t)->shared_from_this());
    CHECK(pointer);
    return pointer;
  }

  int_fd s;
};


/**
 * An abstraction around a socket (file descriptor).
 *
 * Provides reference counting such that the socket is only closed
 * (and thus, has the possibility of being reused) after there are no
 * more references.
 */
template <typename AddressType>
class Socket
{
public:
  static_assert(
      std::is_convertible<AddressType, network::Address>::value,
      "Requires type convertible to `network::Address`");

  /**
   * Returns an instance of a `Socket` using the specified kind of
   * implementation.
   *
   * @param s Optional.  The file descriptor to wrap with the `Socket`.
   * @param kind Optional. The desired `Socket` implementation.
   *
   * @return An instance of a `Socket`.
   */
  static Try<Socket> create(
      int_fd s,
      SocketImpl::Kind kind = SocketImpl::DEFAULT_KIND())
  {
    Try<std::shared_ptr<SocketImpl>> impl = SocketImpl::create(s, kind);
    if (impl.isError()) {
      return Error(impl.error());
    }
    return Socket(impl.get());
  }

  /**
   * Returns an instance of a `Socket` using `AddressType` to determine
   * the address family to use. An optional implementation kind can be
   * specified. The NONBLOCK and CLOEXEC options will be set on the
   * underlying file descriptor for the socket.
   *
   * @param kind Optional. The desired `Socket` implementation.
   *
   * @return An instance of a `Socket`.
   */
  // TODO(josephw): MESOS-5729: Consider making the CLOEXEC option
  // configurable by the caller of the interface.
  static Try<Socket> create(SocketImpl::Kind kind = SocketImpl::DEFAULT_KIND());

  /**
   * Returns an instance of a `Socket` using the specified
   * `Address::Family` to determine the address family to use. An
   * optional implementation kind can be specified. The NONBLOCK and
   * CLOEXEC options will be set on the underlying file descriptor for
   * the socket.
   *
   * NOTE: this is only defined for `AddressType` of
   * `network::Address`, all others are explicitly deleted.
   *
   * @param kind Optional. The desired `Socket` implementation.
   *
   * @return An instance of a `Socket`.
   */
  static Try<Socket> create(
      Address::Family family,
      SocketImpl::Kind kind = SocketImpl::DEFAULT_KIND());

  /**
   * Returns the kind representing the underlying implementation
   * of the `Socket` instance.
   *
   * @see process::network::Socket::Kind
   */
  SocketImpl::Kind kind() const
  {
    return impl->kind();
  }

  bool operator==(const Socket& that) const
  {
    return impl == that.impl;
  }

  operator int_fd() const
  {
    return impl->get();
  }

  Try<AddressType> address() const
  {
    return convert<AddressType>(impl->address());
  }

  Try<AddressType> peer() const
  {
    return convert<AddressType>(impl->peer());
  }

  int_fd get() const
  {
    return impl->get();
  }

  Try<AddressType> bind(const AddressType& address)
  {
    return convert<AddressType>(impl->bind(address));
  }

  Try<Nothing> listen(int backlog)
  {
    return impl->listen(backlog);
  }

  Future<Socket> accept()
  {
    // NOTE: We save a reference to the listening socket itself
    // (i.e., 'this') so that we don't close the listening socket
    // while 'accept' is in flight.
    std::shared_ptr<SocketImpl> self = impl->shared_from_this();

    return impl->accept()
      // TODO(benh): Use && for `accepted` here!
      .then([self](const std::shared_ptr<SocketImpl>& accepted) {
        return Socket(accepted);
      });
  }

  Future<Nothing> connect(const AddressType& address)
  {
    return impl->connect(address);
  }

  Future<size_t> recv(char* data, size_t size) const
  {
    return impl->recv(data, size);
  }

  Future<size_t> send(const char* data, size_t size) const
  {
    return impl->send(data, size);
  }

  Future<size_t> sendfile(int_fd fd, off_t offset, size_t size) const
  {
    return impl->sendfile(fd, offset, size);
  }

  Future<std::string> recv(const Option<ssize_t>& size = None())
  {
    return impl->recv(size);
  }

  Future<Nothing> send(const std::string& data)
  {
    return impl->send(data);
  }

  enum class Shutdown
  {
    READ,
    WRITE,
    READ_WRITE
  };

  // TODO(benh): Replace the default to Shutdown::READ_WRITE or remove
  // all together since it's unclear what the default should be.
  Try<Nothing> shutdown(Shutdown shutdown = Shutdown::READ)
  {
    int how = [&]() {
      switch (shutdown) {
        case Shutdown::READ: return SHUT_RD;
        case Shutdown::WRITE: return SHUT_WR;
        case Shutdown::READ_WRITE: return SHUT_RDWR;
      }
      UNREACHABLE();
    }();

    return impl->shutdown(how);
  }

  // Support implicit conversion of any `Socket<AddressType>` to a
  // `Socket<network::Address>`.
  operator Socket<network::Address>() const
  {
    return Socket<network::Address>(impl);
  }

private:
  // Necessary to support the implicit conversion operator from any
  // `Socket<AddressType>` to `Socket<network::Address>`.
  template <typename T>
  friend class Socket;

  explicit Socket(std::shared_ptr<SocketImpl>&& that) : impl(std::move(that)) {}

  explicit Socket(const std::shared_ptr<SocketImpl>& that) : impl(that) {}

  std::shared_ptr<SocketImpl> impl;
};

} // namespace internal {


using Socket = network::internal::Socket<network::Address>;

namespace inet {
using Socket = network::internal::Socket<inet::Address>;
} // namespace inet {

#ifndef __WINDOWS__
namespace unix {
using Socket = network::internal::Socket<unix::Address>;
} // namespace unix {
#endif // __WINDOWS__


namespace internal {

template <>
Try<Socket<network::Address>> Socket<network::Address>::create(
    SocketImpl::Kind kind) = delete;


template <>
inline Try<Socket<network::Address>> Socket<network::Address>::create(
    Address::Family family,
    SocketImpl::Kind kind)
{
  Try<std::shared_ptr<SocketImpl>> impl = SocketImpl::create(family, kind);
  if (impl.isError()) {
    return Error(impl.error());
  }
  return Socket(impl.get());
}


template <>
inline Try<Socket<inet::Address>> Socket<inet::Address>::create(
    SocketImpl::Kind kind)
{
  // TODO(benh): Replace this function which defaults to IPv4 in
  // exchange for explicit IPv4 and IPv6 versions.
  Try<std::shared_ptr<SocketImpl>> impl =
    SocketImpl::create(Address::Family::INET4, kind);
  if (impl.isError()) {
    return Error(impl.error());
  }
  return Socket(impl.get());
}


template <>
Try<Socket<inet::Address>> Socket<inet::Address>::create(
    Address::Family family,
    SocketImpl::Kind kind) = delete;


#ifndef __WINDOWS__
template <>
inline Try<Socket<unix::Address>> Socket<unix::Address>::create(
    SocketImpl::Kind kind)
{
  Try<std::shared_ptr<SocketImpl>> impl =
    SocketImpl::create(Address::Family::UNIX, kind);
  if (impl.isError()) {
    return Error(impl.error());
  }
  return Socket(impl.get());
}


template <>
Try<Socket<unix::Address>> Socket<unix::Address>::create(
    Address::Family family,
    SocketImpl::Kind kind) = delete;
#endif // __WINDOWS__

} // namespace internal {
} // namespace network {
} // namespace process {

#endif // __PROCESS_SOCKET_HPP__
