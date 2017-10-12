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
import re
import sys

from subprocess import check_output

REVIEWBOARD_URL = 'https://reviews.apache.org'


def get_reviews(revision_range):
    """Return the list of reviews found in the commits in the revision range."""
    log = check_output(['git',
                        '--no-pager',
                        'log',
                        '--no-color',
                        '--reverse',
                        revision_range]).strip()

    review_ids = []
    for line in log.split('\n'):
        pos = line.find('Review: ')
        if pos != -1:
            pattern = re.compile('Review: ({url})$'.format(
                url=os.path.join(REVIEWBOARD_URL, 'r', '[0-9]+')))
            match = pattern.search(line.strip().strip('/'))
            if match is None:
                print "\nInvalid ReviewBoard URL: '{}'".format(line[pos:])
                sys.exit(1)

            url = match.group(1)
            review_ids.append(os.path.basename(url))

    return review_ids


def close_reviews(reviews, options):
    """Mark the given reviews as submitted on ReviewBoard."""
    for review_id in reviews:
        print 'Closing review', review_id
        if not options['dry_run']:
            # TODO(vinod): Include the commit message as '--description'.
            check_output(['rbt', 'close', review_id])


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

    current_branch_ref = check_output(['git', 'symbolic-ref', 'HEAD']).strip()
    current_branch = current_branch_ref.replace('refs/heads/', '', 1)

    if current_branch != 'master':
        print 'Please run this script from master branch'
        sys.exit(1)

    remote_tracking_branch = check_output(['git',
                                           'rev-parse',
                                           '--abbrev-ref',
                                           'master@{upstream}']).strip()

    merge_base = check_output([
        'git',
        'merge-base',
        remote_tracking_branch,
        'master']).strip()

    if merge_base == current_branch_ref:
        print 'No new commits found to push'
        sys.exit(1)

    reviews = get_reviews(merge_base + ".." + current_branch_ref)

    print 'Found reviews', reviews

    # Push the current branch to remote master.
    remote = check_output(['git',
                           'config',
                           '--get',
                           'branch.master.remote']).strip()

    print 'Pushing commits to', remote

    if options['dry_run']:
        check_output(['git',
                      'push',
                      '--dry-run',
                      remote,
                      'master:master'])
    else:
        check_output(['git',
                      'push',
                      remote,
                      'master:master'])

    # Now mark the reviews as submitted.
    close_reviews(reviews, options)

if __name__ == '__main__':
    main()
