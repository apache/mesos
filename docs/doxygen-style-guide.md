---
title: Apache Mesos - Doxygen Style Guide
layout: documentation
---

# Apache Mesos Doxygen Style Guide

This guide introduces a consistent style
for [documenting Mesos source code](http://mesos.apache.org/api/latest/c++)
using [Doxygen](http://www.doxygen.org).
There is an ongoing, incremental effort with the goal to document all public Mesos, libprocess, and stout APIs this way.
For now, existing code may not follow these guidelines, but new code should.


## Source Code Documentation Syntax

Doxygen documentation needs only to be applied to source code parts that
constitute an interface for which we want to generate Mesos API documentation
files. Implementation code that does not participate in this should still be
enhanced by source code comments as appropriate, but these comments should not follow the doxygen style.

We follow the [Javadoc syntax](http://en.wikipedia.org/wiki/Javadoc) to mark comment blocks.
These have the general form:


```cpp
/**
 * Brief summary.
 *
 * Detailed description. More detail.
 * @see Some reference
 *
 * @param <name> Parameter description.
 * @return Return value description.
 */
```

Example:

```cpp
/**
 * Returns a compressed version of a string.
 *
 * Compresses an input string using the foobar algorithm.
 *
 * @param uncompressed The input string.
 * @return A compressed version of the input string.
 */
std::string compress(const std::string& uncompressed);
```

### Doxygen Tags

This is the allowed set of doxygen tags that can be used.

 * [`@param`](http://doxygen.org/manual/commands.html#cmdparam) Describes function parameters.
 * [`@return`](http://doxygen.org/manual/commands.html#cmdreturn) Describes return values.
 * [`@see`](http://doxygen.org/manual/commands.html#cmdsa) Describes a cross-reference to classes, functions, methods, variables, files or URL.

Example:

```cpp
/**
 * Available kinds of implementations.
 *
 * @see process::network::PollSocketImpl
 */
```

 * [`@file`](http://doxygen.org/manual/commands.html#cmdfile) Describes a refence to a file. It is required when documenting global functions, variables, typedefs, or enums in separate files.
 * [`@link`](http://doxygen.org/manual/commands.html#cmdlink) and [`@endlink`](http://doxygen.org/manual/commands.html#cmdendlink) Describes a link to a file, class, or member.
 * [`@example`](http://doxygen.org/manual/commands.html#cmdexample) Describes source code examples.
 * [`@image`](http://doxygen.org/manual/commands.html#cmdimage) Describes an image.

> When following these links be aware that the Doxygen documentation uses the `\param` syntax rather than `@param`.


### Wrapping

We wrap long descriptions using four spaces on the next line:

```cpp
/**
  * @param uncompressed The input string that requires
  *     a very long description and an even longer
  *     description on this line as well.
  */
```

### Constants and Variables

Example:

```cpp
/**
 * Prefix used to name Docker containers in order to distinguish
 * those created by Mesos from those created manually.
 */
extern const std::string DOCKER_NAME_PREFIX;
```

#### Fields

Example:

```cpp
/**
 * The parent side of the pipe for stdin.
 * If the mode is not PIPE, None will be stored.
 * **NOTE**: stdin is a macro on some systems, hence this name instead.
 */
Option<int> in;
```

### Functions and Methods

Example:

```cpp
/**
 * Forks a subprocess and execs the specified 'path' with the
 * specified 'argv', redirecting stdin, stdout, and stderr as
 * specified by 'in', 'out', and 'err' respectively.
 *
 * If 'setup' is not None, runs the specified function after forking
 * but before exec'ing. If the return value of 'setup' is non-zero
 * then that gets returned in 'status()' and we will not exec.
 *
 * @param path Relative or absolute path in the filesytem to the
 *     executable.
 * @param argv Argument vector to pass to exec.
 * @param in Redirection specification for stdin.
 * @param out Redirection specification for stdout.
 * @param err Redirection specification for stderr.
 * @param flags Flags to be stringified and appended to 'argv'.
 * @param environment Environment variables to use for the new
 *     subprocess or if None (the default) then the new subprocess
 *     will inherit the environment of the current process.
 * @param setup Function to be invoked after forking but before
 *     exec'ing. NOTE: Take extra care not to invoke any
 *     async unsafe code in the body of this function.
 * @param clone Function to be invoked in order to fork/clone the
 *     subprocess.
 * @return The subprocess or an error if one occurred.
 */
Try<Subprocess> subprocess(
    const std::string& path,
    std::vector<std::string> argv,
    const Subprocess::IO& in = Subprocess::FD(STDIN_FILENO),
    const Subprocess::IO& out = Subprocess::FD(STDOUT_FILENO),
    const Subprocess::IO& err = Subprocess::FD(STDERR_FILENO),
    const Option<flags::FlagsBase>& flags = None(),
    const Option<std::map<std::string, std::string>>& environment = None(),
    const Option<lambda::function<int()>>& setup = None(),
    const Option<lambda::function<
        pid_t(const lambda::function<int()>&)>>& clone = None());
```

### Classes and Structs

Example:

```cpp
/**
 * Represents a fork() exec()ed subprocess. Access is provided to the
 * input / output of the process, as well as the exit status. The
 * input / output file descriptors are only closed after:
 *   1. The subprocess has terminated.
 *   2. There are no longer any references to the associated
 *      Subprocess object.
 */
class Subprocess
{
public:
```


## Building Doxygen Documentation

As of right now, the Doxygen documentation should be built from the *build* subdirectory using `doxygen ../Doxyfile`. The documentation will then be generated into the `./doxygen` subdirectory.
