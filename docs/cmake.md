---
title: Apache Mesos - CMake
layout: documentation
---

# Install CMake 3.7+

[cmake-download]: https://cmake.org/download/

## Linux

Install the latest version of CMake from [CMake.org][cmake-download].
A self-extracting tarball is available to make this process painless.

Currently, few of the common Linux flavors package a sufficient CMake
version. Ubuntu versions 12.04 and 14.04 package CMake 2;
Ubuntu 16.04 packages CMake 3.5. If you already installed cmake from packages,
you may remove it via: `apt-get purge cmake`.

The standard CentOS package is CMake 2, and unfortunately even the `cmake3`
package in EPEL is only CMake 3.6, you may remove them via:
`yum remove cmake cmake3`.

## Mac OS X

HomeBrew's CMake version is sufficient: `brew install cmake`.

## Windows

Download and install the MSI from [CMake.org][cmake-download].

**NOTE:** Windows needs CMake 3.8+, rather than 3.7+.

# Quick Start

The most basic way to build with CMake, with no configuration, is fairly
straightforward:

```
mkdir build
cd build
cmake ..
cmake --build .
```

The last step, `cmake --build .` can also take a `--target` command to build any
particular target (e.g. `mesos-tests`, or `tests` to build `mesos-tests`,
`libprocess-tests`, and `stout-tests`): `cmake --build . --target tests`. To
send arbitrary flags to the native build system underneath (e.g. `make`), append
the command with `-- <flags to be passed>`: `cmake --build . -- -j4`.

Also, `cmake --build` can be substituted by your build system of choice. For
instance, the default CMake generator on Linux produces GNU Makefiles, so after
configuring with `cmake ..`, you can just run `make tests` in the `build` folder
like usual. Similarly, if you configure with `-G Ninja` to use the Ninja
generator, you can then run `ninja tests` to build the `tests` target with
Ninja.

# Installable build

This example will build Mesos and install it into a custom prefix:
```
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/home/current_user/mesos
cmake --build . --target install
```

To additionally install `mesos-tests` executable and related test helpers
(this can be used to run Mesos tests against the installed binaries),
one can enable the `MESOS_INSTALL_TESTS` option.

To produce a set of binaries and libraries that will work after being
copied/moved to a different location, use `MESOS_FINAL_PREFIX`.

The example below employs both `MESOS_FINAL_PREFIX` and `MESOS_INSTALL_TESTS`.
On a build system:
```
mkdir build && cd build
cmake -DMESOS_FINAL_PREFIX=/opt/mesos -DCMAKE_INSTALL_PREFIX=/home/current_user/mesos -DMESOS_INSTALL_TESTS=ON
cmake --build . --target install
tar -czf mesos.tar.gz mesos -C /home/current_user
```
On a target system:
```
sudo tar -xf mesos.tar.gz -C /opt
# Run tests against Mesos installation
sudo /opt/mesos/bin/mesos-tests
# Start Mesos agent
sudo /opt/mesos/bin/mesos-agent --work-dir=/var/lib/mesos ...
```

# Supported options

See [configuration options](configuration/cmake.md).

# Examples

See [CMake By Example](cmake-examples.md).

# Documentation

The [CMake documentation][] is written as a reference module. The most commonly
used sections are:

