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

"""Runs checks for mesos style."""

import os
import platform
import re
import string
import subprocess
import sys

from pathlib import PurePath


class LinterBase():
    """
    This is an abstract class that provides the base functionality for
    linting files in the mesos project. Its 'main()' function
    walks through the set of files passed to it and runs some
    standard linting over them. This includes checking for license headers
    and checking for non-supported characters. From there it calls a
    'run_lint()' function that can be overridden to provide
    customizable style checks for a specific class of files (e.g. C++,
    Python, etc.).

    Any class that extends from 'LinterBase' should override the
    following class variables / functions:

    linter_type
    source_dirs
    exclude_files
    source_files
    comment_prefix

    run_lint()

    Please see the comments below for details on how to override each
    variable.
    """
    # The name of the linter to help with printing which linter files
    # are currently being processed by.
    linter_type = ''

    # Root source paths (will be traversed recursively).
    source_dirs = []

    # Add file paths and patterns which should not be checked
    # This should include 3rdparty libraries, includes and machine generated
    # source.
    exclude_files = ''

    # A regex of possible matches for your source files.
    source_files = ''

    # A prefix at the beginning of the line to demark comments (e.g. '//')
    comment_prefix = ''

    def check_encoding(self, source_paths):
        """
        Checks for encoding errors in the given files. Source
        code files must contain only printable ascii characters.
        This excludes the extended ascii characters 128-255.
        http://www.asciitable.com/
        """
        error_count = 0
        for path in source_paths:
            with open(path) as source_file:
                for line_number, line in enumerate(source_file):
                    # If we find an error, add 1 to both the character and
                    # the line offset to give them 1-based indexing
                    # instead of 0 (as is common in most editors).
                    char_errors = [offset for offset, char in enumerate(line)
                                   if char not in string.printable]
                    if char_errors:
                        sys.stderr.write(
                            "{path}:{line_number}:  Non-printable characters"
                            " found at [{offsets}]: \"{line}\"\n".format(
                                path=path,
                                line_number=line_number + 1,
                                offsets=', '.join([str(offset + 1) for offset
                                                   in char_errors]),
                                line=line.rstrip()))
                        error_count += 1

        return error_count

    def check_license_header(self, source_paths):
        """Checks the license headers of the given files."""
        error_count = 0
        for path in source_paths:
            with open(path) as source_file:
                # We read the three first lines of the file as the
                # first line could be a shebang and the second line empty.
                head = "".join([next(source_file) for _ in range(3)])

                # TODO(bbannier) We allow `Copyright` for
                # currently deviating files. This should be
                # removed one we have a uniform license format.
                regex = r'^{comment_prefix} [Licensed|Copyright]'.format(
                    comment_prefix=self.comment_prefix)
                # pylint: disable=no-member
                regex = re.compile(regex, re.MULTILINE)

                if not regex.search(head):
                    sys.stderr.write(
                        "{path}:1: A license header should appear's on one of"
                        " the first line of the file starting with"
                        " '{comment_prefix} Licensed'.: {head}".format(
                            path=path,
                            head=head,
                            comment_prefix=self.comment_prefix))
                    error_count += 1

        return error_count

    def find_candidates(self, root_dir):
        """
        Search through the all files rooted at 'root_dir' and compare
        them against 'self.exclude_files' and 'self.source_files' to
        come up with a set of candidate files to lint.
        """
        exclude_file_regex = re.compile(self.exclude_files)
        source_criteria_regex = re.compile(self.source_files)
        for root, _, files in os.walk(root_dir):
            for name in files:
                path = os.path.join(root, name)
                if exclude_file_regex.search(path) is not None:
                    continue

                if source_criteria_regex.search(name) is not None:
                    yield path

    def run_command_in_virtualenv(self, command):
        """
        Activate the virtual environment, run the
        given command and return its output.
        """
        virtualenv = os.path.join('support', '.virtualenv')

        if platform.system() == 'Windows':
            command = r'{virtualenv_path}\Scripts\activate.bat & {cmd}'.format(
                virtualenv_path=virtualenv, cmd=command)
        else:
            command = '. {virtualenv_path}/bin/activate; {cmd}'.format(
                virtualenv_path=virtualenv, cmd=command)

        return subprocess.Popen(command, shell=True, stdout=subprocess.PIPE)

    # pylint: disable=unused-argument
    def run_lint(self, source_paths):
        """
        A custom function to provide linting for 'linter_type'.
        It takes a list of source files to lint and returns the number
        of errors found during the linting process.

        It should print any errors as it encounters them to provide
        feedback to the caller.
        """
        return 0

    def main(self, modified_files):
        """
        This function takes a list of files and lints them for the
        class of files defined by 'linter_type'.
        """

        # Verify that source roots are accessible from current
        # working directory. A common error could be to call
        # the style checker from other (possibly nested) paths.
        for source_dir in self.source_dirs:
            if not os.path.exists(source_dir):
                print("Could not find '{dir}'".format(dir=source_dir))
                print('Please run from the root of the mesos source directory')
                exit(1)

        # Add all source file candidates to candidates list.
        candidates = []
        for source_dir in self.source_dirs:
            for candidate in self.find_candidates(source_dir):
                candidates.append(candidate)

        # Normalize paths of any files given.
        modified_files = [os.fspath(PurePath(f)) for f in modified_files]

        # If file paths are specified, check all file paths that are
        # candidates; else check all candidates.
        file_paths = modified_files if modified_files else candidates

        # Compute the set intersect of the input file paths and candidates.
        # This represents the reduced set of candidates to run lint on.
        candidates_set = set(candidates)
        clean_file_paths_set = set(path.rstrip() for path in file_paths)
        filtered_candidates_set = clean_file_paths_set.intersection(
            candidates_set)

        if filtered_candidates_set:
            plural = '' if len(filtered_candidates_set) == 1 else 's'
            print('Checking {num_files} {linter} file{plural}'.format(
                num_files=len(filtered_candidates_set),
                linter=self.linter_type,
                plural=plural))

            license_errors = self.check_license_header(filtered_candidates_set)
            encoding_errors = self.check_encoding(list(filtered_candidates_set))
            lint_errors = self.run_lint(list(filtered_candidates_set))
            total_errors = license_errors + encoding_errors + lint_errors

            if total_errors > 0:
                sys.stderr.write('Total errors found: {num_errors}\n'.format(
                    num_errors=total_errors))

            return total_errors

        return 0


