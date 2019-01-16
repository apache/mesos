#!/usr/bin/env python3
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


"""
Parallel test runner for GoogleTest programs.

This script allows one to execute GoogleTest tests in parallel.
GoogleTest programs come with built-in support for running in parallel.
Here tests can automatically be partitioned across a number of test
program invocations ("shards"). This script provides a convenient
wrapper around that functionality and stream-lined output.
"""

import argparse
import multiprocessing
import os
import resource
import shlex
import subprocess
import sys


class Bcolors:
    """
    A collection of tty output modifiers.

    To switch the output of a string, prefix it with the desired
    modifier, and terminate it with 'ENDC'.
    """

    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'
    UNDERLINE = '\033[4m'

    @staticmethod
    def colorize(string, *color_codes):
        """Decorate a string with a number of color codes."""
        if sys.stdout.isatty():
            colors = ''.join(color_codes)
            string = '{begin}{string}{end}'.format(
                begin=colors, string=string, end=Bcolors.ENDC)
        return string


def run_test(opts):
    """
    Perform an actual run of the test executable.

    Expects a list of parameters giving the number of the current
    shard, the total number of shards, and the command line to run.
    """
    shard, nshards, args = opts

    env = os.environ.copy()
    env['GTEST_TOTAL_SHARDS'] = str(nshards)
    env['GTEST_SHARD_INDEX'] = str(shard)

    try:
        output = subprocess.check_output(
            args,
            stderr=subprocess.STDOUT,
            env=env,
            universal_newlines=True)
        print(Bcolors.colorize('.', Bcolors.OKGREEN), end='')
        sys.stdout.flush()
        return True, output
    except KeyboardInterrupt:
        return False
    except subprocess.CalledProcessError as error:
        print(Bcolors.colorize('.', Bcolors.FAIL), end='')
        sys.stdout.flush()
        return False, error.output


def parse_arguments():
    """Return the executable to work on, and a list of options."""

    # If the environment variable `MESOS_GTEST_RUNNER_FLAGS` is set we
    # use it to set a default set of flags to pass. Flags passed on
    # the command line always have precedence over these defaults.
    #
    # We manually construct `args` here and make use of the fact that
    # in `optparser`'s implementation flags passed later on the
    # command line overrule identical flags passed earlier.

    env_var = ''
    if 'MESOS_GTEST_RUNNER_FLAGS' in os.environ:
        env_var = os.environ['MESOS_GTEST_RUNNER_FLAGS']

    env_parser = argparse.ArgumentParser()
    env_parser.add_argument('-j', '--jobs', type=int,
                            default=int(multiprocessing.cpu_count()))
    env_parser.add_argument('-s', '--sequential', type=str, default='')
    env_parser.add_argument('-v', '--verbosity', type=int, default=1)

    env_args = env_parser.parse_args(shlex.split(env_var))

    # We have set the default values using the environment variable if
    # possible, we can now parse the arguments.
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '-j', '--jobs', type=int,
        default=env_args.jobs,
        help='number of parallel jobs to spawn. DEFAULT: {default_}'
        .format(default_=env_args.jobs))

    parser.add_argument(
        '-s', '--sequential', type=str,
        default=env_args.sequential,
        help='gtest filter for tests to run sequentially')

    parser.add_argument(
        '-v', '--verbosity', type=int,
        default=env_args.verbosity,
        help='output verbosity:'
        ' 0 only shows summarized information,'
        ' 1 also shows full logs of failed shards, and anything'
        ' >1 shows all output. DEFAULT: 1')
    parser.add_argument("executable", help="the executable to work on")

    parser.epilog = (
        'The environment variable MESOS_GTEST_RUNNER_FLAGS '
        'can be used to set a default set of flags. Flags passed on the '
        'command line always have precedence over these defaults.')

    args = parser.parse_args()

    if not args.executable:
        parser.print_usage()
        sys.exit(1)

    if not os.path.isfile(args.executable):
        print(
            Bcolors.colorize(
                "ERROR: File '{file}' does not exists"
                .format(file=args.executable), Bcolors.FAIL),
            file=sys.stderr)
        sys.exit(1)

    if not os.access(args.executable, os.X_OK):
        print(
            Bcolors.colorize(
                "ERROR: File '{file}' is not executable"
                .format(file=args.executable), Bcolors.FAIL),
            file=sys.stderr)
        sys.exit(1)

    if args.sequential and args.sequential.count(':-'):
        print(
            Bcolors.colorize(
                "ERROR: Cannot use negative filters in "
                "'sequential' parameter: '{filter}'"
                .format(filter=args.sequential), Bcolors.FAIL),
            file=sys.stderr)
        sys.exit(1)

    if args.sequential and os.environ.get('GTEST_FILTER') and \
            os.environ['GTEST_FILTER'].count(':-'):
        print(
            Bcolors.colorize(
                "ERROR: Cannot specify both 'sequential' ""option "
                "and environment variable 'GTEST_FILTER' "
                "containing negative filters",
                Bcolors.FAIL),
            file=sys.stderr)
        sys.exit(1)

    # Since empty strings are falsy, directly compare against `None`
    # to preserve an empty string passed via `GTEST_FILTER`.
    if os.environ.get('GTEST_FILTER') is not None:
        args.parallel = '{env_filter}:-{sequential_filter}'\
                         .format(env_filter=os.environ['GTEST_FILTER'],
                                 sequential_filter=args.sequential)
    else:
        args.parallel = '*:-{sequential_filter}'\
                         .format(sequential_filter=args.sequential)

    executable = args.executable
    delattr(args, 'executable')
    return executable, args


