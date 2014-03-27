#!/usr/bin/env python

# Runs style checker using Google's cpplint.
# http://google-styleguide.googlecode.com/svn/trunk/cpplint/cpplint.py

import os
import re
import subprocess
import sys

# Build active rules from list:
#
# build/class
# build/deprecated
# build/endif_comment
# build/explicit_make_pair
# build/forward_decl
# build/header_guard
# build/include
# build/include_alpha
# build/include_order
# build/include_what_you_use
# build/namespaces
# build/printf_format
# build/storage_class
# legal/copyright
# readability/alt_tokens
# readability/braces
# readability/casting
# readability/check
# readability/constructors
# readability/fn_size
# readability/function
# readability/multiline_comment
# readability/multiline_string
# readability/namespace
# readability/nolint
# readability/streams
# readability/todo
# readability/utf8
# runtime/arrays
# runtime/casting
# runtime/explicit
# runtime/int
# runtime/init
# runtime/invalid_increment
# runtime/member_string_references
# runtime/memset
# runtime/operator
# runtime/printf
# runtime/printf_format
# runtime/references
# runtime/rtti
# runtime/sizeof
# runtime/string
# runtime/threadsafe_fn
# whitespace/blank_line
# whitespace/braces
# whitespace/comma
# whitespace/comments
# whitespace/empty_loop_body
# whitespace/end_of_line
# whitespace/ending_newline
# whitespace/forcolon
# whitespace/indent
# whitespace/labels
# whitespace/line_length
# whitespace/newline
# whitespace/operators
# whitespace/parens
# whitespace/semicolon
# whitespace/tab
# whitespace/todo

# Currently, only tabs are checked.
active_rules = '--filter=-,+whitespace/tab'

# Root source paths (will be traversed recursively).
source_dirs = ['src',
               'include',
               os.path.join('3rdparty', 'libprocess')]

# Add file paths and patterns which should not be checked
# This should include 3rdparty libraries, includes and machine generated
# source.
exclude_files = '(protobuf\-2\.4\.1|gmock\-1\.6\.0|glog\-0\.3\.3|boost\-1\.53\.0|libev\-4\.15|java/jni|\.pb\.cc|\.pb\.h)'

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
    print 'Checking ' + str(len(source_paths)) + ' files...'
    p = subprocess.Popen(
        ['python', 'support/cpplint.py', active_rules] + source_paths,
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

    sys.exit(run_lint(candidates))
