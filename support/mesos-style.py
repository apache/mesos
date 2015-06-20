#!/usr/bin/env python

# Runs style checker using Google's cpplint.
# http://google-styleguide.googlecode.com/svn/trunk/cpplint/cpplint.py

import os
import re
import subprocess
import sys

# See cpplint.py for full list of rules.
active_rules = ['build/class',
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
                'whitespace/todo']

# Root source paths (will be traversed recursively).
source_dirs = ['src',
               'include',
               os.path.join('3rdparty', 'libprocess')]

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
    rules_filter = '--filter=-,+' + ',+'.join(active_rules)
    print 'Checking ' + str(len(source_paths)) + ' files using filter ' \
        + rules_filter
    p = subprocess.Popen(
        ['python', 'support/cpplint.py', rules_filter] + source_paths,
        stderr=subprocess.PIPE,
        close_fds=True)

    # Lines are stored and filtered, only showing found errors instead
    # of 'Done processing XXX.' which tends to be dominant output.
    lint_out_regex = re.compile(':')
    for line in p.stderr:
        if lint_out_regex.search(line) is not None:
            sys.stdout.write(line)

    p.wait()
    return p.returncode


if __name__ == '__main__':
    # Verify that source roots are accessible from current working directory.
    # A common error could be to call the style checker from other
    # (possibly nested) paths.
    for source_dir in source_dirs:
        if not os.path.exists(source_dir):
            print 'Could not find "' + source_dir + '"'
            print 'Please run from the root of the mesos source directory'
            exit(1)

    # Add all source file candidates to candidates list.
    candidates = []
    for source_dir in source_dirs:
        for candidate in find_candidates(source_dir):
            candidates.append(candidate)

    if len(sys.argv) == 1:
        # No file paths specified, run lint on all candidates.
        sys.exit(run_lint(candidates))
    else:
        # File paths specified, run lint on all file paths that are candidates.
        file_paths = sys.argv[1:]

        # Compute the set intersect of the input file paths and candidates.
        # This represents the reduced set of candidates to run lint on.
        candidates_set = set(candidates)
        clean_file_paths_set = set(map(lambda x: x.rstrip(), file_paths))
        filtered_candidates_set = clean_file_paths_set.intersection(
            candidates_set)

        if filtered_candidates_set:
            sys.exit(run_lint(list(filtered_candidates_set)))
        else:
            print "No files to lint\n"
            sys.exit(0)