def validate_setup(options):
    """Validate the execution environment and the used options."""

    # Check the number of processes/threads a user is allowed to execute.
    try:
        # As a proxy for our requirements we assume that each test executable
        # launches a thread for every available CPU times a factor of 16 to
        # accommodate additional processes forked in parallel.
        requirement = options.jobs * multiprocessing.cpu_count() * 16

        nproc_limit = resource.getrlimit(resource.RLIMIT_NPROC)[0]

        if nproc_limit != resource.RLIM_INFINITY and nproc_limit < requirement:
            print(Bcolors.colorize(
                "Detected low process count ulimit ({} vs {}). Increase "
                "'ulimit -u' to avoid spurious test failures."
                .format(nproc_limit, requirement),
                Bcolors.WARNING),
                  file=sys.stderr)
            sys.exit(1)
    except Exception as err:
        print(Bcolors.colorize(
            "Could not check compatibility of ulimit settings: {}".format(err),
            Bcolors.WARNING),
              file=sys.stderr)
        sys.exit()


if __name__ == '__main__':
    EXECUTABLE, OPTIONS = parse_arguments()
    validate_setup(OPTIONS)

    EXECUTABLE = os.path.abspath(EXECUTABLE)

    def options_gen(executable, filter_, jobs):
        """Generator for options for a certain shard.

        Here we set up GoogleTest specific flags, and generate
        distinct shard indices.
        """
        args = [executable]

        # If we run in a terminal, enable colored test output. We
        # still allow users to disable this themselves via extra args.
        if sys.stdout.isatty():
            args.append('--gtest_color=yes')

        if filter_:
            args.append('--gtest_filter={}'.format(filter_))

        for job in range(jobs):
            yield job, jobs, args

    try:
        RESULTS = []

        POOL = multiprocessing.Pool(processes=OPTIONS.jobs)

        # Run parallel tests.
        RESULTS.extend(
            POOL.map(
                run_test,
                options_gen(EXECUTABLE, OPTIONS.parallel, OPTIONS.jobs)))

        # Now run sequential tests.
        if OPTIONS.sequential:
            RESULTS.extend(
                POOL.map(
                    run_test,
                    options_gen(EXECUTABLE, OPTIONS.sequential, 1)))

        # Count the number of failed shards and print results from
        # failed shards.
        #
        # NOTE: The `RESULTS` array stores the result for each
        # `run_test` invocation returning a tuple (success, output).
        NFAILED = sum(not success for success, _ in RESULTS)

        # TODO(bbannier): Introduce a verbosity which prints results
        # as they arrive; this likely requires some output parsing to
        # ensure results from different tests do not interleave.
        for success, out in RESULTS:
            if success:
                if OPTIONS.verbosity > 1:
                    print(out, file=sys.stdout)
            else:
                if OPTIONS.verbosity > 0:
                    print(out, file=sys.stderr)

        if NFAILED > 0:
            print(Bcolors.colorize(
                '\n[FAIL]: {nfailed} shard(s) have failed tests'.format(
                    nfailed=NFAILED),
                Bcolors.FAIL, Bcolors.BOLD),
                  file=sys.stderr)
        else:
            print(Bcolors.colorize('\n[PASS]', Bcolors.OKGREEN, Bcolors.BOLD))

        sys.exit(NFAILED)

    except KeyboardInterrupt:
        # Force a newline after intermediate test reports.
        print()

        print('Caught KeyboardInterrupt, terminating workers')

        POOL.terminate()
        POOL.join()

        sys.exit(1)

    except OSError as error:
        print(Bcolors.colorize(
            '\nERROR: {err}'.format(err=error),
            Bcolors.FAIL, Bcolors.BOLD))

        POOL.terminate()
        POOL.join()

        sys.exit(1)
