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
This script is used to build and test (verify) reviews that are posted
to ReviewBoard. The script is intended for use by automated "ReviewBots"
that are run on ASF infrastructure (or by anyone that wishes to donate
some compute power). For example, see 'support/jenkins/reviewbot.sh'.

The script performs the following sequence:
* A query grabs review IDs from Reviewboard.
* In reverse order (most recent first), the script determines if the
  review needs verification (if the review has been updated or changed
  since the last run through this script).
* For each review that needs verification:
  * The review is applied (via 'support/apply-reviews.py').
  * Mesos is built and unit tests are run.
  * The result is posted to ReviewBoard.
"""

import argparse
import atexit
import json
import os
import platform
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

from datetime import datetime

from common import REVIEWBOARD_URL

REVIEW_SIZE = 100000  # 100 KB in bytes.

# Parse arguments.
parser = argparse.ArgumentParser(
    description="Reviews that need verification from the Review Board")
parser.add_argument(
    "-u", "--user", type=str, required=True, help="Review Board user name")
parser.add_argument(
    "-p",
    "--password",
    type=str,
    required=True,
    help="Review Board user password")
parser.add_argument(
    "-r",
    "--reviews",
    type=int,
    required=False,
    default=-1,
    help="The number of reviews to fetch,"
    " that will need verification")

# Unless otherwise specified consider pending review requests to Mesos updated
# since 03/01/2014. Note that we need to explicitly set `max-results` query
# parameter because the default is 25 (max allowed is 200).
# TODO(vinod): Paginate through the API to get all pending review requests
# instead of just getting the first 200.
group = "mesos"
last_updated = "2014-03-01T00:00:00"
query_parameters = \
    "?to-groups=%s&status=pending&last-updated-from=%s&max-results=200" \
    % (group, last_updated)
parser.add_argument(
    "-q",
    "--query",
    type=str,
    required=False,
    help="Query parameters",
    default=query_parameters)

parser.add_argument(
    "-o",
    "--out-file",
    type=str,
    required=False,
    help="The out file with the reviews IDs that"
    " need verification")
parser.add_argument(
    "--skip-verify",
    action='store_true',
    required=False,
    help="Skip the verification and just write the review"
    " ids that need verification")

parameters = parser.parse_args()
USER = parameters.user
PASSWORD = parameters.password
NUM_REVIEWS = parameters.reviews
QUERY_PARAMS = parameters.query
OUT_FILE = parameters.out_file
SKIP_VERIFY = parameters.skip_verify


class ReviewError(Exception):
    """Exception returned by post_review()."""


def parse_time(timestamp):
    """Parse time in ReviewBoard date format."""
    return datetime.strptime(timestamp, "%Y-%m-%dT%H:%M:%SZ")


def shell(command, working_dir=None):
    """Run a shell command."""
    out = subprocess.check_output(
        command, stderr=subprocess.STDOUT, cwd=working_dir, shell=True)
    return out.decode(sys.stdout.encoding)


SCRIPT_PATH = os.path.dirname(os.path.realpath(__file__))
HEAD = shell("git rev-parse HEAD", SCRIPT_PATH)


def api(url, data=None):
    """Call the ReviewBoard API."""
    try:
        auth_handler = urllib.request.HTTPBasicAuthHandler()
        auth_handler.add_password(
            realm="Web API",
            uri="reviews.apache.org",
            user=USER,
            passwd=PASSWORD)

        opener = urllib.request.build_opener(auth_handler)
        urllib.request.install_opener(opener)

        if isinstance(data, str):
            data = str.encode(data)
        f = urllib.request.urlopen(url, data=data)
        return json.loads(f.read().decode("utf-8"))
    except urllib.error.HTTPError as err:
        print("Error handling URL %s: %s (%s)" % (url, err.reason, err.read()))
        exit(1)
    except urllib.error.URLError as err:
        print("Error handling URL %s: %s" % (url, err.reason))
        exit(1)


def apply_review(review_id):
    """Apply a review using the script apply-reviews.py."""
    print("Applying review %s" % review_id)
    shell(
        "cd .. && %s support/apply-reviews.py -n -r %s" %
        (sys.executable, review_id), SCRIPT_PATH)


def apply_reviews(review_request, reviews):
    """Apply multiple reviews at once."""
    # If there are no reviewers specified throw an error.
    if not review_request["target_people"]:
        raise ReviewError("No reviewers specified. Please find a reviewer by"
                          " asking on JIRA or the mailing list.")

    # If there is a circular dependency throw an error.`
    if review_request["id"] in reviews:
        raise ReviewError(
            "Circular dependency detected for review %s."
            "Please fix the 'depends_on' field." % review_request["id"])

    reviews.append(review_request["id"])

    # First recursively apply the dependent reviews.
    for review in review_request["depends_on"]:
        review_url = review["href"]
        print("Dependent review: %s " % review_url)
        apply_reviews(api(review_url)["review_request"], reviews)

    # Now apply this review if not yet submitted.
    if review_request["status"] != "submitted":
        # If the patch does not apply we translate the error to a
        # `ReviewError`; otherwise we let the original exception
        # propagate.
        try:
            apply_review(review_request["id"])
        except subprocess.CalledProcessError as err:
            error = err.output.decode("utf-8")
            if "patch does not apply" in error:
                raise ReviewError(error)

            raise err


def post_review(review_request, message):
    """Post a review on the review board."""
    print("Posting review on %s :\n%s" \
          % (review_request["absolute_url"], message))

    review_url = review_request["links"]["reviews"]["href"]
    data = urllib.parse.urlencode({"body_top": message, "public": "true"})
    api(review_url, data)


@atexit.register
def cleanup():
    """Clean the git repository."""
    try:
        shell("git clean -fd", SCRIPT_PATH)

        print(HEAD)
        shell("git reset --hard %s" % HEAD, SCRIPT_PATH)
    except subprocess.CalledProcessError as err:
        print("Failed command: %s\n\nError: %s" % (err.cmd, err.output))


def verify_review(review_request):
    """Verify a review."""
    print("Verifying review %s" % review_request["id"])
    build_output = "build_" + str(review_request["id"])

    try:
        # Recursively apply the review and its dependents.
        reviews = []
        apply_reviews(review_request, reviews)

        reviews.reverse()  # Reviews are applied in the reverse order.

        command = ""
        if platform.system() == 'Windows':
            command = "support\\windows-build.bat"

            # There is no equivalent to `tee` on Windows.
            subprocess.check_call(
                ['cmd', '/c',
                 '%s 2>&1 > %s' % (command, build_output)])
        else:
            # Launch docker build script.
            configuration = ("export "
                             "OS='ubuntu:16.04' "
                             "BUILDTOOL='autotools' "
                             "COMPILER='gcc' "
                             "CONFIGURATION='--verbose "
                             "--disable-libtool-wrappers "
                             "--disable-parallel-test-execution' "
                             "ENVIRONMENT='GLOG_v=1 MESOS_VERBOSE=1'")

            command = "%s; ./support/jenkins/buildbot.sh" % configuration

            # `tee` the output so that the console can log the whole build
            # output. `pipefail` ensures that the exit status of the build
            # command ispreserved even after tee'ing.
            subprocess.check_call([
                'bash', '-c',
                ('set -o pipefail; %s 2>&1 | tee %s') % (command, build_output)
            ])

        # Success!
        post_review(
            review_request, "Patch looks great!\n\n"
            "Reviews applied: %s\n\n"
            "Passed command: %s" % (reviews, command))
    except subprocess.CalledProcessError as err:
        # If we are here because the docker build command failed, read the
        # output from `build_output` file. For all other command failures read
        # the output from `e.output`.
        if os.path.exists(build_output):
            output = open(build_output).read()
        else:
            output = err.output.decode(sys.stdout.encoding)

        if platform.system() == 'Windows':
            # We didn't output anything during the build (because `tee`
            # doesn't exist), so we print the output to stdout upon error.
            sys.stdout.buffer.write(output.encode())

        # Truncate the output when posting the review as it can be very large.
        if len(output) > REVIEW_SIZE:
            output = "...<truncated>...\n" + output[-REVIEW_SIZE:]

        if os.environ.get("BUILD_URL"):
            output += "\nFull log: "
            output += urllib.parse.urljoin(os.environ["BUILD_URL"], "console")

        post_review(
            review_request, "Bad patch!\n\n"
            "Reviews applied: %s\n\n"
            "Failed command: %s\n\n"
            "Error:\n%s" % (reviews, err.cmd, output))
    except ReviewError as err:
        post_review(
            review_request, "Bad review!\n\n"
            "Reviews applied: %s\n\n"
            "Error:\n%s" % (reviews, err.args[0]))

    # Clean up.
    cleanup()


def review_updated(review_request, review_time):
    """Returns whether this review request chain was updated after review
       time."""
    # If the latest diff on this review request was uploaded after the last
    # review from `USER`, we need to verify it.
    diffs_url = review_request["links"]["diffs"]["href"]
    diffs = api(diffs_url)
    diff_time = None
    if "diffs" in diffs:
        # Get the timestamp of the latest diff.
        timestamp = diffs["diffs"][-1]["timestamp"]
        diff_time = parse_time(timestamp)
        print("Latest diff timestamp: %s" % diff_time)

    if diff_time and review_time < diff_time:
        return True

    # If the latest dependency change on this review request happened after the
    # last review from `USER`, we need to verify it.
    changes_url = review_request["links"]["changes"]["href"]
    changes = api(changes_url)
    dependency_time = None
    for change in changes["changes"]:
        if "depends_on" in change["fields_changed"]:
            timestamp = change["timestamp"]
            dependency_time = parse_time(timestamp)
            print("Latest dependency change timestamp: %s" % dependency_time)
            break

    if dependency_time and review_time < dependency_time:
        return True

    # Recursively check if any of the dependent review requests need
    # verification.
    for review in review_request["depends_on"]:
        review_url = review["href"]
        print("Dependent review: %s " % review_url)
        if review_updated(api(review_url)["review_request"], review_time):
            return True

    return False


def needs_verification(review_request):
    """Returns whether this review request chain needs to be verified."""
    print("Checking if review: %s needs verification" % review_request["id"])

    # Skip if the review blocks another review.
    if review_request["blocks"]:
        print("Skipping blocking review %s" % review_request["id"])
        return False

    # Get the timestamp of the latest review from `USER`.
    reviews_url = review_request["links"]["reviews"]["href"]
    reviews = api(reviews_url + "?max-results=200")
    review_time = None
    for review in reversed(reviews["reviews"]):
        if review["links"]["user"]["title"] == USER:
            timestamp = review["timestamp"]
            review_time = parse_time(timestamp)
            print("Latest review timestamp: %s" % review_time)
            break

    # Verify the review request if no reviews were found from `USER`.
    if review_time is None:
        return True

    # Recursively check if any review requests in the chain were updated.
    return review_updated(review_request, review_time)


def write_review_ids(review_ids):
    """Write the IDs of the review requests that need verification."""
    print("%s review requests need verification" % len(review_ids))
    if OUT_FILE is not None:
        with open(OUT_FILE, 'w') as f:
            f.write('\n'.join(review_ids))
    else:
        print('\n'.join(review_ids))


def main():
    """Main function to verify the submitted reviews."""
    review_requests_url = \
        "%s/api/review-requests/%s" % (REVIEWBOARD_URL, QUERY_PARAMS)

    review_requests = api(review_requests_url)
    review_ids = []
    for review_request in reversed(review_requests["review_requests"]):
        if (NUM_REVIEWS == -1 or len(review_ids) < NUM_REVIEWS) and \
           needs_verification(review_request):
            if not SKIP_VERIFY:
                verify_review(review_request)
            review_ids.append(str(review_request["id"]))

    write_review_ids(review_ids)


if __name__ == '__main__':
    main()
