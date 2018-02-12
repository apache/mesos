# `clang-tidy` docker image for Mesos

This directory contains tooling to build a docker image to run `clang-tidy`
checks over a Mesos repository. It uses a
[customized version of `clang-tidy`][custom-clang-tidy] which is augmented with
Mesos-specific checks.

__IMPORTANT__: This directory is intended for maintainers of `mesos-tidy`
checks. Users should reach for `support/mesos-tidy.sh`.

## Building the docker image

Building the image involves compiling a Mesos-flavored version of
`clang-tidy`, installing Mesos dependencies, and installation of tools
to drive check invocations.

The image can be built with

```bash
$ docker build -t mesos-tidy .
```

A pre-built image is available via Docker Hub as `mesos/mesos-tidy`.

## Running checks

To run checks over a Mesos checkout invoke

```bash
$ docker run \
      --rm \
      -v <MESOS_CHECKOUT>:/SRC \
      [-e CHECKS=<CHECKS>] \
      [-e CMAKE_ARGS=<CMAKE_ARGS>] \
      mesos-tidy
```

Here `MESOS_CHECKOUT` points to a git checkout of the Mesos source tree.

Additional configure parameters can be passed to the `cmake` invocation of Mesos
via `CMAKE_ARGS`. By default, `cmake` will be invoked with
`-DCMAKE_BUILD_TYPE=Release` and `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`.

Optionally, the set of checks to perform can be specified in a
`clang-tidy` check regex via `CHECKS`.

Results from 3rdparty external dependencies are filtered from the result set.


[custom-clang-tidy]: https://github.com/mesos/clang-tools-extra/tree/mesos_50
