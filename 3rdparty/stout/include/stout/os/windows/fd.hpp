// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_OS_WINDOWS_FD_HPP__
#define __STOUT_OS_WINDOWS_FD_HPP__

#include <windows.h>
#include <WinSock2.h>

#include <array>
#include <memory>
#include <ostream>

#include <stout/check.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

namespace os {

// The `WindowsFD` class exists to provide an common interface with the POSIX
// file descriptor. While the bare `int` representation of the POSIX file
// descriptor API is undesirable, we rendezvous there in order to maintain the
// existing code in Mesos.
//
// In the platform-agnostic code paths, the `int_fd` type is aliased to
// `WindowsFD`. The `os::*` functions return a type appropriate to the platform,
// which allows us to write code like this:
//
//   Try<int_fd> fd = os::open(...);
//
// The `WindowsFD` constructs off one of:
//   (1) `int` - from the WinCRT API
//   (2) `HANDLE` - from the Win32 API
//   (3) `SOCKET` - from the WinSock API
//
// The `os::*` functions then take an instance of `WindowsFD`, examines
// the state and dispatches to the appropriate API.

class WindowsFD
{
public:
  enum Type
  {
    FD_CRT,
    FD_HANDLE,
    FD_SOCKET
  };

  WindowsFD() = default;

  WindowsFD(int crt)
    : type_(FD_CRT),
      crt_(crt),
      handle_(
          crt < 0 ? INVALID_HANDLE_VALUE
                  : reinterpret_cast<HANDLE>(::_get_osfhandle(crt))) {}

  // IMPORTANT: The `HANDLE` here is expected to be file handles. Specifically,
  //            `HANDLE`s returned by file API such as `CreateFile`. There are
  //            APIs that return `HANDLE`s with different error values, and
  //            therefore must be handled accordingly. For example, a thread API
  //            such as `CreateThread` returns `NULL` as the error value, rather
  //            than `INVALID_HANDLE_VALUE`.
  // TODO(mpark): Consider adding a second parameter which tells us what the
  //              error values are.
  WindowsFD(HANDLE handle)
    : type_(FD_HANDLE),
      crt_(
          handle == INVALID_HANDLE_VALUE
            ? -1
            : ::_open_osfhandle(reinterpret_cast<intptr_t>(handle), O_RDWR)),
      handle_(handle) {}

  WindowsFD(SOCKET socket) : type_(FD_SOCKET), socket_(socket) {}

  WindowsFD(const WindowsFD&) = default;
  WindowsFD(WindowsFD&&) = default;

  ~WindowsFD() = default;

  WindowsFD& operator=(const WindowsFD&) = default;
  WindowsFD& operator=(WindowsFD&&) = default;

  int crt() const
  {
    CHECK((type() == FD_CRT) || (type() == FD_HANDLE));
    return crt_;
  }

  operator HANDLE() const
  {
    CHECK((type() == FD_CRT) || (type() == FD_HANDLE));
    return handle_;
  }

  operator SOCKET() const
  {
    CHECK_EQ(FD_SOCKET, type());
    return socket_;
  }

  // On Windows, libevent's `evutil_socket_t` is set to `intptr_t`.
  operator intptr_t() const
  {
    CHECK_EQ(FD_SOCKET, type());
    return static_cast<intptr_t>(socket_);
  }

  Type type() const { return type_; }

private:
  Type type_;

