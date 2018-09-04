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
This script is typically used by Mesos committers to push a locally applied
review chain to ASF git repo and mark the reviews as submitted on ASF
ReviewBoard.

Example Usage:

> git checkout master
> git pull origin
> ./support/apply-reviews.py -c -r 1234
> ./support/push-commits.py
"""

# TODO(vinod): Also post the commit message to the corresponding ASF JIRA
# tickets and resolve them if necessary.

import argparse
import os
import platform
import re
import sys
import urllib.parse

from subprocess import check_output

REVIEWBOARD_URL = 'https://reviews.apache.org'

def _check_output(args):
    return check_output(args).decode(sys.stdout.encoding)

def get_reviews(revision_range):
    """Return the list of reviews found in the commits in the revision range."""
    reviews = [] # List of (review id, commit log) tuples

    rev_list = _check_output(['git',
                              'rev-list',
                              '--reverse',
                              revision_range]).strip().split('\n')
    for rev in rev_list:
        commit_log = _check_output(['git',
                                    '--no-pager',
                                    'show',
                                    '--no-color',
                                    '--no-patch',
                                    rev]).strip()

        pos = commit_log.find('Review: ')
        if pos != -1:
            regex = 'Review: ({url})$'.format(
                url=urllib.parse.urljoin(REVIEWBOARD_URL, 'r/[0-9]+'))
            pattern = re.compile(regex)
            match = pattern.search(commit_log.strip().strip('/'))
            if match is None:
                print("\nInvalid ReviewBoard URL: '{}'".format(
                    commit_log[pos:]))
                sys.exit(1)

            url = match.group(1)
            reviews.append((os.path.basename(url), commit_log))

    return reviews


def close_reviews(reviews, options):
    """Mark the given reviews as submitted on ReviewBoard."""
    for review_id, commit_log in reviews:
        print('Closing review', review_id)
        if not options['dry_run']:
            rbt_command = 'rbt'
            # Windows command name must have `cmd` extension.
            if platform.system() == 'Windows':
                rbt_command = 'rbt.cmd'
            _check_output([rbt_command,
                           'close',
                           '--description',
                           commit_log,
                           review_id])


def parse_options():
    """Return a dictionary of options parsed from command line arguments."""
    parser = argparse.ArgumentParser()

    parser.add_argument('-n',
                        '--dry-run',
                        action='store_true',
                        help='Perform a dry run.')

    args = parser.parse_args()

    options = {}
    options['dry_run'] = args.dry_run

    return options


def main():
    """Main function to push the commits in this branch as review requests."""
    options = parse_options()

    current_branch_ref = _check_output(['git', 'symbolic-ref', 'HEAD']).strip()
    current_branch = current_branch_ref.replace('refs/heads/', '', 1)

    if current_branch != 'master':
        print('Please run this script from master branch')
        sys.exit(1)

    remote_tracking_branch = _check_output(['git',
                                            'rev-parse',
                                            '--abbrev-ref',
                                            'master@{upstream}']).strip()

    merge_base = _check_output([
        'git',
        'merge-base',
        remote_tracking_branch,
        'master']).strip()

    if merge_base == current_branch_ref:
        print('No new commits found to push')
        sys.exit(1)

    reviews = get_reviews(merge_base + ".." + current_branch_ref)

    # Push the current branch to remote master.
    remote = _check_output(['git',
                            'config',
                            '--get',
                            'branch.master.remote']).strip()

    print('Pushing commits to', remote)

    if options['dry_run']:
        _check_output(['git',
                       'push',
                       '--dry-run',
                       remote,
                       'master:master'])
    else:
        _check_output(['git',
                       'push',
                       remote,
                       'master:master'])

    # Now mark the reviews as submitted.
    close_reviews(reviews, options)

if __name__ == '__main__':
    main()