class CppLinter(LinterBase):
    """The linter for C++ files, uses cpplint."""
    linter_type = 'C++'

    source_dirs = ['src',
                   'include',
                   os.path.join('3rdparty', 'libprocess'),
                   os.path.join('3rdparty', 'stout')]

    exclude_files = '(' \
                    r'elfio\-3\.2|' \
                    r'protobuf\-2\.4\.1|' \
                    r'googletest\-release\-1\.8\.0|' \
                    r'glog\-0\.3\.3|' \
                    r'boost\-1\.53\.0|' \
                    r'libev\-4\.15|' \
                    r'java/jni|' \
                    r'\.pb\.cc|\.pb\.h|\.md|\.virtualenv' \
                    ')'

    source_files = r'\.(cpp|hpp|cc|h)$'

    comment_prefix = r'\/\/'

    def run_lint(self, source_paths):
        """
        Runs cpplint over given files.

        http://google-styleguide.googlecode.com/svn/trunk/cpplint/cpplint.py
        """

        # See cpplint.py for full list of rules.
        active_rules = [
            'build/class',
            'build/deprecated',
            'build/endif_comment',
            'build/nullptr',
            'readability/todo',
            'readability/namespace',
            'runtime/vlog',
            'whitespace/blank_line',
            'whitespace/comma',
            'whitespace/end_of_line',
            'whitespace/ending_newline',
            'whitespace/forcolon',
            'whitespace/indent',
            'whitespace/line_length',
            'whitespace/operators',
            'whitespace/semicolon',
            'whitespace/tab',
            'whitespace/comments',
            'whitespace/todo']

        rules_filter = '--filter=-,+' + ',+'.join(active_rules)

        # We do not use a version of cpplint available through pip as
        # we use a custom version (see cpplint.path) to lint C++ files.
        process = subprocess.Popen(
            [sys.executable, 'support/cpplint.py',
             rules_filter] + source_paths,
            stderr=subprocess.PIPE,
            close_fds=True)

        # Lines are stored and filtered, only showing found errors instead
        # of e.g., 'Done processing XXX.' which tends to be dominant output.
        for line in process.stderr:
            line = line.decode(sys.stdout.encoding)
            if re.match('^(Done processing |Total errors found: )', line):
                continue
            sys.stderr.write(line)

        process.wait()
        return process.returncode


