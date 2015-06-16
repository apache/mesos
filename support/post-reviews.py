#!/usr/bin/env python
# This is a wrapper around the 'post-review'/'rbt' tool provided by
# Review Board. This is currently used by Apache Mesos development.
#
# What does this do?
# It provides the ability to send a review for each commit on the
# current branch.
#
# Why is that useful?
# No one likes a 5000 line review request. Using this tool forces one
# to create logical commits which can be reviewed independently.
#
# How do I use it?
# First install 'RBTools' from Review Board.
# http://www.reviewboard.org/downloads/rbtools/
#
# $ cd /path/to/mesos
# $ [ do some work on your branch off of master, make commit(s) ]
# $ ./support/post-reviews.py --server=https://reviews.apache.org \
#   --tracking-branch=origin/master --target-groups=mesos --open
#
# NOTE: post-reviews is currently specific to Mesos development,
# but can easily be adapted for other projects.

import argparse
import atexit
import os
import sys

from distutils.version import LooseVersion

from subprocess import call, Popen, PIPE, STDOUT

def execute(command, ignore_errors=False):
    process = None
    try:
        process = Popen(command,
                stdin=PIPE,
                stdout=PIPE,
                stderr=STDOUT,
                shell=False)
    except:
        if not ignore_errors:
            raise
        return None

    data = process.stdout.read()
    status = process.wait()
    if status != 0 and not ignore_errors:
        cmdline = ' '.join(command) if isinstance(command, list) else command
        print 'Failed to execute: \'' + cmdline + '\':'
        print data
        sys.exit(1)
    elif status != 0:
        return None
    return data


# TODO(benh): Make sure this is a git repository, apologize if not.

# Choose 'post-review' if available, otherwise choose 'rbt'.
post_review = None
rbt_version = execute(['rbt', '--version'], ignore_errors=True)
if rbt_version:
  rbt_version = LooseVersion(rbt_version)
  post_review = ['rbt', 'post']
elif execute(['post-review', '--version'], ignore_errors=True):
  post_review = ['post-review']
else:
  print 'Please install RBTools before proceeding'
  sys.exit(1)

# Don't do anything if people have unstaged changes.
diff_stat = execute(['git', 'diff', '--shortstat']).strip()

if diff_stat:
  print 'Please commit or stash any changes before using post-reviews!'
  sys.exit(1)

# Don't do anything if people have uncommitted changes.
diff_stat = execute(['git', 'diff', '--shortstat', '--staged']).strip()

if diff_stat:
  print 'Please commit staged changes before using post-reviews!'
  sys.exit(1)

top_level_dir = execute(['git', 'rev-parse', '--show-toplevel']).strip()

# Use the tracking_branch specified by the user if exists.
# TODO(jieyu): Parse .reviewboardrc as well.
parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--tracking-branch')
args, _ = parser.parse_known_args()

tracking_branch = args.tracking_branch if args.tracking_branch else 'master'

branch_ref = execute(['git', 'symbolic-ref', 'HEAD']).strip()
branch = branch_ref.replace('refs/heads/', '', 1)

# do not work on master branch
if branch == "master":
    print "We're expecting you to be working on another branch from master!"
    sys.exit(1)

temporary_branch = '_post-reviews_' + branch

# Always delete the temporary branch.
atexit.register(lambda: execute(['git', 'branch', '-D', temporary_branch], True))

# Always put us back on the original branch.
atexit.register(lambda: execute(['git', 'checkout', branch]))

merge_base = execute(['git', 'merge-base', tracking_branch, branch_ref]).strip()


print 'Running \'%s\' across all of ...' % " ".join(post_review)

call(['git',
      '--no-pager',
      'log',
      '--pretty=format:%Cred%H%Creset -%C(yellow)%d%Creset %s %Cgreen(%cr)%Creset',
      merge_base + '..HEAD'])

log = execute(['git',
               '--no-pager',
               'log',
               '--no-color',
               '--pretty=oneline',
               '--reverse',
               merge_base + '..HEAD']).strip()

if len(log) <= 0:
    print "No new changes compared with master branch!"
    sys.exit(1)

shas = []

for line in log.split('\n'):
    sha = line.split()[0]
    shas.append(sha)


