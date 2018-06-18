---
title: Apache Mesos - Developer Guide
layout: documentation
---

# Developer Guide

This document is distinct from the [C++ Style Guide](c++-style-guide.md) as it
covers best practices, design patterns, and other tribal knowledge, not just how
to format code correctly.

# General

## How to Navigate the Source

For a complete IDE-like experience, see the documentation on using
[cquery](cquery.md).

## When to Introduce Abstractions

Don't introduce an abstraction just for code de-duplication. Always think about
if the abstraction makes sense.

## Include What You Use

IWYU: the principle that if you use a type or symbol from a header file, that
header file should be included.

While IWYU should always be followed in C++, we have a problem specifically with
the `os` namespace. Originally, all functions like `os::realpath` were
implemented in `stout/os.hpp`. At some point, however, each of these were moved
to their own file (i.e. `stout/os/realpath.hpp`). Unfortunately, it is very easy
to use an `os` function without including its respective header because
`stout/posix/os.hpp` includes almost all of these headers. This tends to break
other platforms, as `stout/windows/os.hpp` _does not_ include all of these
headers. The guideline is to Include What You Use, especially for
`stout/os/*.hpp`.

## Error message reporting

The general pattern is to just include the reason for an error, and to not
include any information the caller already has, because otherwise the callers
will double log:

```
namespace os {

Try<Nothing> copyfile(string source, string destination)
{
  if (... copying failed ...) {
    return Error("Failed to copy '" + source + "' to '" + destination + "'");
  }

  return Nothing();
}

} // namespace os

Try<Nothing> copy = os::copyfile(source, destination);

if (copy.isError()) {
  return ("Failed to copy '" + source + "'"
          " to '" + destination + "': " + copy.error();
}
```

This would emit:

> Failed to copy 's' to 'd': Failed to copy 's' to 'd': No disk space left

A good way to think of this is: "what is the 'actual' error message?"

An error message consists of several parts, much like an exception: the "reason"
for the error, and multiple "stacks" of context. If you're referring to the
"reason" when you said "actual", both approaches (the one Mesos uses, or the
above example) include the reason in their returned error message. The
distinction lies in _where_ the "stacks" of context get included.

The decision Mesos took some time ago was to have the "owner" of the context be
responsible for including it. So if we call `os::copyfile` we know which
function we're calling and which `source` and `destination` we're passing into
it. This matches POSIX-style programming, which is likely why this approach was
chosen.

The POSIX-style code:

```
int main()
{
  int fd = open("/file");

  if (fd == -1) {
    // Caller logs the thing it was doing, and gets the reason for the error:
    LOG(ERROR) << "Failed to initialize: Failed to open '/file': " << strerror(errno);
  }
}
```

is similar to the following Mesos-style code:

```
int main()
{
  Try<int> fd = open("/file");

  if (fd.isError()) {
    // Caller logs the thing it was doing, and gets the reason for the error:
    LOG(ERROR) << "Failed to initialize: Failed to open '/file': " << fd.error();
  }
}
```

If we use the alternative approach to have the leaf include all the information
it has, then we have to compose differently:

```
int main()
{
  Try<int> fd = os::open("/file");

  if (fd.isError()) {
    // Caller knows that no additional context needs to be added because callee has all of it.
    LOG(ERROR) << "Failed to initialize: " << fd.error();
  }
}
```

The approach we chose was to treat the error as just the "reason" (much like
`strerror`), so if the caller wants to add context to it, they can. Both
approaches work, but we have to pick one and apply it consistently as best we
can. So don't add information to an error message that the caller already has.

# Windows

## Unicode

Mesos is explicitly compiled with `UNICODE` and `_UNICODE` preprocess
defintions, forcing the use of the wide `wchar_t` versions of ambiguous APIs.
Nonetheless, developers should be explicit when using an API: use
`::SetCurrentDirectoryW` over the ambiguous macro `::SetCurrentyDirectory`.

When converting from `std::string` to `std::wstring`, do not reinvent the wheel!
Use the `wide_stringify()` and `stringify()` functions from
[`stringify.hpp`](https://github.com/apache/mesos/blob/master/3rdparty/stout/include/stout/stringify.hpp).

## Long Path Support

Mesos has built-in NTFS long path support. On Windows, the usual maximum path is
about 255 characters (it varies per API). This is unusable because Mesos uses
directories with GUIDs, and easily exceeds this limitation. To support this, we
use the Unicode versions of the Windows APIs, and explicitly preprend the long
path marker `\\?\` to any path sent to these APIs.

The pattern, when using a Windows API which takes a path, is to:

1. Use the wide version of the API (suffixed with `W`).
2. Ensure the API supports long paths (check MSDN for the API).
3. Use `::internal::windows::longpath(std::string path)` to safely convert the path.
4. Only use the `longpath` for Windows APIs, or internal Windows API wrappers.

For an example, see
[`chdir.hpp`](https://github.com/apache/mesos/blob/master/3rdparty/stout/include/stout/os/windows/chdir.hpp).

The long path helper is found in
[`longpath.hpp`](https://github.com/apache/mesos/blob/master/3rdparty/stout/include/stout/internal/windows/longpath.hpp).

### Windows CRT

While it is tempting to use the Windows CRT to ease porting, we explicitly avoid
using it as much as possible for several reasons:

* It does not interact well with Windows APIs. For instance, an environment
  variable set by the Win32 API `SetEnvironmentVariable` will not be visible in
  the CRT API `environ`.

* The CRT APIs tend to be difficult to encapsulate properly with RAII.

* Parts of the CRT have been deprecated, and even more are marked unsafe.

It is almost always preferable to use Win32 APIs, which is akin to "Windows
system programming" rather than porting Mesos onto a POSIX compatibility layer.
It may not always be possible to avoid the CRT, but consider the implementation
carefully before using it.

## Handles

The Windows API is flawed and has multiple invalid semantic values for the
`HANDLE` type, i.e. some APIs return `-1` or `INVALID_HANDLE_VALUE`, and other
APIs return `nullptr`. It is simply
[inconsistent](https://blogs.msdn.microsoft.com/oldnewthing/20040302-00/?p=40443),
so developers must take extra caution when checking handles returned from the
Windows APIs. Please double check the documentation to determine which value
will indicate it is invalid.

Using raw handles (or indeed raw pointers anywhere) in C++ is treachorous. Mesos
has a `SafeHandle` class which should be used immediately when obtaining a
`HANDLE` from a Windows API, with the deleter likely set to `::CloseHandle`.

## Nano Server Compatibility

We would like to target Microsoft Nano Server. This means we are restricted to
the set of Windows APIs available on Nano,
[Nano Server APIs](https://msdn.microsoft.com/en-us/library/mt588480(v=vs.85).aspx).
An example of an *excluded and unavailable* set of APIs is `Shell32.dll` AKA
`<shlobj.h>`.