  union
  {
    // We keep both a CRT FD as well as a `HANDLE`
    // regardless of whether we were constructed
    // from a file or a handle.
    //
    // This is because once we request for a CRT FD
    // from a `HANDLE`, we're required to close it
    // via `_close`. If we were to do the conversion
    // lazily upon request, the resulting CRT FD
    // would be dangling.
    struct
    {
      int crt_;
      HANDLE handle_;
    };
    SOCKET socket_;
  };
};


inline std::ostream& operator<<(std::ostream& stream, const WindowsFD& fd)
{
  switch (fd.type()) {
    case WindowsFD::FD_CRT: {
      stream << fd.crt();
      break;
    }
    case WindowsFD::FD_HANDLE: {
      stream << static_cast<HANDLE>(fd);
      break;
    }
    case WindowsFD::FD_SOCKET: {
      stream << static_cast<SOCKET>(fd);
      break;
    }
  }
  return stream;
}


// The complexity in this function is due to our effort in trying to support the
// cases where file descriptors are compared as an `int` on POSIX. For example,
// we use expressions such as `fd < 0` to check for validity.
// TODO(mpark): Consider introducing an `is_valid` function for `int_fd`.
inline bool operator<(const WindowsFD& left, const WindowsFD& right)
{
  // In general, when compared against a `WindowsFD` in the `FD_CRT`, we map
  // `INVALID_HANDLE_VALUE` and `INVALID_SOCKET` to `-1` before performing the
  // comparison. The check for `< 0` followed by cast to `HANDLE` or `SOCKET` is
  // due to the fact that `HANDLE` and `SOCKET` are both unsigned.
  switch (left.type()) {
    case WindowsFD::FD_CRT: {
      switch (right.type()) {
        case WindowsFD::FD_CRT: {
          return left.crt() < right.crt();
        }
        case WindowsFD::FD_HANDLE: {
          if (static_cast<HANDLE>(right) == INVALID_HANDLE_VALUE) {
            return left.crt() < -1;
          }
          if (left.crt() < 0) {
            return true;
          }
#pragma warning(push)
#pragma warning(disable : 4312)
          // Disable `int`-to-`HANDLE` compiler warning. This is safe to do,
          // see comment above.
          return reinterpret_cast<HANDLE>(left.crt()) <
                 static_cast<HANDLE>(right);
#pragma warning(pop)
        }
        case WindowsFD::FD_SOCKET: {
          if (static_cast<SOCKET>(right) == INVALID_SOCKET) {
            return left.crt() < -1;
          }
          if (left.crt() < 0) {
            return true;
          }
          return static_cast<SOCKET>(left.crt()) < static_cast<SOCKET>(right);
        }
      }
    }
    case WindowsFD::FD_HANDLE: {
      switch (right.type()) {
        case WindowsFD::FD_CRT: {
          if (static_cast<HANDLE>(left) == INVALID_HANDLE_VALUE) {
            return -1 < right.crt();
          }
          if (right.crt() < 0) {
            return false;
          }
#pragma warning(push)
#pragma warning(disable : 4312)
          // Disable `int`-to-`HANDLE` compiler warning. This is safe to do,
          // see comment above.
          return static_cast<HANDLE>(left) <
                 reinterpret_cast<HANDLE>(right.crt());
#pragma warning(pop)
        }
        case WindowsFD::FD_HANDLE: {
          return static_cast<HANDLE>(left) < static_cast<HANDLE>(right);
        }
        case WindowsFD::FD_SOCKET: {
          return static_cast<HANDLE>(left) <
                 reinterpret_cast<HANDLE>(static_cast<SOCKET>(right));
        }
      }
    }
    case WindowsFD::FD_SOCKET: {
      switch (right.type()) {
        case WindowsFD::FD_CRT: {
          if (static_cast<SOCKET>(left) == INVALID_SOCKET) {
            return -1 < right.crt();
          }
          if (right.crt() < 0) {
            return false;
          }
          return static_cast<SOCKET>(left) < static_cast<SOCKET>(right.crt());
        }
        case WindowsFD::FD_HANDLE: {
          return reinterpret_cast<HANDLE>(static_cast<SOCKET>(left)) <
                 static_cast<HANDLE>(right);
        }
        case WindowsFD::FD_SOCKET: {
          return static_cast<SOCKET>(left) < static_cast<SOCKET>(right);
        }
      }
    }
  }
  UNREACHABLE();
}


inline bool operator<(int left, const WindowsFD& right)
{
  return WindowsFD(left) < right;
}


inline bool operator<(const WindowsFD& left, int right)
{
  return left < WindowsFD(right);
}


inline bool operator>(const WindowsFD& left, const WindowsFD& right)
{
  return right < left;
}


inline bool operator>(int left, const WindowsFD& right)
{
  return WindowsFD(left) > right;
}


inline bool operator>(const WindowsFD& left, int right)
{
  return left > WindowsFD(right);
}


inline bool operator<=(const WindowsFD& left, const WindowsFD& right)
{
  return !(left > right);
}


inline bool operator<=(int left, const WindowsFD& right)
{
  return WindowsFD(left) <= right;
}


inline bool operator<=(const WindowsFD& left, int right)
{
  return left <= WindowsFD(right);
}


inline bool operator>=(const WindowsFD& left, const WindowsFD& right)
{
  return !(left < right);
}


inline bool operator>=(int left, const WindowsFD& right)
{
  return WindowsFD(left) >= right;
}


inline bool operator>=(const WindowsFD& left, int right)
{
  return left >= WindowsFD(right);
}


// The complexity in this function is due to our effort in trying to support the
// cases where file descriptors are compared as an `int` on POSIX. For example,
// we use expressions such as `fd != -1` to check for validity.
// TODO(mpark): Consider introducing an `is_valid` function for `int_fd`.
inline bool operator==(const WindowsFD& left, const WindowsFD& right)
{
  // In general, when compared against a `WindowsFD` in the `FD_CRT`, we map
  // `INVALID_HANDLE_VALUE` and `INVALID_SOCKET` to `-1` before performing the
  // comparison. The check for `< 0` followed by cast to `HANDLE` or `SOCKET` is
  // due to the fact that `HANDLE` and `SOCKET` are both unsigned.
  switch (left.type()) {
    case WindowsFD::FD_CRT: {
      switch (right.type()) {
        case WindowsFD::FD_CRT: {
          return left.crt() == right.crt();
        }
        case WindowsFD::FD_HANDLE: {
          if (static_cast<HANDLE>(right) == INVALID_HANDLE_VALUE) {
            return left.crt() == -1;
          }
          if (left.crt() < 0) {
            return false;
          }
#pragma warning(push)
#pragma warning(disable : 4312)
          // Disable `int`-to-`HANDLE` compiler warning. This is safe to do,
          // see comment above.
          return reinterpret_cast<HANDLE>(left.crt()) ==
                 static_cast<HANDLE>(right);
#pragma warning(pop)
        }
        case WindowsFD::FD_SOCKET: {
          if (static_cast<SOCKET>(right) == INVALID_SOCKET) {
            return left.crt() == -1;
          }
          if (left.crt() < 0) {
            return false;
          }
          return static_cast<SOCKET>(left.crt()) == static_cast<SOCKET>(right);
        }
      }
    }
    case WindowsFD::FD_HANDLE: {
      switch (right.type()) {
        case WindowsFD::FD_CRT: {
          if (static_cast<HANDLE>(left) == INVALID_HANDLE_VALUE) {
            return -1 == right.crt();
          }
          if (right.crt() < 0) {
            return false;
          }
#pragma warning(push)
#pragma warning(disable : 4312)
          // Disable `int`-to-`HANDLE` compiler warning. This is safe to do,
          // see comment above.
          return static_cast<HANDLE>(left) ==
                 reinterpret_cast<HANDLE>(right.crt());
#pragma warning(pop)
        }
        case WindowsFD::FD_HANDLE: {
          return static_cast<HANDLE>(left) == static_cast<HANDLE>(right);
        }
        case WindowsFD::FD_SOCKET: {
          return static_cast<HANDLE>(left) ==
                 reinterpret_cast<HANDLE>(static_cast<SOCKET>(right));
        }
      }
    }
    case WindowsFD::FD_SOCKET: {
      switch (right.type()) {
        case WindowsFD::FD_CRT: {
          if (static_cast<SOCKET>(left) == INVALID_SOCKET) {
            return -1 == right.crt();
          }
          if (right.crt() < 0) {
            return false;
          }
          return static_cast<SOCKET>(left) == static_cast<SOCKET>(right.crt());
        }
        case WindowsFD::FD_HANDLE: {
          return reinterpret_cast<HANDLE>(static_cast<SOCKET>(left)) ==
                 static_cast<HANDLE>(right);
        }
        case WindowsFD::FD_SOCKET: {
          return static_cast<SOCKET>(left) == static_cast<SOCKET>(right);
        }
      }
    }
  }
  UNREACHABLE();
}


inline bool operator==(int left, const WindowsFD& right)
{
  return WindowsFD(left) == right;
}


inline bool operator==(const WindowsFD& left, int right)
{
  return left == WindowsFD(right);
}


inline bool operator!=(const WindowsFD& left, const WindowsFD& right)
{
  return !(left == right);
}


inline bool operator!=(int left, const WindowsFD& right)
{
  return WindowsFD(left) != right;
}


inline bool operator!=(const WindowsFD& left, int right)
{
  return left != WindowsFD(right);
}

} // namespace os {

namespace std {

template <>
struct hash<os::WindowsFD>
{
  using argument_type = os::WindowsFD;
  using result_type = size_t;

  result_type operator()(const argument_type& fd) const
  {
    switch (fd.type()) {
      case os::WindowsFD::FD_CRT: {
        return static_cast<result_type>(fd.crt());
      }
      case os::WindowsFD::FD_HANDLE: {
        return reinterpret_cast<result_type>(static_cast<HANDLE>(fd));
      }
      case os::WindowsFD::FD_SOCKET: {
        return static_cast<result_type>(static_cast<SOCKET>(fd));
      }
    }
    UNREACHABLE();
  }
};

} // namespace std {

#endif // __STOUT_OS_WINDOWS_FD_HPP__