class JsLinter(LinterBase):
    """The linter for JavaScript files, uses eslint."""
    linter_type = 'JavaScript'

    source_dirs = [os.path.join('src', 'webui')]

    exclude_files = '(' \
                    r'angular\-1\.2\.32|' \
                    r'angular\-route\-1\.2\.32|' \
                    r'bootstrap\-table\-1\.11\.1|' \
                    r'clipboard\-1\.5\.16|' \
                    r'jquery\-3\.2\.1|' \
                    r'relative\-date|' \
                    r'ui\-bootstrap\-tpls\-0\.9\.0|' \
                    r'angular\-route\-1\.2\.32|' \
                    r'underscore\-1\.4\.3' \
                    ')'

    source_files = r'\.(js)$'

    comment_prefix = '//'

    def run_lint(self, source_paths):
        """
        Runs eslint over given files.

        https://eslint.org/docs/user-guide/configuring
        """

        num_errors = 0

        source_files = ' '.join(source_paths)
        config_path = os.path.join('support', '.eslintrc.js')

        process = self.run_command_in_virtualenv(
            'eslint {files} -c {config} -f compact'.format(
                files=source_files,
                config=config_path
            )
        )

        for line in process.stdout:
            line = line.decode(sys.stdout.encoding)
            if "Error -" in line or "Warning -" in line:
                sys.stderr.write(line)
                if "Error -" in line:
                    num_errors += 1

        return num_errors


class PyLinter(LinterBase):
    """The linter for Python files, uses pylint."""
    linter_type = 'Python'

    cli_dir = os.path.join('src', 'python', 'cli_new')
    lib_dir = os.path.join('src', 'python', 'lib')
    support_dir = os.path.join('support')
    source_dirs_to_lint_with_venv = [support_dir]
    source_dirs_to_lint_with_tox = [cli_dir, lib_dir]
    source_dirs = source_dirs_to_lint_with_tox + source_dirs_to_lint_with_venv

    exclude_files = '(' \
                    r'protobuf\-2\.4\.1|' \
                    r'googletest\-release\-1\.8\.0|' \
                    r'glog\-0\.3\.3|' \
                    r'boost\-1\.53\.0|' \
                    r'libev\-4\.15|' \
                    r'java/jni|' \
                    r'\.virtualenv|' \
                    r'\.tox' \
                    ')'

    source_files = r'\.(py)$'

    comment_prefix = '#'

    pylint_config = os.path.abspath(os.path.join('support', 'pylint.config'))

    def run_tox(self, configfile, args, tox_env=None, recreate=False):
        """
        Runs tox with given configfile and args. Optionally set tox env
        and/or recreate the tox-managed virtualenv.
        """
        support_dir = os.path.dirname(__file__)
        bin_dir = 'Script' if platform.system() == 'Windows' else 'bin'

        cmd = [os.path.join(support_dir, '.virtualenv', bin_dir, 'tox')]
        cmd += ['-qq']
        cmd += ['-c', configfile]
        if tox_env is not None:
            cmd += ['-e', tox_env]
        if recreate:
            cmd += ['--recreate']
        cmd += ['--']
        cmd += args

        # We do not use `run_command_in_virtualenv()` here, as we
        # directly call `tox` from inside the virtual environment bin
        # directory without activating the virtualenv.
        return subprocess.Popen(cmd, stdout=subprocess.PIPE)

    def filter_source_files(self, source_dir, source_files):
        """
        Filters out files starting with source_dir.
        """
        return [f for f in source_files if f.startswith(source_dir)]

    def lint_source_files_under_source_dir(self, source_dir, source_files):
        """
        Runs pylint directly or indirectly throgh tox on source_files which
        are under source_dir. If tox is to be used, it must be configured
        in source_dir, i.e. a tox.ini must be present.
        """
        filtered_source_files = self.filter_source_files(
            source_dir, source_files)

        if not filtered_source_files:
            return 0

        if source_dir in self.source_dirs_to_lint_with_tox:
            process = self.run_tox(
                configfile=os.path.join(source_dir, 'tox.ini'),
                args=['--rcfile='+self.pylint_config] + filtered_source_files,
                tox_env='py3-lint')
        else:
            process = self.run_command_in_virtualenv(
                'pylint --score=n --rcfile={rcfile} {files}'.format(
                    rcfile=self.pylint_config,
                    files=' '.join(filtered_source_files)))

        num_errors = 0
        for line in process.stdout:
            line = line.decode(sys.stdout.encoding)
            if re.search(r'[RCWEF][0-9]{4}:', line):
                num_errors += 1
            sys.stderr.write(line)

        return num_errors

    def run_lint(self, source_paths):
        """
        Runs pylint over given files.

        https://google.github.io/styleguide/pyguide.html
        """
        num_errors = 0

        for source_dir in self.source_dirs:
            num_errors += self.lint_source_files_under_source_dir(
                source_dir, source_paths)

        return num_errors