* [buildsystem overview](https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html)
* [commands](https://cmake.org/cmake/help/latest/manual/cmake-commands.7.html)
* [properties](https://cmake.org/cmake/help/latest/manual/cmake-properties.7.html)
* [variables](https://cmake.org/cmake/help/latest/manual/cmake-variables.7.html)

The wiki also has a set of [useful variables][].

[CMake documentation]: https://cmake.org/cmake/help/latest/
[useful variables]: https://cmake.org/Wiki/CMake_Useful_Variables

# Dependency graph

Like any build system, CMake has a dependency graph. The difference is
that targets in CMake's dependency graph are _much richer_ compared to other
build systems. CMake targets have the notion of 'interfaces', where build
properties are saved as part of the target, and these properties can be
inherited transitively within the graph.

For example, say there is a library `mylib`, and anything which links it must
include its headers, located in `mylib/include`. When building the library, some
private headers must also be included, but not when linking to it. When
compiling the executable `myprogram`, `mylib`'s public headers must be included,
but not its private headers. There is no manual step to add `mylib/include` to
`myprogram` (and any other program which links to `mylib`), it is instead
deduced from the public interface property of `mylib`. This is represented by
the following code:

```
# A new library with a single source file (headers are found automatically).
add_library(mylib mylib.cpp)

# The folder of private headers, not exposed to consumers of `mylib`.
target_include_directories(mylib PRIVATE mylib/private)

# The folder of public headers, added to the compilation of any consumer.
target_include_directories(mylib PUBLIC mylib/include)

# A new exectuable with a single source file.
add_executable(myprogram main.cpp)

# The creation of the link dependency `myprogram` -> `mylib`.
target_link_libraries(myprogram mylib)

# There is no additional step to add `mylib/include` to `myprogram`.
```

This same notion applies to practically every build property:
compile definitions via [`target_compile_definitions`][],
include directories via [`target_include_directories`][],
link libraries via [`target_link_libraries`][],
compile options via [`target_compile_options`][],
and compile features via [`target_compile_features`][].

All of these commands also take an optional argument of
`<INTERFACE|PUBLIC|PRIVATE>`, which constrains their transitivity in the graph.
That is, a `PRIVATE` include directory is recorded for the target, but not
shared transitively to anything depending on the target, `PUBLIC` is used
for both the target and dependencies on it, and `INTERFACE` is used only
for dependencies.

Notably missing from this list are link directories. CMake explicitly prefers
finding and using the absolute paths to libraries, obsoleting link directories.

[`target_compile_definitions`]: https://cmake.org/cmake/help/latest/command/target_compile_definitions.html
[`target_include_directories`]: https://cmake.org/cmake/help/latest/command/target_include_directories.html
[`target_link_libraries`]: https://cmake.org/cmake/help/latest/command/target_link_libraries.html
[`target_compile_options`]: https://cmake.org/cmake/help/latest/command/target_compile_options.html
[`target_compile_features`]: https://cmake.org/cmake/help/latest/command/target_compile_features.html

# Common mistakes

## Booleans

CMake treats `ON`, `OFF`, `TRUE`, `FALSE`, `1`, and `0` all as true/false
booleans. Furthermore, variables of the form `<target>-NOTFOUND` are also
treated as false (this is used for finding packages).

In Mesos, we prefer the boolean types `TRUE` and `FALSE`.

See [`if`](https://cmake.org/cmake/help/latest/command/if.html) for more info.

## Conditionals

For historical reasons, CMake conditionals such as `if` and `elseif`
automatically interpolate variable names. It is therefore dangerous to
interpolate them manually, because if `${FOO}` evaluates to `BAR`, and `BAR` is
another variable name, then `if (${FOO})` becomes `if (BAR)`, and `BAR` is then
evaluated again by the `if`. Stick to `if (FOO)` to check the value of `${FOO}`.
Do not use `if (${FOO})`.

Also see the CMake policies
[CMP0012](https://cmake.org/cmake/help/latest/policy/CMP0012.html) and
[CMP0054](https://cmake.org/cmake/help/latest/policy/CMP0054.html).

## Definitions

When using `add_definitions()` (which should be used rarely, as it is for
"global" compile definitions), the flags must be prefixed with `-D` to be
treated as preprocessor definitions. However, when using
`target_compile_definitions()` (which should be preferred, as it is
for specific targets), the flags do not need the prefix.

# Style

In general, wrap at 80 lines, and use a two-space indent. When wrapping
arguments, put the command on a separate line and arguments on subsequent lines:

```
target_link_libraries(
  program PRIVATE
  alpha
  beta
  gamma)
```

Otherwise keep it together:

```
target_link_libraries(program PUBLIC library)
```

Always keep the trailing parenthesis with the last argument.

Use a single space between conditionals and their open parenthesis, e.g.
`if (FOO)`, but not for commands, e.g. `add_executable(program)`.

CAPITALIZE the declaration and use of custom functions and macros (e.g.
`EXTERNAL` and `PATCH_CMD`), and do not capitalize the use of CMake built-in
(including modules) functions and macros. CAPITALIZE variables.

# CMake anti-patterns

[anti-patterns]: http://voices.canonical.com/jussi.pakkanen/2013/03/26/a-list-of-common-cmake-antipatterns/

Because CMake handles much more of the grunt work for you than other build
systems, there are unfortunately a lot of CMake [anti-patterns][] you should
look out for when writing new CMake code. These are some common problems
that should be avoided when writing new CMake code:

## Superfluous use of `add_dependencies`

When you've linked library `a` to library `b` with `target_link_libraries(a b)`,
the CMake graph is already updated with the dependency information. It is
redundant to use `add_dependencies(a b)` to (re)specify the dependency. In fact,
this command should _rarely_ be used.

The exceptions to this are:

  1. Setting a dependency from an imported library to a target added via
     `ExternalProject_Add`.
  2. Setting a dependency on Mesos modules since no explicit linking is done.
  3. Setting a dependency between executables (e.g. the `mesos-agent` requiring the
     `mesos-containerizer` executable). In general, runtime dependencies need
     to be setup with `add_dependency`, but never link dependencies.

## Use of `link_libraries` or `link_directories`

Neither of these commands should ever be used. The only appropriate command used
to link libraries is [`target_link_libraries`][], which records the information
in the CMake dependency graph. Furthermore, imported third-party libraries
should have correct locations recorded in their respective targets, so the use
of `link_directories` should never be necessary. The
[official documentation][link-directories] states:

> Note that this command is rarely necessary. Library locations returned by
> `find_package()` and `find_library()` are absolute paths. Pass these absolute
> library file paths directly to the `target_link_libraries()` command. CMake
> will ensure the linker finds them.

[link-directories]: https://cmake.org/cmake/help/latest/command/link_directories.html

The difference is that the former sets global (or directory level) side effects,
and the latter sets specific target information stored in the graph.

## Use of `include_directories`

This is similar to the above: the [`target_include_directories`][] should always
be preferred so that the include directory information remains localized to the
appropriate targets.

## Adding anything to `endif ()`

Old versions of CMake expected the style `if (FOO) ... endif (FOO)`, where the
`endif` contained the same expression as the `if` command. However, this is
tortuously redundant, so leave the parentheses in `endif ()` empty. This goes
for other endings too, such as `endforeach ()`, `endwhile ()`, `endmacro ()` and
`endfunction ()`.

## Specifying header files superfluously

One of the distinct advantages of using CMake for C and C++ projects is that
adding header files to the source list for a target is unnecessary. CMake is
designed to parse the source files (`.c`, `.cpp`, etc.) and determine their
required headers automatically. The exception to this is headers generated as
part of the build (such as protobuf or the JNI headers).

## Checking `CMAKE_BUILD_TYPE`

See the ["Building debug or release configurations"](cmake-examples.md#building-debug-or-release-configurations)
example for more information. In short, not all generators respect the variable
`CMAKE_BUILD_TYPE` at configuration time, and thus it must not be used in CMake
logic. A usable alternative (where supported) is a [generator expression][] such
as `$<$<CONFIG:Debug>:DEBUG_MODE>`.

[generator expression]: https://cmake.org/cmake/help/latest/manual/cmake-generator-expressions.7.html#logical-expressions

# Remaining hacks

## `3RDPARTY_DEPENDENCIES`

Until Mesos on Windows is stable, we keep some dependencies in an external
repository, [3rdparty](https://github.com/mesos/3rdparty). When
all dependencies are bundled with Mesos, this extra repository will no longer be
necessary. Until then, the CMake variable `3RDPARTY_DEPENDENCIES` points by
default to this URL, but it can also point to the on-disk location of a local
clone of the repo. With this option you can avoid pulling from GitHub for every
clean build. Note that this must be an absolute path with forward slashes, e.g.
`-D3RDPARTY_DEPENDENCIES=C:/3rdparty`, otherwise it will fail on Windows.

## `EXTERNAL`

The CMake function `EXTERNAL` defines a few variables that make it easy for us
to track the directory structure of a dependency. In particular, if our
library's name is `boost`, we invoke:

```
EXTERNAL(boost ${BOOST_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
```

Which will define the following variables as side-effects in the current scope:

* `BOOST_TARGET`     (a target folder name to put dep in e.g., `boost-1.53.0`)
* `BOOST_CMAKE_ROOT` (where to have CMake put the uncompressed source, e.g.,
                     `build/3rdparty/boost-1.53.0`)
* `BOOST_ROOT`       (where the code goes in various stages of build, e.g.,
                     `build/.../boost-1.53.0/src`, which might contain folders
                     `build-1.53.0-build`, `-lib`, and so on, for each build
                     step that dependency has)

The implementation is in `3rdparty/cmake/External.cmake`.

This is not to be confused with the CMake module [ExternalProject][], from which
we use `ExternalProject_Add` to download, extract, configure, and build our
dependencies.

[ExternalProject]: https://cmake.org/cmake/help/latest/module/ExternalProject.html

## `CMAKE_NOOP`

This is a CMake variable we define in `3rdparty/CMakeLists.txt` so that we can
cancel steps of `ExternalProject`. `ExternalProject`'s default behavior is to
attempt to configure, build, and install a project using CMake. So when one of
these steps must be skipped, we use set it to `CMAKE_NOOP` so that nothing
is run instead.

## `CMAKE_FORWARD_ARGS`

The `CMAKE_FORWARD_ARGS` variable defined in `3rdparty/CMakeLists.txt` is sent
as the `CMAKE_ARGS` argument to the `ExternalProject_Add` macro (along with any
per-project arguments), and is used when the external project is configured as a
CMake project. If either the `CONFIGURE_COMMAND` or `BUILD_COMMAND` arguments of
`ExternalProject_Add` are used, then the `CMAKE_ARGS` argument will be ignored.
This variable ensures that compilation configurations are properly propagated to
third-party dependencies, such as compiler flags.

### `CMAKE_SSL_FORWARD_ARGS`

The `CMAKE_SSL_FORWARD_ARGS` variable defined in `3rdparty/CMakeLists.txt`
is like `CMAKE_FORWARD_ARGS`, but only used for specific external projects
that find and link against OpenSSL.

## `LIBRARY_LINKAGE`

This variable is a shortcut used in `3rdparty/CMakeLists.txt`. It is set to
`SHARED` when `BUILD_SHARED_LIBS` is true, and otherwise it is set to `STATIC`.
The `SHARED` and `STATIC` keywords are used to declare how a library should be
built; however, if left out then the type is deduced automatically from
`BUILD_SHARED_LIBS`.

## `MAKE_INCLUDE_DIR`

This function works around a [CMake issue][cmake-15052] with setting include
directories of imported libraries built with `ExternalProject_Add`. We have to
call this for each `IMPORTED` third-party dependency which has set
`INTERFACE_INCLUDE_DIRECTORIES`, just to make CMake happy. An example is Glog:

```
MAKE_INCLUDE_DIR(glog)
```

[cmake-15052]: https://gitlab.kitware.com/cmake/cmake/issues/15052

## `GET_BYPRODUCTS`

This function works around a [CMake issue][cmake-060234] with the Ninja
generator where it does not understand imported libraries, and instead needs
`BUILD_BYPRODUCTS` explicitly set. This simply allows us to use
`ExternalProject_Add` and Ninja. For Glog, it looks like this:

```
GET_BYPRODUCTS(glog)
```

Also see the CMake policy [CMP0058][].

[cmake-060234]: https://cmake.org/pipermail/cmake/2015-April/060234.html
[CMP0058]: https://cmake.org/cmake/help/latest/policy/CMP0058.html

## `PATCH_CMD`

The CMake function `PATCH_CMD` generates a patch command given a patch file.
If the path is not absolute, it's resolved to the current source directory.
It stores the command in the variable name supplied. This is used to easily
patch third-party dependencies. For Glog, it looks like this:

```
PATCH_CMD(GLOG_PATCH_CMD glog-${GLOG_VERSION}.patch)
ExternalProject_Add(
  ${GLOG_TARGET}
  ...
  PATCH_COMMAND     ${GLOG_PATCH_CMD})
```

The implementation is in `3rdparty/cmake/PatchCommand.cmake`.

### Windows `patch.exe`

While using `patch` on Linux is straightforward, doing the same on Windows takes
a bit of work. `PATH_CMD` encapsulates this:

* Checks the cache variable `PATCHEXE_PATH` for `patch.exe`.
* Searches for `patch.exe` in its default locations.
* Copies `patch.exe` and a custom manifest to the temporary directory.
* Applies the manifest to avoid the UAC prompt.
* Uses the patched `patch.exe`.

As such, `PATCH_CMD` lets us apply patches as we do on Linux, without requiring
an administrative prompt.

Note that on Windows, the patch file must have CRLF line endings. A file with LF
line endings will cause the error: "Assertion failed, hunk, file patch.c, line
343". For this reason, it is required to checkout the Mesos repo with `git
config core.autocrlf true`.
