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

"""Runs checks for mesos style."""

import os
import re
import string
import subprocess
import sys


class LinterBase(object):
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

    def run_lint(self, source_paths):
        """
        A custom function to provide linting for 'linter_type'.
        It takes a list of source files to lint and returns the number
        of errors found during the linting process.

        It should print any errors as it encounters them to provide
        feedback to the caller.
        """
        pass

    def check_license_header(self, source_paths):
        """Checks the license headers of the given files."""
        error_count = 0
        for path in source_paths:
            with open(path) as source_file:
                # We read the three first lines of the file as the
                # first line could be a shebang and the second line empty.
                head = "".join([next(source_file) for _ in xrange(3)])

                # TODO(bbannier) We allow `Copyright` for
                # currently deviating files. This should be
                # removed one we have a uniform license format.
                regex = r'^{comment_prefix} [Licensed|Copyright]'.format(
                    comment_prefix=self.comment_prefix)
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

    def main(self, file_list):
        """
        This function takes a list of files and lints them for the
        class of files defined by 'linter_type'.
        """

        # Verify that source roots are accessible from current
        # working directory. A common error could be to call
        # the style checker from other (possibly nested) paths.
        for source_dir in self.source_dirs:
            if not os.path.exists(source_dir):
                print "Could not find '{dir}'".format(dir=source_dir)
                print 'Please run from the root of the mesos source directory'
                exit(1)

        # Add all source file candidates to candidates list.
        candidates = []
        for source_dir in self.source_dirs:
            for candidate in self.find_candidates(source_dir):
                candidates.append(candidate)

        # If file paths are specified, check all file paths that are
        # candidates; else check all candidates.
        file_paths = file_list if len(file_list) > 0 else candidates

        # Compute the set intersect of the input file paths and candidates.
        # This represents the reduced set of candidates to run lint on.
        candidates_set = set(candidates)
        clean_file_paths_set = set(path.rstrip() for path in file_paths)
        filtered_candidates_set = clean_file_paths_set.intersection(
            candidates_set)

        if filtered_candidates_set:
            plural = '' if len(filtered_candidates_set) == 1 else 's'
            print 'Checking {num_files} {linter} file{plural}'.format(
                num_files=len(filtered_candidates_set),
                linter=self.linter_type,
                plural=plural)

            license_errors = self.check_license_header(filtered_candidates_set)
            encoding_errors = self.check_encoding(list(filtered_candidates_set))
            lint_errors = self.run_lint(list(filtered_candidates_set))
            total_errors = license_errors + encoding_errors + lint_errors

            sys.stderr.write('Total errors found: {num_errors}\n'.format(
                num_errors=total_errors))

            return total_errors
        else:
            print "No {linter} files to lint".format(linter=self.linter_type)
            return 0


class CppLinter(LinterBase):
    """The linter for C++ files, uses cpplint."""
    linter_type = 'C++'

    source_dirs = ['src',
                   'include',
                   os.path.join('3rdparty', 'libprocess'),
                   os.path.join('3rdparty', 'stout')]

    exclude_files = '(' \
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
        process = subprocess.Popen(
            ['python', 'support/cpplint.py', rules_filter] + source_paths,
            stderr=subprocess.PIPE,
            close_fds=True)

        # Lines are stored and filtered, only showing found errors instead
        # of e.g., 'Done processing XXX.' which tends to be dominant output.
        for line in process.stderr:
            if re.match('^(Done processing |Total errors found: )', line):
                continue
            sys.stderr.write(line)

        process.wait()
        return process.returncode


class PyLinter(LinterBase):
    """The linter for Python files, uses pylint."""
    linter_type = 'Python'

    source_dirs = [os.path.join('src', 'python', 'cli_new'),
                   os.path.join('src', 'python', 'lib')]

    exclude_files = '(' \
                    r'protobuf\-2\.4\.1|' \
                    r'googletest\-release\-1\.8\.0|' \
                    r'glog\-0\.3\.3|' \
                    r'boost\-1\.53\.0|' \
                    r'libev\-4\.15|' \
                    r'java/jni|\.virtualenv' \
                    ')'

    source_files = r'\.(py)$'

    comment_prefix = '#'

    def run_lint(self, source_paths):
        """
        Runs pylint over given files.

        https://google.github.io/styleguide/pyguide.html
        """

        cli_dir = os.path.abspath(self.source_dirs[0])
        source_files = ' '.join(source_paths)

        process = subprocess.Popen(
            [('. {virtualenv_dir}/bin/activate;'
              ' PYTHONPATH={lib_dir}:{bin_dir} pylint'
              ' --rcfile={config} --ignore={ignore} {files}').
             format(virtualenv_dir=os.path.join(cli_dir, '.virtualenv'),
                    lib_dir=os.path.join(cli_dir, 'lib'),
                    bin_dir=os.path.join(cli_dir, 'bin'),
                    config=os.path.join(cli_dir, 'pylint.config'),
                    ignore=os.path.join(cli_dir, 'bin', 'mesos'),
                    files=source_files)],
            shell=True, stdout=subprocess.PIPE)

        num_errors = 0
        for line in process.stdout:
            if not line.startswith('*'):
                num_errors += 1
            sys.stderr.write(line)

        return num_errors

    def __should_build_virtualenv(self, file_list):
        cli_dir = os.path.abspath(self.source_dirs[0])

        if not os.path.isdir(os.path.join(cli_dir, '.virtualenv')):
            print 'Virtualenv for python linter not detected ... building'
            return True

        basenames = []
        if file_list:
            basenames = [os.path.basename(path) for path in file_list]

        if 'pip-requirements.txt' in basenames:
            print 'The "pip-requirements.txt" file has changed.'
            return True

        if 'mesos.bash_completion' in basenames:
            print 'The "mesos.bash_completion" file has changed.'
            return True

        # NOTE: If the file list is empty, we are linting the entire codebase.
        # We should always rebuild the virtualenv in this case.
        if len(file_list) <= 0:
            return True

        return False

    def __build_virtualenv(self):
        """Rebuild the virtualenv."""
        cli_dir = os.path.abspath(self.source_dirs[0])

        print 'Rebuilding virtualenv ...'

        process = subprocess.Popen(
            [os.path.join(cli_dir, 'bootstrap')],
            stdout=subprocess.PIPE)

        output = ''
        for line in process.stdout:
            output += line

        process.wait()

        if process.returncode != 0:
            sys.stderr.write(output)
            sys.exit(1)

    def main(self, file_list):
        """Override main to rebuild our virtualenv if necessary."""
        if self.__should_build_virtualenv(file_list):
            self.__build_virtualenv()

        return super(PyLinter, self).main(file_list)


if __name__ == '__main__':
    CPP_LINTER = CppLinter()
    CPP_ERRORS = CPP_LINTER.main(sys.argv[1:])
    PY_LINTER = PyLinter()
    PY_ERRORS = PY_LINTER.main(sys.argv[1:])
    sys.exit(CPP_ERRORS + PY_ERRORS)
