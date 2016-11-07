#!/usr/bin/env python
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


from __future__ import print_function

import multiprocessing
import optparse
import os
import signal
import subprocess
import sys


DEFAULT_NUM_JOBS = int(multiprocessing.cpu_count() * 1.5)


class Bcolors:
    """
    A collection of tty output modifiers.

    To switch the output of a string, prefix it with the desired
    modifier, and terminate it with 'ENDC'.
    """

    HEADER = '\033[95m' if sys.stdout.isatty() else ''
    OKBLUE = '\033[94m' if sys.stdout.isatty() else ''
    OKGREEN = '\033[92m' if sys.stdout.isatty() else ''
    WARNING = '\033[93m' if sys.stdout.isatty() else ''
    FAIL = '\033[91m'if sys.stdout.isatty() else ''
    ENDC = '\033[0m' if sys.stdout.isatty() else ''
    BOLD = '\033[1m' if sys.stdout.isatty() else ''
    UNDERLINE = '\033[4m' if sys.stdout.isatty() else ''

    @staticmethod
    def colorize(string, *color_codes):
        """Decorate a string with a number of color codes."""
        colors = ''.join(color_codes)
        return '{begin}{string}{end}'.format(
            begin=colors if sys.stdout.isatty() else '',
            string=string,
            end=Bcolors.ENDC if sys.stdout.isatty() else '')


def run_test(opts):
    """
    Perform an actual run of the test executable.

    Expects a list of parameters giving the number of the current
    shard, the total number of shards, and the executable to run.
    """
    shard, nshards, executable = opts

    signal.signal(signal.SIGINT, signal.SIG_IGN)

    env = os.environ.copy()
    env['GTEST_TOTAL_SHARDS'] = str(nshards)
    env['GTEST_SHARD_INDEX'] = str(shard)

    try:
        output = subprocess.check_output(
            executable.split(),
            stderr=subprocess.STDOUT,
            env=env,
            universal_newlines=True)
        print(Bcolors.colorize('.', Bcolors.OKGREEN), end='')
        sys.stdout.flush()
        return True, output
    except subprocess.CalledProcessError as ex:
        print(Bcolors.colorize('.', Bcolors.FAIL), end='')
        sys.stdout.flush()
        return False, ex.output


def parse_arguments():
    """Return the executable to work on, and a list of options."""
    parser = optparse.OptionParser(
        usage='Usage: %prog [options] <test> [-- <test_options>]')

    parser.add_option(
        '-j', '--jobs', type='int',
        default=DEFAULT_NUM_JOBS,
        help='number of parallel jobs to spawn. DEFAULT: {default_}'
        .format(default_=DEFAULT_NUM_JOBS))

    parser.add_option(
        '-s', '--sequential', type='string',
        default='',
        help='gtest filter for tests to run sequentially')

    parser.add_option(
        '-v', '--verbosity', type='int',
        default=1,
        help='output verbosity:'
        ' 0 only shows summarized information,'
        ' 1 also shows full logs of failed shards, and anything'
        ' >1 shows all output. DEFAULT: 1')

    (options, executable) = parser.parse_args()

    if not executable:
        parser.print_usage()
        sys.exit(1)

    if not os.path.isfile(executable[0]):
        print(
            Bcolors.colorize(
                "ERROR: File '{file}' does not exists"
                .format(file=executable[0]), Bcolors.FAIL),
            file=sys.stderr)
        sys.exit(1)

    if not os.access(executable[0], os.X_OK):
        print(
            Bcolors.colorize(
                "ERROR: File '{file}' is not executable"
                .format(file=executable[0]), Bcolors.FAIL),
            file=sys.stderr)
        sys.exit(1)

    if options.sequential and options.sequential.count(':-'):
        print(
            Bcolors.colorize(
                "ERROR: Cannot use negative filters in "
                "'sequential' parameter: '{filter}'"
                .format(filter=options.sequential), Bcolors.FAIL),
            file=sys.stderr)
        sys.exit(1)

    if options.sequential and os.environ.get('GTEST_FILTER') and \
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
    if os.environ.get('GTEST_FILTER') != None:
        options.parallel = '{env_filter}:-{sequential_filter}'\
                         .format(env_filter=os.environ['GTEST_FILTER'],
                                 sequential_filter=options.sequential)
    else:
        options.parallel = '*:-{sequential_filter}'\
                         .format(sequential_filter=options.sequential)

    return executable, options


if __name__ == '__main__':
    EXECUTABLE, OPTIONS = parse_arguments()

    def options_gen(executable, filter_, jobs):
        """Generator for options for a certain shard.

        Here we set up GoogleTest specific flags, and generate
        distinct shard indices.
        """
        opts = range(jobs)

        # If we run in a terminal, enable colored test output. We
        # still allow users to disable this themselves via extra args.
        if sys.stdout.isatty():
            args = executable[1:]
            executable = '{exe} --gtest_color=yes {args}'\
                         .format(exe=executable[0], args=args if args else '')

        if filter_:
            executable = '{exe} --gtest_filter={filter}'\
                         .format(exe=executable, filter=filter_)

        for opt in opts:
            yield opt, jobs, executable

    try:
        RESULTS = []

        POOL = multiprocessing.Pool(processes=OPTIONS.jobs)

        # Run parallel tests.
        #
        # Multiprocessing's `map` cannot properly handle `KeyboardInterrupt` in
        # some python versions. Use `map_async` with an explicit timeout
        # instead. See http://stackoverflow.com/a/1408476.
        RESULTS.extend(
            POOL.map_async(
                run_test,
                options_gen(
                    EXECUTABLE, OPTIONS.parallel, OPTIONS.jobs)).get(
                        timeout=sys.maxint))

        # Now run sequential tests.
        if OPTIONS.sequential:
            RESULTS.extend(
                POOL.map_async(
                    run_test,
                    options_gen(
                        EXECUTABLE, OPTIONS.sequential, 1)).get(
                            timeout=sys.maxint))

        # Count the number of failed shards and print results from
        # failed shards.
        #
        # NOTE: The `RESULTS` array stores the result for each
        # `run_test` invocation returning a tuple (success, output).
        NFAILED = len([success for success, __ in RESULTS if not success])

        # TODO(bbannier): Introduce a verbosity which prints results
        # as they arrive; this likely requires some output parsing to
        # ensure results from different tests do not interleave.
        for result in RESULTS:
            if not result[0]:
                if OPTIONS.verbosity > 0:
                    print(result[1], file=sys.stderr)
            else:
                if OPTIONS.verbosity > 1:
                    print(result[1], file=sys.stdout)

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

    except OSError as ex:
        print(Bcolors.colorize(
            '\nERROR: {ex}'.format(ex=ex),
            Bcolors.FAIL, Bcolors.BOLD))

        POOL.terminate()
        POOL.join()

        sys.exit(1)
