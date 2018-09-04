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
Wrapper around the post-review/rbt tool provided by Review Board.

This script provides the ability to send one review for each
commit on the current branch, instead of squashing all changes
into a single large review. We encourage contributors to create
logical commits which can be reviewed independently.

Options to pass onto 'rbt' can be placed in a '.reviewboardrc'
file at the top of the Mesos source directory.  A default
'.reviewboardrc' can be found at 'support/reviewboardrc'.
Running './bootstrap' will populate this file for you.

To use this script, first install 'RBTools' from Review Board:
http://www.reviewboard.org/downloads/rbtools/

$ cd /path/to/mesos
$ [ do some work on your branch off of master, make commit(s) ]
$ ./support/post-reviews.py
"""
# pylint: skip-file

import argparse
import atexit
import importlib.machinery
import importlib.util
import os
import platform
import re
import sys
import urllib.parse

from distutils.version import LooseVersion

from subprocess import check_output, Popen, PIPE, STDOUT


def execute(command, ignore_errors=False):
    """Execute a process and leave."""
    process = None
    try:
        process = Popen(command,
                        stdin=PIPE,
                        stdout=PIPE,
                        stderr=STDOUT,
                        shell=False)
    except Exception:
        if not ignore_errors:
            raise
        return None

    data, _ = process.communicate()
    data = data.decode(sys.stdout.encoding)
    status = process.wait()
    if status != 0 and not ignore_errors:
        cmdline = ' '.join(command) if isinstance(command, list) else command
        need_login = 'Please log in to the Review Board' \
                     ' server at reviews.apache.org.'
        if need_login in data:
            print(need_login, '\n')
            print("You can either:")
            print("  (1) Run 'rbt login', or")
            print("  (2) Set the default USERNAME/PASSWORD in '.reviewboardrc'")
        else:
            print('Failed to execute: \'' + cmdline + '\':')
            print(data)
        sys.exit(1)
    elif status != 0:
        return None
    return data


def main():
    """Main function, post commits added to this branch as review requests."""
    # TODO(benh): Make sure this is a git repository, apologize if not.

    # Choose 'rbt' if available, otherwise choose 'post-review'.
    post_review = None

    rbt_command = 'rbt'
    # Windows command name must have `cmd` extension.
    if platform.system() == 'Windows':
        rbt_command = 'rbt.cmd'

    rbt_version = execute([rbt_command, '--version'], ignore_errors=True)
    if rbt_version:
        rbt_version = LooseVersion(rbt_version)
        post_review = [rbt_command, 'post']
    elif execute(['post-review', '--version'], ignore_errors=True):
        post_review = ['post-review']
    else:
        print('Please install RBTools before proceeding')
        sys.exit(1)

    # Warn if people have unstaged changes.
    diff_stat = execute(['git', 'diff', '--shortstat']).strip()

    if diff_stat:
        print('WARNING: Worktree contains unstaged changes, continuing anyway.', file=sys.stderr)

    # Warn if people have uncommitted changes.
    diff_stat = execute(['git', 'diff', '--shortstat', '--staged']).strip()

    if diff_stat:
        print('WARNING: Worktree contains staged but uncommitted changes, ' \
            'continuing anyway.', file=sys.stderr)

    # Grab a reference to the repo's git directory. Usually this is simply
    # .git in the repo's top level directory. However, when submodules are
    # used, it may appear elsewhere. The most up-to-date way of finding this
    # directory is to use `git rev-parse --git-common-dir`. This is necessary
    # to support things like git worktree in addition to git submodules.
    # However, as of January 2016, support for the '--git-common-dir' flag is
    # fairly new, forcing us to fall back to the '--git-dir' flag if
    # '--git-common-dir' is not supported. We do this by checking the output of
    # git rev-parse --git-common-dir` and check if it gives a valid directory.
    # If not, we set the git directory using the '--git-dir' flag instead.
    git_dir = execute(['git', 'rev-parse', '--git-common-dir']).strip()
    if not os.path.isdir(git_dir):
        git_dir = execute(['git', 'rev-parse', '--git-dir']).strip()

    # Grab a reference to the top level directory of this repo.
    top_level_dir = execute(['git', 'rev-parse', '--show-toplevel']).strip()

    # Use the tracking_branch specified by the user if exists.
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument(
        '--server',
        help='Specifies the Review Board server to use.')
    parser.add_argument(
        '--no-markdown',
        action='store_true',
        help='Specifies if the commit text should not be treated as Markdown.')
    parser.add_argument(
        '--bugs-closed',
        help='The comma-separated list of bug IDs closed.')
    parser.add_argument(
        '--target-people',
        help='The usernames of the people who should perform the review.')
    parser.add_argument(
        '--tracking-branch',
        help='The remote tracking branch from which your local branch is derived.')
    args, _ = parser.parse_known_args()

    # Try to read the .reviewboardrc in the top-level directory.
    reviewboardrc_filepath = os.path.join(top_level_dir, '.reviewboardrc')
    if os.path.exists(reviewboardrc_filepath):
        # Prevent generation of '.reviewboardrcc'.
        sys.dont_write_bytecode = True
        loader = importlib.machinery.SourceFileLoader(
            'reviewboardrc', reviewboardrc_filepath)
        spec = importlib.util.spec_from_loader(loader.name, loader)
        reviewboardrc = importlib.util.module_from_spec(spec)
        loader.exec_module(reviewboardrc)

    if args.server:
        reviewboard_url = args.server
    elif 'REVIEWBOARD_URL' in dir(reviewboardrc):
        reviewboard_url = reviewboardrc.REVIEWBOARD_URL
    else:
        reviewboard_url = 'https://reviews.apache.org'

    if args.tracking_branch:
        tracking_branch = args.tracking_branch
    elif 'TRACKING_BRANCH' in dir(reviewboardrc):
        tracking_branch = reviewboardrc.TRACKING_BRANCH
    else:
        tracking_branch = 'master'

    branch_ref = execute(['git', 'symbolic-ref', 'HEAD']).strip()
    branch = branch_ref.replace('refs/heads/', '', 1)

    # Do not work on the tracking branch.
    if branch == tracking_branch:
        print("We're expecting you to be working on another branch" \
              " from {}!".format(tracking_branch))
        sys.exit(1)

    temporary_branch = '_post-reviews_' + branch

    # Always delete the temporary branch.
    atexit.register(
        lambda: execute(['git', 'branch', '-D', temporary_branch], True))

    # Always put us back on the original branch.
    atexit.register(lambda: execute(['git', 'checkout', branch]))

    # Warn if the tracking branch is no direct ancestor of this review chain.
    if execute([
            'git', 'merge-base', '--is-ancestor', tracking_branch, branch_ref],
            ignore_errors=True) is None:
        print("WARNING: Tracking branch '%s' is no direct ancestor of HEAD." \
            " Did you forget to rebase?" % tracking_branch, file=sys.stderr)

        try:
            input("Press enter to continue or 'Ctrl-C' to abort.\n")
        except KeyboardInterrupt:
            sys.exit(0)

    merge_base = execute(
        ['git', 'merge-base', tracking_branch, branch_ref]).strip()

    output = check_output([
        'git',
        '--no-pager',
        'log',
        '--pretty=format:%Cred%H%Creset -%C'
        '(yellow)%d%Creset %s %Cgreen(%cr)%Creset',
        merge_base + '..HEAD'])

    print('Running \'%s\' across all of ...' % " ".join(post_review))
    sys.stdout.buffer.write(output)

    log = execute(['git',
                   '--no-pager',
                   'log',
                   '--no-color',
                   '--pretty=oneline',
                   '--reverse',
                   merge_base + '..HEAD']).strip()

    if len(log) <= 0:
        print("No new changes compared with master branch!")
        sys.exit(1)

    shas = []

    for line in log.split('\n'):
        sha = line.split()[0]
        shas.append(sha)

    previous = merge_base
    parent_review_request_id = None
    for i, sha in enumerate(shas):
        execute(['git', 'branch', '-D', temporary_branch], True)

        message = execute(['git',
                           '--no-pager',
                           'log',
                           '--pretty=format:%s%n%n%b',
                           previous + '..' + sha])

        review_request_id = None

        pos = message.find('Review:')
        if pos != -1:
            regex = 'Review: ({url})$'.format(
                url=urllib.parse.urljoin(reviewboard_url, 'r/[0-9]+'))
            pattern = re.compile(regex)
            match = pattern.search(message[pos:].strip().strip('/'))
            if match is None:
                print("\nInvalid ReviewBoard URL: '{}'".format(message[pos:]))
                sys.exit(1)

            url = match.group(1)
            review_request_id = url.split('/')[-1]

        # Show the commit.
        if review_request_id is None:
            output = check_output([
                'git',
                '--no-pager',
                'log',
                '--pretty=format:%Cred%H%Creset -%C(yellow)%d%Creset %s',
                previous + '..' + sha])
            print('\nCreating diff of:')
            sys.stdout.buffer.write(output)
        else:
            output = check_output([
                'git',
                '--no-pager',
                'log',
                '--pretty=format:%Cred%H%Creset -%C'
                '(yellow)%d%Creset %s %Cgreen(%cr)%Creset',
                previous + '..' + sha])
            print('\nUpdating diff of:')
            sys.stdout.buffer.write(output)

        # Show the "parent" commit(s).
        output = check_output([
            'git',
            '--no-pager',
            'log',
            '--pretty=format:%Cred%H%Creset -%C'
            '(yellow)%d%Creset %s %Cgreen(%cr)%Creset',
            tracking_branch + '..' + previous])

        if output:
            print('\n... with parent diff created from:')
            sys.stdout.buffer.write(output)

        try:
            input('\nPress enter to continue or \'Ctrl-C\' to skip.\n')
        except KeyboardInterrupt:
            i = i + 1
            previous = sha
            parent_review_request_id = review_request_id
            continue

        # Strip the review url from the commit message, so that
        # it is not included in the summary message when GUESS_FIELDS
        # is set in .reviewboardc. Update the SHA appropriately.
        if review_request_id:
            stripped_message = message[:pos]
            execute(['git', 'checkout', sha])
            execute(['git', 'commit', '--amend', '-m', stripped_message])
            sha = execute(['git', 'rev-parse', 'HEAD']).strip()
            execute(['git', 'checkout', branch])

        revision_range = previous + ':' + sha

        # Build the post-review/rbt command up
        # to the point where they are common.
        command = post_review

        if not args.no_markdown:
            command = command + ['--markdown']

        if args.bugs_closed:
            command = command + ['--bugs-closed=' + args.bugs_closed]

        if args.target_people:
            command = command + ['--target-people=' + args.target_people]

        if args.tracking_branch is None:
            command = command + ['--tracking-branch=' + tracking_branch]

        if review_request_id:
            command = command + ['--review-request-id=' + review_request_id]

        # Determine how to specify the revision range.
        if rbt_command in post_review and \
           rbt_version >= LooseVersion('RBTools 0.6'):
            # rbt >= 0.6.1 supports '--depends-on' argument.
            # Only set the "depends on" if this
            # is not  the first review in the chain.
            if rbt_version >= LooseVersion('RBTools 0.6.1') and \
               parent_review_request_id:
                command = command + ['--depends-on=' + parent_review_request_id]

            # rbt >= 0.6 revisions are passed in as args.
            command = command + sys.argv[1:] + [previous, sha]
        else:
            # post-review and rbt < 0.6 revisions are
            # passed in using the revision range option.
            command = command + \
                ['--revision-range=' + revision_range] + \
                sys.argv[1:]

        output = execute(command)

        # Output is a string, we convert it to a byte string before writing it.
        sys.stdout.buffer.write(output.encode())

        # If we already have a request_id, continue on to the next commit in the
        # chain. We update 'previous' from the shas[] array because we have
        # overwritten the temporary sha variable above.
        if review_request_id is not None:
            previous = shas[i]
            parent_review_request_id = review_request_id
            i = i + 1
            continue

        # Otherwise, get the request_id from the output of post-review, append
        # it to the commit message and rebase all other commits on top of it.
        lines = output.split('\n')

        # The last line of output in post-review is the review url.
        # The second to the last line of output in rbt is the review url.
        url = lines[len(lines) - 2] if rbt_command in post_review \
            else lines[len(lines) - 1]

        # Using rbt >= 0.6.3 on Linux prints out two URLs where the second
        # one has /diff/ at the end. We want to remove this so that a
        # subsequent call to post-reviews does not fail when looking up
        # the reviewboard entry to edit.
        url = url.replace('diff/', '')
        url = url.strip('/')
        review_request_id = os.path.basename(url)

        # Construct new commit message.
        message = message + '\n' + 'Review: ' + url + '\n'

        execute(['git', 'checkout', '-b', temporary_branch])
        execute(['git', 'reset', '--hard', sha])
        execute(['git', 'commit', '--amend', '-m', message])

        # Now rebase all remaining shas on top of this amended commit.
        j = i + 1
        old_sha = execute(
            ['git', 'rev-parse', '--verify', temporary_branch]).strip()
        previous = old_sha
        while j < len(shas):
            execute(['git', 'checkout', shas[j]])
            execute(['git', 'rebase', temporary_branch])
            # Get the sha for our detached HEAD.
            new_sha = execute([
                'git',
                '--no-pager',
                'log',
                '--pretty=format:%H', '-n', '1', 'HEAD']).strip()
            execute(['git',
                     'update-ref',
                     'refs/heads/' + temporary_branch,
                     new_sha,
                     old_sha])
            old_sha = new_sha
            shas[j] = new_sha
            j = j + 1

        # Okay, now update the actual branch to our temporary branch.
        new_sha = old_sha
        old_sha = execute(['git', 'rev-parse', '--verify', branch]).strip()
        execute(['git', 'update-ref', 'refs/heads/' + branch, new_sha, old_sha])

        i = i + 1
        parent_review_request_id = review_request_id

if __name__ == '__main__':
    main()