def should_build_virtualenv(modified_files):
    """
    Check if we should build the virtual environment required.
    This is the case if the requirements of the environment
    have changed or if the support script is run with no
    arguments (meaning that the entire codebase should be linted).
    """
    # NOTE: If the file list is empty, we are linting the entire test
    # codebase. We should always rebuild the virtualenv in this case.
    if not modified_files:
        return True

    support_dir = os.path.dirname(__file__)
    bin_dir = 'Script' if platform.system() == 'Windows' else 'bin'

    interpreter = os.path.basename(sys.executable)
    interpreter = os.path.join(support_dir, '.virtualenv', bin_dir, interpreter)
    if not os.path.isfile(interpreter):
        return True

    basenames = [os.path.basename(path) for path in modified_files]

    if 'pip-requirements.txt' in basenames:
        print('The "pip-requirements.txt" file has changed.')
        return True

    if 'build-virtualenv' in basenames:
        print('The "build-virtualenv" file has changed.')
        return True

    # The JS and Python linters require a virtual environment.
    # If all the files modified are not JS or Python files,
    # we do not need to build the virtual environment.
    # TODO(ArmandGrillet): There should be no duplicated logic to know
    # which linters to instantiate depending on the files to analyze.
    if not os.path.isdir(os.path.join('support', '.virtualenv')):
        js_and_python_files = [JsLinter().source_files, PyLinter().source_files]
        js_and_python_files_regex = re.compile('|'.join(js_and_python_files))

        for basename in basenames:
            if js_and_python_files_regex.search(basename) is not None:
                print('Virtualenv not detected and required... building')
                return True

    return False


def build_virtualenv():
    """
    Rebuild the virtualenv by running a bootstrap script.
    This will exit the program if there is a failure.
    """
    print('Rebuilding virtualenv...')

    python3_env = os.environ.copy()
    python3_env["PYTHON"] = sys.executable

    build_virtualenv_file = [os.path.join('support', 'build-virtualenv')]

    if platform.system() == 'Windows':
        # TODO(andschwa): Port more of the `build-virtualenv` Bash script.
        python_dir = os.path.dirname(sys.executable)
        virtualenv = os.path.join(python_dir, 'Scripts', 'virtualenv.exe')
        build_virtualenv_file = [virtualenv,
                                 '--no-site-packages',
                                 'support/.virtualenv']

    process = subprocess.Popen(
        build_virtualenv_file,
        env=python3_env,
        stdout=subprocess.PIPE)

    output = ''
    for line in process.stdout:
        output += line.decode(sys.stdout.encoding)

    process.wait()

    if process.returncode != 0:
        sys.stderr.write(output)
        sys.exit(1)

    # TODO(andschwa): Move this into a script like above.
    if platform.system() == 'Windows':
        def run_command_in_virtualenv(command):
            """
            Stolen from `PyLinter`, runs command in virtualenv.
            """
            virtualenv = os.path.join('support',
                                      '.virtualenv',
                                      'Scripts',
                                      'activate.bat')
            command = '{virtualenv_path} & {cmd}'.format(
                virtualenv_path=virtualenv, cmd=command)

            return subprocess.Popen(command, shell=True, stdout=subprocess.PIPE)

        pip_install_pip = 'python.exe -m pip install --upgrade pip'
        process = run_command_in_virtualenv(pip_install_pip)
        for line in process.stdout:
            output += line.decode(sys.stdout.encoding)
        process.wait()

        if process.returncode != 0:
            sys.stderr.write(output)
            sys.exit(1)

        pip_reqs = 'python.exe -m pip install -r support/pip-requirements.txt'
        process = run_command_in_virtualenv(pip_reqs)
        for line in process.stdout:
            output += line.decode(sys.stdout.encoding)
        process.wait()

        if process.returncode != 0:
            sys.stderr.write(output)
            sys.exit(1)

if __name__ == '__main__':
    if should_build_virtualenv(sys.argv[1:]):
        build_virtualenv()

    # TODO(ArmandGrillet): We should only instantiate the linters
    # required to lint the files to analyze. See MESOS-8351.
    CPP_LINTER = CppLinter()
    CPP_ERRORS = CPP_LINTER.main(sys.argv[1:])
    JS_LINTER = JsLinter()
    JS_ERRORS = JS_LINTER.main(sys.argv[1:])
    PY_LINTER = PyLinter()
    PY_ERRORS = PY_LINTER.main(sys.argv[1:])
    sys.exit(CPP_ERRORS + JS_ERRORS + PY_ERRORS)
