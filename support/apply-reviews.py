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
This script translates reviews on ReviewBoard into commits
on the current branch.
"""

import argparse
import atexit
import json
import linecache
import os
import pipes
import platform
import re
import ssl
import subprocess
import sys
import urllib.request
import urllib.error
import urllib.parse


REVIEWBOARD_REVIEW_URL = 'https://reviews.apache.org/r'
REVIEWBOARD_API_URL =\
  'https://reviews.apache.org/api/review-requests'
REVIEWBOARD_USER_URL = 'https://reviews.apache.org/api/users'


GITHUB_URL = 'https://api.github.com/repos/apache/mesos/pulls'
GITHUB_PATCH_URL =\
  'https://patch-diff.githubusercontent.com/raw/apache/mesos/pull'


def review_api_url(review_id):
    """Returns a Review Board API URL given a review ID."""
    # Reviewboard REST API expects '/' at the end of the URL.
    return '{base}/{review}/'.format(
        base=REVIEWBOARD_API_URL,
        review=review_id)


def review_url(review_id):
    """Returns a Review Board UI URL given a review ID."""
    return '{base}/{review}/'.format(
        base=REVIEWBOARD_REVIEW_URL,
        review=review_id)


def pull_request_url(pull_request_number):
    """Returns a GitHub pull request URL given a PR number."""
    return '{base}/{pr}'.format(
        base=GITHUB_URL,
        pr=pull_request_number)


def reviewboard_user_url(username):
    """Returns a Review Board URL for a user given a username."""
    # Reviewboard REST API expects '/' at the end of the URL.
    return '{base}/{user}/'.format(
        base=REVIEWBOARD_USER_URL,
        user=username)


def patch_url(options):
    """Returns a Review Board or a GitHub URL for a patch."""
    if options['review_id']:
        # Reviewboard REST API expects '/' at the end of the URL.
        return '{base}/{review}/diff/raw/'.format(
            base=REVIEWBOARD_REVIEW_URL,
            review=options['review_id'])
    if options['github']:
        return '{base}/{patch}.patch'.format(
            base=GITHUB_PATCH_URL,
            patch=options['github'])
    return None


def url_to_json(url):
    """Performs HTTP request and returns JSON-ified response."""
    json_str = urllib.request.urlopen(url).read().decode('utf-8')
    return json.loads(json_str)

def extract_review_id(url):
    """Extracts review ID from Review Board URL."""
    review_id = re.search(REVIEWBOARD_API_URL + r'/(\d+)/', url)
    if review_id:
        return review_id.group(1)
    return ''

def review_chain(review_id):
    """Returns a parent review chain for a given review ID."""
    json_obj = url_to_json(review_api_url(review_id))

    # Stop as soon as we stumble upon a submitted request.
    status = json_obj.get('review_request').get('status')
    if status == "submitted":
        sys.stderr.write('Warning: Review {review} has already'
                         ' been submitted and did not get applied'
                         ' to you current work-tree\n'.format(review=review_id))
        return []

    # Verify that the review has exactly one parent.
    parent = json_obj.get('review_request').get('depends_on')
    if len(parent) > 1:
        sys.stderr.write('Error: Review {review} has more than'
                         ' one parent'.format(review=review_id))
        sys.exit(1)
    elif not parent:
        return [(review_id, json_obj.get('review_request').get('summary'))]
    else:
        # The review has exactly one parent.
        review_list = review_chain(extract_review_id(parent[0].get('href')))

        review = (review_id, json_obj.get('review_request').get('summary'))
        if review not in review_list:
            return review_list + [review]
        sys.stderr.write('Found a circular dependency in the chain starting'
                         ' at {review}\n'.format(review=review_id))
        sys.exit(1)


def shell(command, dry_run):
    """
    Runs a command in a shell, unless the dry-run option
    is set (in which case it just prints the command).
    """
    if dry_run:
        print(command)
        return

    error_code = subprocess.call(command, stderr=subprocess.STDOUT, shell=True)
    if error_code != 0:
        sys.exit(error_code)


def apply_review(options):
    """Applies a review with a given ID locally."""
    # Make sure we don't leave the patch behind in case of failure.
    # We store the patch ID in a local variable to ensure the lambda
    # captures the current patch ID.
    patch_file = '%s.patch' % patch_id(options)
    if not options["keep_patches"]:
        atexit.register(
            lambda: os.path.exists(patch_file) and os.remove(patch_file))

    fetch_patch(options)
    apply_patch(options)
    commit_patch(options)


def ssl_create_default_context():
    """
    Equivalent to `ssl.create_default_context` with default arguments and
    certificate/hostname verification disabled.
    See: https://github.com/python/cpython/blob/2.7/Lib/ssl.py#L410
    This function requires Python >= 2.7.9.
    """
    # pylint: disable=no-member
    context = ssl.SSLContext(ssl.PROTOCOL_SSLv23)

    # SSLv2 considered harmful.
    context.options |= ssl.OP_NO_SSLv2

    # SSLv3 has problematic security and is only required for really old
    # clients such as IE6 on Windows XP.
    context.options |= ssl.OP_NO_SSLv3

    # Disable compression to prevent CRIME attacks (OpenSSL 1.0+).
    context.options |= getattr(ssl, "OP_NO_COMPRESSION", 0)

    # Disable certificate and hostname verification.
    context.verify_mode = ssl.CERT_NONE
    context.check_hostname = False


def fetch_patch(options):
    """Fetches a patch from Review Board or GitHub."""
    # pylint: disable=unexpected-keyword-arg
    if platform.system() == 'Windows':
        # This call requires Python >= 2.7.9.
        response = urllib.request.urlopen(
            patch_url(options),
            context=ssl_create_default_context())

        with open('%s.patch' % patch_id(options), 'wb') as patch:
            patch.write(response.read())
    else:
        # NOTE: SSL contexts are only supported in Python 2.7.9+. The version
        # of Python running on the non-Windows ASF CI machines is sometimes
        # older. Hence, we fall back to `wget` on non-Windows machines.
        cmd = ' '.join([
            'wget',
            '--no-check-certificate',
            '--no-verbose',
            '-O '
            '{review_id}.patch',
            '{url}']).format(
                review_id=patch_id(options),
                url=patch_url(options))

        # In case of GitHub we always need to fetch the patch to extract
        # username and email, so we ignore the dry_run option by setting the
        # second parameter to False.
        if options['github']:
            shell(cmd, False)
        else:
            shell(cmd, options['dry_run'])


def patch_id(options):
    """Returns the review ID or the GitHub pull request number."""
    return options['review_id'] or options['github']


def apply_patch(options):
    """Applies patch locally."""
    cmd = 'git apply --index {review_id}.patch'.format(
        review_id=patch_id(options))

    if options['3way']:
        cmd += ' --3way'

    if platform.system() == 'Windows':
        # NOTE: Depending on the Git settings, there may or may not be
        # carriage returns in files and in the downloaded patch.
        # We ignore these errors on Windows.
        cmd += ' --ignore-whitespace'

    shell(cmd, options['dry_run'])


def quote(string):
    """Quote a variable so it can be safely used in shell."""
    return string.replace("'", "'\\''")


def commit_patch(options):
    """Commits patch locally."""
    data = patch_data(options)

    # Check whether we need to amend the commit message.
    if options['no_amend']:
        amend = ''
    else:
        amend = '-e'

    # Check whether we should skip the commit hooks.
    if options['skip_hooks']:
        verify = '-n'
    else:
        verify = ''

    # NOTE: Windows does not support multi-line commit messages via the shell.
    message_file = '%s.message' % patch_id(options)
    atexit.register(
        lambda: os.path.exists(message_file) and os.remove(message_file))

    with open(message_file, 'w') as message:
        # Add a shell command creating the message file for dry-run mode.
        if options["dry_run"]:
            shell(
                "printf {msg} > {file}".format(
                    msg=pipes.quote(data['message']).replace('\n', '\\n'),
                    file=message_file),
                True)
        message.write(data['message'])

    cmd = 'git commit' \
          ' --author \"{author}\"' \
          ' {amend} -aF \"{message}\"' \
          ' {verify}'.format(
              author=quote(data['author']),
              amend=amend,
              message=message_file,
              verify=verify)

    shell(cmd, options['dry_run'])


def patch_data(options):
    """
    Populates and returns a dictionary with data necessary for
    committing the patch (such as the message, the author, etc.).
    """
    if options['review_id']:
        return reviewboard_data(options)
    if options['github']:
        return github_data(options)
    return None


def get_author(patch):
    """Reads the author name and email from the .patch file"""
    author = linecache.getline(patch, 2)
    return author.replace('From: ', '').rstrip()


def github_data(options):
    """Fetches pull request data and populates internal data structure."""
    pull_request_number = options['github']
    pull_request = url_to_json(pull_request_url(pull_request_number))

    title = pull_request.get('title')
    description = pull_request.get('body')
    url = '{url}/{pr}'.format(url=GITHUB_URL, pr=pull_request_number)
    author = get_author('{pr}.patch'.format(pr=pull_request_number))
    message = '\n\n'.join([
        title,
        description,
        'This closes #{pr}'.format(pr=pull_request_number)])

    review_data = {
        "summary": title,
        "description": description,
        "url": url,
        "author": author,
        "message": message
    }

    return review_data


def reviewboard_data(options):
    """Fetches review data and populates internal data structure."""
    review_id = options['review_id']

    # Populate review object.
    review = url_to_json(review_api_url(review_id)).get('review_request')

    url = review_url(review_id)

    # Populate user object.
    user = url_to_json(reviewboard_user_url(
        review.get('links').get('submitter').get('title'))).get('user')

    # Only include a description if it is not identical to the summary.
    message_data = [review.get('summary')]
    if review.get('description') != review.get('summary'):
        message_data.append(review.get('description'))
    message_data.append('Review: {review_url}'.format(review_url=url))

    author = '{author} <{email}>'.format(
        author=user.get('fullname'),
        email=user.get('email'))
    message = '\n\n'.join(message_data)

    review_data = {
        "summary": review.get('summary'),
        "description": review.get('description'),
        "url": url,
        "author": author,
        "message": message
    }

    return review_data


def parse_options():
    """Parses command line options and returns an option dictionary."""
    options = {}

    parser = argparse.ArgumentParser(
        description='Recursively apply Review Board reviews'
                    ' and GitHub pull requests.')

    parser.add_argument('-d', '--dry-run',
                        action='store_true',
                        help='Perform a dry run.')
    parser.add_argument('-k', '--keep-patches',
                        action='store_true',
                        help="Do not delete downloaded patch files.")
    parser.add_argument('-n', '--no-amend',
                        action='store_true',
                        help='Do not amend commit message.')
    parser.add_argument('-c', '--chain',
                        action='store_true',
                        help='Recursively apply parent review chain.')
    parser.add_argument('-s', '--skip-hooks',
                        action='store_true',
                        help='Skip the commit hooks (e.g., Mesos style check).')
    parser.add_argument('-3', '--3way',
                        dest='three_way',
                        action='store_true',
                        help='Use 3 way merge in git apply.')

    # Add -g and -r and make them mutually exclusive.
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('-g', '--github',
                       metavar='PULL_REQUEST',
                       help='Pull request number')
    group.add_argument('-r', '--review-id',
                       metavar='REVIEW_ID',
                       help='Numeric review ID')

    args = parser.parse_args()

    options['review_id'] = args.review_id
    options['dry_run'] = args.dry_run
    options['keep_patches'] = args.keep_patches
    options['no_amend'] = args.no_amend
    options['github'] = args.github
    options['chain'] = args.chain
    options['skip_hooks'] = args.skip_hooks
    options['3way'] = args.three_way

    return options


def reviewboard(options):
    """Applies either a chain of reviewboard patches or a single patch."""
    if options['chain']:
        # Retrieve the list of reviews to apply.
        applied = set()
        for review_id, _ in review_chain(options['review_id']):
            if review_id not in applied:
                applied.add(review_id)
                options['review_id'] = review_id
                apply_review(options)
    else:
        apply_review(options)


def main():
    """
    Main function to apply reviews.
    """
    options = parse_options()

    if options['review_id']:
        reviewboard(options)
    else:
        apply_review(options)

if __name__ == "__main__":
    main()
