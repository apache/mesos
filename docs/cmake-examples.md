---
title: Apache Mesos - CMake By Example
layout: documentation
---

# Adding a new library or executable

When adding a new library or executable, prefer using the name directly as the
target. E.g. `libprocess` is `add_library(process)`, and `mesos-agent` is
`add_executable(mesos-agent)`. Note that, on platforms where it is conventional,
`add_library` will prepend `lib` when writing the library to disk.

Do not introduce a variable simply to hold the name of the target; if the name
on disk needs to be a specific value, set the target property `OUTPUT_NAME`.

# Adding a third-party dependency

When adding a third-party dependency, keep the principle of locality in mind.
All necessary data for building with and linking to the library should
be defined where the library is imported. A consumer of the dependency should
only have to add `target_link_libraries(consumer dependency)`, with every other
build property coming from the graph (library location, include directories,
compiler definitions, etc.).

The steps to add a new third-party dependency are:

  1. Add the version and SHA256 hash to `Versions.cmake`.
  2. Add the URL/tarball file to the top of `3rdparty/CMakeLists.txt`.
  3. Find an appropriate location in `3rdparty/CMakeLists.txt` to declare the
     library. Add a nice header with the name, description, and home page.
  4. Use `add_library(IMPORTED)` to declare an imported target.
     A header-only library is imported with `add_library(INTERFACE)`.
  5. Use [`ExternalProject_Add`][] to obtain, configure, and build the library.
  6. Link the consumer to the dependency.

[`ExternalProject_Add`]: https://cmake.org/cmake/help/latest/module/ExternalProject.html

## `INTERFACE` libraries

Using header-only libraries in CMake is a breeze. The special `INTERFACE`
library lets you declare a header-only library as a proper CMake target, and
then use it like any other library. Let's look at Boost for an example.

First, we add two lines to `Versions.cmake`:

```
set(BOOST_VERSION "1.53.0")
set(BOOST_HASH    "SHA256=CED7CE2ED8D7D34815AC9DB1D18D28FCD386FFBB3DE6DA45303E1CF193717038")
```

This lets us keep the versions (and the SHA256 hash of the tarball) of all our
third-party dependencies in one location.

Second, we add one line to the top of `3rdparty/CMakeLists.txt` to declare the
location of the tarball:

```
set(BOOST_URL ${FETCH_URL}/boost-${BOOST_VERSION}.tar.gz)
```

The `FETCH_URL` variable lets the `REBUNDLED` option switch between offline and
online versions. The use of `BOOST_VERSION` shows why this variable is declared
early; it's used a few times.

Third, we find a location in `3rdparty/CMakeLists.txt` to declare the Boost
library.

```
# Boost: C++ Libraries.
# http://www.boost.org
#######################
...
```

We start with a proper header naming and describing the library, complete with
its home page URL. This is for other developers to easily identify why this
third-party dependency exists.

```
...
EXTERNAL(boost ${BOOST_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(boost INTERFACE)
add_dependencies(boost ${BOOST_TARGET})
target_include_directories(boost INTERFACE ${BOOST_ROOT})
...
```

Fourth, we declare the Boost target.

To make things easier, we invoke our custom CMake function `EXTERNAL` to setup
some variables for us: `BOOST_TARGET`, `BOOST_ROOT`, and `BOOST_CMAKE_ROOT`. See
[the docs](cmake.md#EXTERNAL) for more explanation of `EXTERNAL`.

Then we call `add_library(boost INTERFACE)`. This creates a header-only CMake
target, usable like any other library. We use `add_dependencies(boost
${BOOST_TARGET})` to add a manual dependency on the `ExternalProject_Add` step;
this is necessary as CMake is lazy and won't execute code unless it must (say,
because of a dependency). The final part of creating this header-only library in
our build system is `target_include_directories(boost INTERFACE
${BOOST_ROOT})`, which sets the `BOOST_ROOT` folder (the destination of the
extracted headers) as the include interface for the `boost` target. All
dependencies on Boost will now automatically include this folder during
compilation.

Fifth, we setup the `ExternalProject_Add` step. This CMake module is incredibly
flexible, but we're using it in the simplest case.

```
...
ExternalProject_Add(
  ${BOOST_TARGET}
  PREFIX            ${BOOST_CMAKE_ROOT}
  CONFIGURE_COMMAND ${CMAKE_NOOP}
  BUILD_COMMAND     ${CMAKE_NOOP}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${BOOST_URL}
  URL_HASH          ${BOOST_HASH})
```

