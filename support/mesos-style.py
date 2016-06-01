#!/usr/bin/env python

''' Runs checks for mesos style. '''

import os
import re
import string
import subprocess
import sys

# Root source paths (will be traversed recursively).
source_dirs = ['src',
               'include',
               os.path.join('3rdparty', 'libprocess'),
               os.path.join('3rdparty', 'stout')]

# Add file paths and patterns which should not be checked
# This should include 3rdparty libraries, includes and machine generated
# source.
exclude_files = '(protobuf\-2\.4\.1|gmock\-1\.6\.0|glog\-0\.3\.3|boost\-1\.53\.0|libev\-4\.15|java/jni|\.pb\.cc|\.pb\.h|\.md)'

source_files = '\.(cpp|hpp|cc|h)$'

def find_candidates(root_dir):
    exclude_file_regex = re.compile(exclude_files)
    source_criteria_regex = re.compile(source_files)
    for root, dirs, files in os.walk(root_dir):
        for name in files:
            path = os.path.join(root, name)
            if exclude_file_regex.search(path) is not None:
                continue

            if source_criteria_regex.search(name) is not None:
                yield path

def run_lint(source_paths):
    '''
    Runs cpplint over given files.

    http://google-styleguide.googlecode.com/svn/trunk/cpplint/cpplint.py
    '''

    # See cpplint.py for full list of rules.
    active_rules = [
        'build/class',
        'build/deprecated',
        'build/endif_comment',
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
    p = subprocess.Popen(
        ['python', 'support/cpplint.py', rules_filter] + source_paths,
        stderr=subprocess.PIPE,
        close_fds=True)

    # Lines are stored and filtered, only showing found errors instead
    # of e.g., 'Done processing XXX.' which tends to be dominant output.
    for line in p.stderr:
        if re.match('^(Done processing |Total errors found: )', line):
            continue
        sys.stderr.write(line)

    p.wait()
    return p.returncode

def check_license_header(source_paths):
    ''' Checks the license headers of the given files. '''
    error_count = 0
    for path in source_paths:
        with open(path) as source_file:
            head = source_file.readline()

            # Check that opening comment has correct style.
            # TODO(bbannier) We allow `Copyright` for currently deviating files.
            # This should be removed one we have a uniform license format.
            if not re.match(r'^\/\/ [Licensed|Copyright]', head):
                sys.stderr.write(
                    "{path}:1:  A license header should appear on the file's "
                    "first line starting with '// Licensed'.: {head}".\
                        format(path=path, head=head))
                error_count += 1

    return error_count

def check_encoding(source_paths):
    '''
    Checks for encoding errors in the given files. Source
    code files must contain only printable ascii characters.
    This excludes the extended ascii characters 128-255.
    http://www.asciitable.com/
    '''
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


if __name__ == '__main__':
    # Verify that source roots are accessible from current working directory.
    # A common error could be to call the style checker from other
    # (possibly nested) paths.
    for source_dir in source_dirs:
        if not os.path.exists(source_dir):
            print "Could not find '{dir}'".format(dir=source_dir)
            print 'Please run from the root of the mesos source directory'
            exit(1)

    # Add all source file candidates to candidates list.
    candidates = []
    for source_dir in source_dirs:
        for candidate in find_candidates(source_dir):
            candidates.append(candidate)

    # If file paths are specified, check all file paths that are
    # candidates; else check all candidates.
    file_paths = sys.argv[1:] if len(sys.argv) > 1 else candidates

    # Compute the set intersect of the input file paths and candidates.
    # This represents the reduced set of candidates to run lint on.
    candidates_set = set(candidates)
    clean_file_paths_set = set(map(lambda x: x.rstrip(), file_paths))
    filtered_candidates_set = clean_file_paths_set.intersection(
        candidates_set)

    if filtered_candidates_set:
        print 'Checking {num_files} files'.\
                format(num_files=len(filtered_candidates_set))
        license_errors = check_license_header(filtered_candidates_set)
        encoding_errors = check_encoding(list(filtered_candidates_set))
        lint_errors = run_lint(list(filtered_candidates_set))
        total_errors = license_errors + encoding_errors + lint_errors
        sys.stderr.write('Total errors found: {num_errors}\n'.\
                            format(num_errors=total_errors))
        sys.exit(total_errors)
    else:
        print "No files to lint\n"
        sys.exit(0)
