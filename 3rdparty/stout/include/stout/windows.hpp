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

#ifndef __STOUT_WINDOWS_HPP__
#define __STOUT_WINDOWS_HPP__

#include <direct.h>   // For `_mkdir`.
#include <errno.h>    // For `_set_errno`.
#include <fcntl.h>    // For file access flags like `_O_CREAT`.
#include <io.h>       // For `_read`, `_write`.
#include <process.h>  // For `_getpid`.
#include <stdlib.h>   // For `_PATH_MAX`.

#include <sys/stat.h> // For permissions flags.

#include <BaseTsd.h>  // For `SSIZE_T`.
// We include `Winsock2.h` before `Windows.h` explicitly to avoid symbold
// re-definitions. This is a known pattern in the windows community.
#include <WS2tcpip.h>
#include <Winsock2.h>
#include <mswsock.h>
#include <winioctl.h>
#include <Windows.h>

#include <memory>

#include <glog/logging.h>


#if !defined(_UNICODE) || !defined(UNICODE)
// Much of the Windows API is available both in `string` and `wstring`
// varieties. To avoid polluting the namespace with two versions of every
// function, a common pattern in the Windows headers is to offer a single macro
// that expands to the `string` or `wstring` version, depending on whether the
// `_UNICODE` and `UNICODE` preprocessor symbols are set. For example,
// `GetMessage` will expand to either `GetMessageA` (the `string` version) or
// `GetMessageW` (the `wstring` version) depending on whether these symbols are
// defined.
//
// Unfortunately the `string` version is not UTF-8, like a developer would
// expect on Linux, but is instead the current Windows code page, and thus may
// take a different value at runtime. This makes it potentially difficult to
// decode, whereas the `wstring` version is always encoded as UTF-16, and thus
// can be programatically converted to UTF-8 using the `stringify()` function
// (converting UTF-8 to UTF-16 is done with `wide_stringify()`).
//
// Furthermore, support for NTFS long paths requires the use of the `wstring`
// APIs, coupled with the `\\?\` long path prefix marker for paths longer than
// the max path limit. This is accomplished by wrapping paths sent to Windows
// APIs with the `internal::windows::longpath()` helper. In order to prevent
// future regressions, we enforce the definitions of `_UNICODE` and `UNICODE`.
//
// NOTE: The Mesos code should always use the explicit `W` suffixed APIs in
// order to avoid type ambiguity.
#error "Mesos must be built with `_UNICODE` and `UNICODE` defined."
#endif // !defined(_UNICODE) || !defined(UNICODE)

// An RAII `HANDLE`.
class SharedHandle : public std::shared_ptr<void>
{
  static_assert(std::is_same<HANDLE, void*>::value,
                "Expected `HANDLE` to be of type `void*`.");

public:
  // We delete the default constructor so that the callsite is forced to make
  // an explicit decision about what the empty `HANDLE` value should be, as it
  // is not the same for all `HANDLE` types.  For example, `OpenProcess`
  // returns a `nullptr` for an invalid handle, but `CreateFile` returns an
  // `INVALID_HANDLE_VALUE` instead. This inconsistency is inherent in the
  // Windows API.
  SharedHandle() = delete;

  template <typename Deleter>
  SharedHandle(HANDLE handle, Deleter deleter)
      : std::shared_ptr<void>(handle, deleter) {}

  HANDLE get_handle() const { return this->get(); }
};


// Definitions and constants used for Windows compat.
//
// Gathers most of the Windows-compatibility definitions.  This makes it
// possible for files throughout the codebase to remain relatively free of all
// the #if's we'd need to make them work.
//
// Roughly, the things that should go in this file are definitions and
// declarations that one would not mind being:
//   * in global scope.
//   * globally available throughout both the Stout codebase, and any code
//     that uses it (such as Mesos).