The name of the custom target this creates is `BOOST_TARGET`, and the prefix
directory for all the subsequent steps is `BOOST_CMAKE_ROOT`. Because this is a
header-only library, and `ExternalProject_Add` defaults to invoking `cmake`, we
use `CMAKE_NOOP` to disable the configure, build, and install commands. See [the
docs](cmake.md#cmake_noop) for more explanation of `CMAKE_NOOP`. Thus this code
will simply verify the tarball with `BOOST_HASH`, and then extract it from
`BOOST_URL` to `BOOST_ROOT` (a sub-folder of `BOOST_CMAKE_ROOT`).

Sixth, and finally, we link `stout` to `boost`. This is the _only_ change
necessary to `3rdparty/stout/CMakeLists.txt`, as the include directory
information is embedded in the CMake graph.

```
target_link_libraries(
  stout INTERFACE
  ...
  boost
  ...)
```

This dependency need not be specified again, as `libprocess` and `libmesos` link
to `stout`, and so `boost` is picked up transitively.

### Stout

Stout is a header-only library. Like Boost, it is a real CMake target, declared
in `3rdparty/stout/CMakeLists.txt`, just without the external bits.

```
add_library(stout INTERFACE)
target_include_directories(stout INTERFACE include)
target_link_libraries(
  stout INTERFACE
  apr
  boost
  curl
  elfio
  glog
  ...)
```

It is added as an `INTERFACE` library. Its include directory is specified as an
`INTERFACE` (the `PUBLIC` property cannot be used as the library itself is just
an interface). Its "link" dependencies (despite not being a real, linkable
library) are specified as an `INTERFACE`.

This notion of an interface in the CMake dependency graph is what makes the
build system reasonable. The Mesos library and executables, and `libprocess`, do
not have to repeat these lower level dependencies that come from `stout`.

## `IMPORTED` libraries

Third-party dependencies that we build are only more complicated because we have
to encode their build steps too. We'll examine `glog`, and go over the
differences from the interface library `boost`.

Notably, when we declare the library, we use:

```
add_library(glog ${LIBRARY_LINKAGE} IMPORTED GLOBAL)
```

Instead of `INTERFACE` we specify `IMPORTED` as it is an actual library. We add
`GLOBAL` to enable our pre-compiled header module `cotire` to find the targets
(as they would otherwise be scoped only to `3rdparty` and below). And most
oddly, we use `${LIBRARY_LINKAGE}` to set it as `SHARED` or `STATIC` based on
`BUILD_SHARED_LIBS`, as we can build this dependency in both manners. See [the
docs](cmake.md#library_linkage) for more information.

We must patch our bundled version of `glog` so we call:

```
PATCH_CMD(GLOG_PATCH_CMD glog-${GLOG_VERSION}.patch)
```

This generates a patch command. See [the docs](cmake.md#patch_cmd) for more
information.

This library is an example of where we differ on Windows and other platforms. On
Windows, we build `glog` with CMake, and have several properties we must set:

```
set_target_properties(
  glog PROPERTIES
  IMPORTED_LOCATION_DEBUG ${GLOG_ROOT}-build/Debug/glog${LIBRARY_SUFFIX}
  IMPORTED_LOCATION_RELEASE ${GLOG_ROOT}-build/Release/glog${LIBRARY_SUFFIX}
  IMPORTED_IMPLIB_DEBUG ${GLOG_ROOT}-build/Debug/glog${CMAKE_IMPORT_LIBRARY_SUFFIX}
  IMPORTED_IMPLIB_RELEASE ${GLOG_ROOT}-build/Release/glog${CMAKE_IMPORT_LIBRARY_SUFFIX}
  INTERFACE_INCLUDE_DIRECTORIES ${GLOG_ROOT}/src/windows
  # TODO(andschwa): Remove this when glog is updated.
  IMPORTED_LINK_INTERFACE_LIBRARIES DbgHelp
  INTERFACE_COMPILE_DEFINITIONS "${GLOG_COMPILE_DEFINITIONS}")
```

The location of an imported library _must_ be set for the build system to link
to it. There is no notion of search through link directories for imported
libraries.

Windows requires both the `DEBUG` and `RELEASE` locations of the library
specified, and since we have (experimental) support to build `glog` as a shared
library on Windows, we also have to declare the `IMPLIB` location. Fortunately,
these locations are programmatic based of `GLOG_ROOT`, set from our call to
`EXTERNAL`.

Note that we cannot use `target_include_directories` with an imported target. We
have to set `INTERFACE_INCLUDE_DIRECTORIES` manually instead.

This version of `glog` on Windows depends on `DbgHelp` but does not use a
`#pragma` to include it, so we set it as an interface library that must also be
linked, using the `IMPORTED_LINK_INTERFACE_LIBRARIES` property.

For Windows there are multiple compile definitions that must be set when
building with the `glog` headers, these are specified with the
`INTERFACE_COMPILE_DEFINITIONS` property.

For non-Windows platforms, we just set the Autotools commands to configure,
make, and install `glog`. These commands depend on the project requirements. We
also set the `IMPORTED_LOCATION` and `INTERFACE_INCLUDE_DIRECTORIES`.

```
set(GLOG_CONFIG_CMD  ${GLOG_ROOT}/src/../configure --with-pic GTEST_CONFIG=no --prefix=${GLOG_ROOT}-build)
set(GLOG_BUILD_CMD   make)
set(GLOG_INSTALL_CMD make install)

set_target_properties(
  glog PROPERTIES
  IMPORTED_LOCATION ${GLOG_ROOT}-build/lib/libglog${LIBRARY_SUFFIX}
  INTERFACE_INCLUDE_DIRECTORIES ${GLOG_ROOT}-build/include)
```

To work around some issues, we have to call `MAKE_INCLUDE_DIR(glog)` to create
the include directory immediately so as to satisfy CMake's requirement that it
exists (it will be populated by `ExternalProject_Add` during the build, but must
exist first). See [the docs](cmake.md#make_include_dir) for more information.

Then call `GET_BYPRODUCTS(glog)` to create the `GLOG_BYPRODUCTS` variable, which
is sent to `ExternalProject_Add` to make the Ninja build generator happy. See
[the docs](cmake.md#get_byproducts) for more information.

```
MAKE_INCLUDE_DIR(glog)
GET_BYPRODUCTS(glog)
```

Like with Boost, we call `ExternalProject_Add`:

```
ExternalProject_Add(
  ${GLOG_TARGET}
  PREFIX            ${GLOG_CMAKE_ROOT}
  BUILD_BYPRODUCTS  ${GLOG_BYPRODUCTS}
  PATCH_COMMAND     ${GLOG_PATCH_CMD}
  CMAKE_ARGS        ${CMAKE_FORWARD_ARGS};-DBUILD_TESTING=OFF
  CONFIGURE_COMMAND ${GLOG_CONFIG_CMD}
  BUILD_COMMAND     ${GLOG_BUILD_CMD}
  INSTALL_COMMAND   ${GLOG_INSTALL_CMD}
  URL               ${GLOG_URL}
  URL_HASH          ${GLOG_HASH})
```

In contrast to an interface library, we need to send all the build information,
which we set in variables prior. This includes the `BUILD_BYPRODUCTS`, and the
`PATCH_COMMAND` as we have to patch `glog`.

Since we build `glog` with CMake on Windows, we have to set `CMAKE_ARGS` with
the `CMAKE_FORWARD_ARGS`, and particular to `glog`, we disable its tests with
`-DBUILD_TESTING=OFF`, though this is not a canonical CMake option.

On Linux, we set the config, build, and install commands, and send them too.
These are empty on Windows, so `ExternalProject_Add` will fallback to using
CMake, as we needed.

Finally, we add `glog` to as a link library to `stout`:

```
target_link_libraries(
  stout INTERFACE
  ...
  glog
  ...)
```

No other code is necessary, we have completed adding, building, and linking to
`glog`. The same patterns can be adapted for any other third-party dependency.

# Building debug or release configurations

The default configuration is always `Debug`, which means with debug symbols and
without (many) optimizations. Of course, when deploying Mesos an optimized
`Release` build is desired. This is one of the few inconsistencies in CMake, and
it's due to the difference between so-called "single-configuration generators"
(such as GNU Make) and "multi-configuration generators" (such as Visual Studio).

## Configuration-time configurations

In single-configuration generators, the configuration (debug or release) is
chosen at configuration time (that is, when initially calling `cmake` to
configure the build), and it is not changeable without re-configuring. So
building a `Release` configuration on Linux (with GNU Make) is done via:

```
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

## Build-time configurations

However, the Visual Studio generator on Windows allows the developer to change
the release at build-time, making it a multi-configuration generator. CMake
generates a configuration-agnostic solution (and so `CMAKE_BUILD_TYPE` is
ignored), and the user switches the configuration when building. This can be
done with the familiar configuration menu in the Visual Studio IDE, or with
CMake via:

```
cmake ..
cmake --build . --config Release
```

In the same build folder, a `Debug` build can also be built, with the binaries
stored in `Debug` and `Release` folders respectively. Unfortunately, the current
CMake build explicitly sets the final binary destination directories, and so the
final libraries and executables will overwrite each other when building
different configurations.

Note that Visual Studio is not the only IDE that uses a multi-configuration
generator, Xcode on Mac OS X does as well.
See [MESOS-7943][] for more information.

[MESOS-7943]: https://issues.apache.org/jira/browse/MESOS-7943

## Building with shared or static libraries

On Linux, the configuration option `-DBUILD_SHARED_LIBS=FALSE` can be used to
switch to static libraries where possible. Otherwise Linux builds shared
libraries by default.

On Windows, static libraries are the default. Building with shared libraries on
Windows is not yet supported, as it requires code change to import symbols
properly.