previous = tracking_branch
parent_review_request_id = None
for i in range(len(shas)):
    sha = shas[i]

    execute(['git', 'branch', '-D', temporary_branch], True)

    message = execute(['git',
                       '--no-pager',
                       'log',
                       '--pretty=format:%s%n%n%b',
                       previous + '..' + sha])

    review_request_id = None

    if message.find('Review: ') != -1:
        url = message[(message.index('Review: ') + len('Review: ')):].strip()
        # TODO(benh): Handle bad (or not Review Board) URLs.
        review_request_id = os.path.basename(url.strip('/'))

    # Show the commit.
    if review_request_id is None:
        print '\n\nCreating diff of:'
        call(['git',
              '--no-pager',
              'log',
              '--pretty=format:%Cred%H%Creset -%C(yellow)%d%Creset %s',
              previous + '..' + sha])
    else:
        print '\n\nUpdating diff of:'
        call(['git',
              '--no-pager',
              'log',
              '--pretty=format:%Cred%H%Creset -%C(yellow)%d%Creset %s %Cgreen(%cr)%Creset',
              previous + '..' + sha])

    # Show the "parent" commit(s).
    print '\n\n... with parent diff created from:'
    call(['git',
          '--no-pager',
          'log',
          '--pretty=format:%Cred%H%Creset -%C(yellow)%d%Creset %s %Cgreen(%cr)%Creset',
          tracking_branch + '..' + previous])

    try:
        raw_input('\nPress enter to continue or \'Ctrl-C\' to skip.\n')
    except KeyboardInterrupt:
        i = i + 1
        previous = sha
        parent_review_request_id = review_request_id
        continue

    revision_range = previous + ':' + sha

    # Build the post-review/rbt command up to the point where they are common.
    command = post_review

    if args.tracking_branch is None:
        command = command + ['--tracking-branch=' + tracking_branch]

    if review_request_id:
        command = command + ['--review-request-id=' + review_request_id]

    # Determine how to specify the revision range.
    if 'rbt' in post_review and rbt_version >= LooseVersion('RBTools 0.6'):
       # rbt >= 0.6.1 supports '--depends-on' argument.
       # Only set the "depends on" if this is not the first review in the chain.
       if rbt_version >= LooseVersion('RBTools 0.6.1') and parent_review_request_id:
         command = command + ['--depends-on=' + parent_review_request_id]

       # rbt >= 0.6 revisions are passed in as args.
       command = command + sys.argv[1:] + [previous, sha]
    else:
        # post-review and rbt < 0.6 revisions are passed in using the revision
        # range option.
        command = command + \
            ['--revision-range=' + revision_range] + \
            sys.argv[1:]

    output = execute(command).strip()

    print output


    if review_request_id is not None:
        i = i + 1
        previous = sha
        parent_review_request_id = review_request_id
        continue

    lines = output.split('\n')

    # The last line of output in post-review is the review url.
    # The second to the last line of output in rbt is the review url.
    url = lines[len(lines) - 2] if 'rbt' in post_review \
        else lines[len(lines) - 1]

    # Using rbt >= 0.6.3 on Linux prints out two URLs where the second
    # one has /diff/ at the end. We want to remove this so that a
    # subsequent call to post-reviews does not fail when looking up
    # the reviewboard entry to edit.
    url = url.replace('diff/','')
    url = url.strip('/')
    review_request_id = os.path.basename(url)

    # Construct new commit message.
    message = message + '\n' + 'Review: ' + url + '\n'

    execute(['git', 'checkout', '-b', temporary_branch])
    execute(['git', 'reset', '--hard', sha])
    execute(['git', 'commit', '--amend', '-m', message])

    # Now rebase all remaining shas on top of this amended commit.
    j = i + 1
    old_sha = execute(['cat', os.path.join(top_level_dir, '.git/refs/heads', temporary_branch)]).strip()
    previous = old_sha
    while j < len(shas):
        execute(['git', 'checkout', shas[j]])
        execute(['git', 'rebase', temporary_branch])
        # Get the sha for our detached HEAD.
        new_sha = execute(['git', '--no-pager', 'log', '--pretty=format:%H', '-n', '1', 'HEAD']).strip()
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
    old_sha = execute(['cat', os.path.join(top_level_dir, '.git/refs/heads', branch)]).strip()
    execute(['git', 'update-ref', 'refs/heads/' + branch, new_sha, old_sha])

    i = i + 1
    parent_review_request_id = review_request_id