// This code un-defines the global `GetMessage` macro defined by the Windows
// headers, and replaces it with an inline function that is equivalent.
//
// There are two reasons for doing this. The first is because this macro
// interferes with `google::protobufs::Reflection::GetMessage`. Replacing the
// `GetMessage` macro with an inline function allows people calling the
// `GetMessage` macro to carry on doing so with no code changes, but it will
// also allow us to use `google::protobufs::Reflection::GetMessage` without
// interference from the macro.
//
// The second is because we don't want to obliterate the `GetMessage` macro for
// people who include this header, either on purpose, or incidentally as part
// of some other Mesos header. The effect is that our need to call protobuf's
// `GetMessage` function has no deleterious effect on customers of this API.
//
// NOTE: the Windows headers also don't use define-once semantics for the
// `GetMessage` macro. In particular, this means that every time you include
// `Winuser.h` and a `GetMessage` macro isn't defined, the Windows headers will
// redefine it for you. The impact of this is that you should re-un-define the
// macro every time you include `Windows.h`; since we should be including
// `Windows.h` only from this file, we un-define it just after we include
// `Windows.h`.
#ifdef GetMessage
inline BOOL GetMessageWindows(
    LPMSG lpMsg,
    HWND hWnd,
    UINT wMsgFilterMin,
    UINT wMsgFilterMax)
{
  return GetMessage(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
}
#undef GetMessage
inline BOOL GetMessage(
    LPMSG lpMsg,
    HWND hWnd,
    UINT wMsgFilterMin,
    UINT wMsgFilterMax)
{
  return GetMessageWindows(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
}
#endif // GetMessage


// Define constants used for Windows compat. Allows a lot of code on
// Windows and POSIX systems to be the same, because we can pass the
// same constants to functions we call to do things like file I/O.
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define R_OK 0x4
#define W_OK 0x2
#define X_OK 0x0 // No such permission on Windows.
#define F_OK 0x0

#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_RDWR _O_RDWR
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define O_APPEND _O_APPEND
#define O_CLOEXEC _O_NOINHERIT

#define MAXHOSTNAMELEN NI_MAXHOST

#define PATH_MAX _MAX_PATH

// NOTE: for details on what this value should be, consult [1], [2], and [3].
//
// [1] http://www.opensource.apple.com/source/gnumake/gnumake-119/make/w32/include/dirent.h
// [2] https://en.wikipedia.org/wiki/Comparison_of_file_systems#Limits
// [3] https://msdn.microsoft.com/en-us/library/930f87yf.aspx
const int NAME_MAX = PATH_MAX;

// Corresponds to `mode_t` defined in sys/types.h of the POSIX spec.
// See large "permissions API" comment below for an explanation of
// why this is an int instead of unsigned short (as is common on
// POSIX).
typedef int mode_t;

// `DWORD` is expected to be the type holding PIDs throughout the Windows API,
// including functions like `OpenProcess`.
typedef DWORD pid_t;

typedef int uid_t;
typedef int gid_t;

typedef SSIZE_T ssize_t;

// Socket flags. Define behavior of a socket when it (e.g.) shuts down. We map
// the Windows versions of these flags to their POSIX equivalents so we don't
// have to change any socket code.
constexpr int SHUT_RD = SD_RECEIVE;
constexpr int SHUT_WR = SD_SEND;
constexpr int SHUT_RDWR = SD_BOTH;
constexpr int MSG_NOSIGNAL = 0; // `SIGPIPE` signal does not exist on Windows.

// The following functions are usually macros on POSIX; we provide them here as
// functions to avoid having global macros lying around. Note that these
// operate on the `_stat` struct (a Windows version of the standard POSIX
// `stat` struct), of which the `st_mode` field is known to be an `int`.
inline bool S_ISDIR(const int mode)
{
  return (mode & S_IFMT) == S_IFDIR; // Directory.
}


inline bool S_ISREG(const int mode)
{
  return (mode & S_IFMT) == S_IFREG;  // File.
}


inline bool S_ISCHR(const int mode)
{
  return (mode & S_IFMT) == S_IFCHR;  // Character device.
}


inline bool S_ISFIFO(const int mode)
{
  return (mode & S_IFMT) == _S_IFIFO; // Pipe.
}


inline bool S_ISBLK(const int mode)
{
  return false;                       // Block special device.
}


inline bool S_ISSOCK(const int mode)
{
  return false;                       // Socket.
}


inline bool S_ISLNK(const int mode)
{
  return false;                       // Symbolic link.
}

// Permissions API. (cf. MESOS-3176 to track ongoing permissions work.)
//
// We are currently able to emulate a subset of the POSIX permissions model
// with the Windows model:
//   [x] User write permissions.
//   [x] User read permissions.
//   [ ] User execute permissions.
//   [ ] Group permissions of any sort.
//   [ ] Other permissions of any sort.
//   [x] Flags to control "fallback" behavior (e.g., we might choose
//       to fall back to user readability when the user passes the
//       group readability flag in, since we currently do not support
//       group readability).
//
//
// Rationale:
// Windows currently implements two permissions models: (1) an extremely
// primitive permission model it largely inherited from DOS, and (2) the Access
// Control List (ACL) API. Because there is no trivial way to map the classic
// POSIX model into the ACL model, we have implemented POSIX-style permissions
// in terms of the DOS model. The result is the permissions limitations above.
//
//
// Flag implementation:
// Flags fall into the following two categories.
//   (1) Flags which exist in both permission models, but which have
//       different names (e.g., `S_IRUSR` in POSIX is called `_S_IREAD` on
//       Windows). In this case, we define the POSIX name to be the Windows
//       value (e.g., we define `S_IRUSR` to have the same value as `_S_IREAD`),
//       so that we can pass the POSIX name into Windows functions like
//       `_open`.
//   (2) Flags which exist only on POSIX (e.g., `S_IXUSR`). Here we
//       define the POSIX name to be the value given in the glibc
//       documentation[1], shifted left by 16 bits (since `mode_t`
//       is unsigned short on POSIX and `int` on Windows). We give these
//       flags glibc values to stay consistent, and so that existing
//       calls to functions like `open` do not break when they try to
//       use a flag that doesn't exist on Windows. But, of course,
//       these flags do not affect the execution of these functions.
//
//
// Flag strictness:
// Because the current implementation does not directly support setting or
// getting group or other permission bits on the Windows platform, there is a
// question of what we should fall back to when these flags are passed in to
// Stout methods.
//
// TODO(hausdorff): Investigate permissions mappings.
// We force "strictness" of the permission flag semantics:
//   * The group permissions flags will not fall back to anything, and will be
//     completely ignored.
//   * Other permissions: Same as above, but with other permissions.
//
//
// Execute permissions:
// Because DOS has no notion of "execute permissions", we define execute
// permissions to be read permissions. This is not ideal, but it is closest to
// being accurate.
//
//
// [1] http://www.delorie.com/gnu/docs/glibc/libc_288.html


// User permission flags.
const mode_t S_IRUSR = mode_t(_S_IREAD);  // Readable by user.
const mode_t S_IWUSR = mode_t(_S_IWRITE); // Writeable by user.
const mode_t S_IXUSR = S_IRUSR;           // Fallback to user read.
const mode_t S_IRWXU = S_IRUSR | S_IWUSR | S_IXUSR;


// Group permission flags. Lossy mapping to Windows permissions. See
// note above about flag strictness for explanation.
const mode_t S_IRGRP = 0x00200000;        // No-op.
const mode_t S_IWGRP = 0x00100000;        // No-op.
const mode_t S_IXGRP = 0x00080000;        // No-op.
const mode_t S_IRWXG = S_IRGRP | S_IWGRP | S_IXGRP;


// Other permission flags. Lossy mapping to Windows permissions. See
// note above about flag stictness for explanation.
const mode_t S_IROTH = 0x00040000;        // No-op.
const mode_t S_IWOTH = 0x00020000;        // No-op.
const mode_t S_IXOTH = 0x00010000;        // No-op.
const mode_t S_IRWXO = S_IROTH | S_IWOTH | S_IXOTH;


// Flags for set-ID-on-exec.
const mode_t S_ISUID = 0x08000000;        // No-op.
const mode_t S_ISGID = 0x04000000;        // No-op.
const mode_t S_ISVTX = 0x02000000;        // No-op.


// Flags not supported by Windows.
const mode_t O_SYNC     = 0x00000000;     // No-op.
const mode_t O_NONBLOCK = 0x00000000;     // No-op.

// Linux signal flags not used in Windows. We define them per
// `Linux sys/signal.h` to branch properly for Windows
//  processes' stop, resume and kill.
const mode_t SIGCONT = 0x00000009;     // Signal Cont.
const mode_t SIGSTOP = 0x00000011;     // Signal Stop.
const mode_t SIGKILL = 0x00000013;     // Signal Kill.

inline auto strerror_r(int errnum, char* buffer, size_t length) ->
decltype(strerror_s(buffer, length, errnum))
{
  return strerror_s(buffer, length, errnum);
}


// NOTE: Signals do not exist on Windows, so all signals are unknown.
// If the signal number is unknown, the Posix specification leaves the
// return value of `strsignal` unspecified.
inline const char* strsignal(int signum)
{
  static const char UNKNOWN_STRSIGNAL[] = "Unknown signal";
  return UNKNOWN_STRSIGNAL;
}


#define SIGPIPE 100

// `os::system` returns -1 if the processor cannot be started
// therefore any return value indicates the process has been started
#ifndef WIFEXITED
#define WIFEXITED(x) ((x) != -1)
#endif // WIFEXITED

// Returns the exit status of the child.
#ifndef WEXITSTATUS
#define WEXITSTATUS(x) (x & 0xFF)
#endif // WEXITSTATUS

#ifndef WIFSIGNALED
#define WIFSIGNALED(x) ((x) != -1)
#endif // WIFSIGNALED

// Returns the number of the signal that caused the child process to
// terminate, only be used if WIFSIGNALED is true.
#ifndef WTERMSIG
#define WTERMSIG(x) 0
#endif // WTERMSIG

// Whether the child produced a core dump, only be used if WIFSIGNALED is true.
#ifndef WCOREDUMP
#define WCOREDUMP(x) false
#endif // WCOREDUMP

// Whether the child was stopped by delivery of a signal.
#ifndef WIFSTOPPED
#define WIFSTOPPED(x) false
#endif // WIFSTOPPED

// Whether the child was stopped by delivery of a signal.
#ifndef WSTOPSIG
#define WSTOPSIG(x) 0
#endif // WSTOPSIG

// Specifies that `::waitpid` should return immediately rather than
// blocking and waiting for child to notify of state change.
#ifndef WNOHANG
#define WNOHANG 1
#endif // WNOHANG

#ifndef WUNTRACED
#define WUNTRACED   2 // Tell about stopped, untraced children.
#endif // WUNTRACED

#endif // __STOUT_WINDOWS_HPP__
