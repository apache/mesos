clang-tidy docker image for Mesos
=================================

This directory contains tooling to build a docker image to run
clang-tidy checks over a Mesos repository. It uses a [version of
clang-tidy](https://github.com/mesos/clang-tools-extra/tree/mesos_38)
augmented with Mesos-specific checks.

*IMPORTANT*: This directory is for maintainers of `mesos-tidy` checks.
Users should reach for `support/mesos-tidy.sh`.


Building the docker image
-------------------------

Building the image involves compiling a Mesos-flavored version of
`clang-tidy`, installing Mesos dependencies, and installation of tools
to drive check invocations.

The image can be built with

    $ docker build -t mesos-tidy .

A pre-built image is available via Docker Hub as `mesosphere/mesos-tidy`.


Running checks
--------------

To run checks over a Mesos checkout invoke

    $ docker run \
        -v <MESOS_CHECKOUT>:/SRC \
        [-e CHECKS=<CHECKS>] \
        [-e CONFIGURE_FLAGS=<CONFIGURE_FLAGS>] \
        --rm \
        mesos-tidy

Here `MESOS_CHECKOUT` points to a git checkout of the Mesos source
tree.

Additional configure parameters can be passed to the `./configure`
invocation of Mesos via `CONFIGURE_FLAGS`. By default, `./configure`
will be invoked without arguments.

Optionally, the set of checks to perform can be specified in a
clang-tidy check regex via `CHECKS`.

Results from 3rdparty external dependencies are filtered from the result set.
