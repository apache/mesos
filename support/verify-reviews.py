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

import atexit
import json
import os
import platform
import subprocess
import sys
import urllib
import urllib2
import urlparse

from datetime import datetime

REVIEWBOARD_URL = "https://reviews.apache.org"
REVIEW_SIZE = 1000000  # 1 MB in bytes.

# TODO(vinod): Use 'argparse' module.
# Get the user and password from command line.
if len(sys.argv) < 3:
    print("Usage: ./verify-reviews.py <user>"
          "<password> [num-reviews] [query-params]")
    sys.exit(1)

USER = sys.argv[1]
PASSWORD = sys.argv[2]

# Number of reviews to verify.
NUM_REVIEWS = -1  # All possible reviews.
if len(sys.argv) >= 4:
    NUM_REVIEWS = int(sys.argv[3])

# Unless otherwise specified consider pending review requests to Mesos updated
# since 03/01/2014.
GROUP = "mesos"
LAST_UPDATED = "2014-03-01T00:00:00"
QUERY_PARAMS = "?to-groups=%s&status=pending&last-updated-from=%s" \
    % (GROUP, LAST_UPDATED)
if len(sys.argv) >= 5:
    QUERY_PARAMS = sys.argv[4]


class ReviewError(Exception):
    """Exception returned by post_review()."""
    pass


def shell(command):
    """Run a shell command."""
    print command
    return subprocess.check_output(
        command, stderr=subprocess.STDOUT, shell=True)


HEAD = shell("git rev-parse HEAD")


def api(url, data=None):
    """Call the ReviewBoard API."""
    try:
        auth_handler = urllib2.HTTPBasicAuthHandler()
        auth_handler.add_password(
            realm="Web API",
            uri="reviews.apache.org",
            user=USER,
            passwd=PASSWORD)

        opener = urllib2.build_opener(auth_handler)
        urllib2.install_opener(opener)

        return json.loads(urllib2.urlopen(url, data=data).read())
    except urllib2.HTTPError as err:
        print "Error handling URL %s: %s (%s)" % (url, err.reason, err.read())
        exit(1)
    except urllib2.URLError as err:
        print "Error handling URL %s: %s" % (url, err.reason)
        exit(1)


def apply_review(review_id):
    """Apply a review using the script apply-reviews.py."""
    print "Applying review %s" % review_id
    shell("python support/apply-reviews.py -n -r %s" % review_id)


def apply_reviews(review_request, reviews):
    """Apply multiple reviews at once."""
    # If there are no reviewers specified throw an error.
    if not review_request["target_people"]:
        raise ReviewError("No reviewers specified. Please find a reviewer by"
                          " asking on JIRA or the mailing list.")

    # If there is a circular dependency throw an error.`
    if review_request["id"] in reviews:
        raise ReviewError("Circular dependency detected for review %s."
                          "Please fix the 'depends_on' field."
                          % review_request["id"])
    else:
        reviews.append(review_request["id"])

    # First recursively apply the dependent reviews.
    for review in review_request["depends_on"]:
        review_url = review["href"]
        print "Dependent review: %s " % review_url
        apply_reviews(api(review_url)["review_request"], reviews)

    # Now apply this review if not yet submitted.
    if review_request["status"] != "submitted":
        apply_review(review_request["id"])


def post_review(review_request, message):
    """Post a review on the review board."""
    print "Posting review: %s" % message

    review_url = review_request["links"]["reviews"]["href"]
    data = urllib.urlencode({'body_top': message, 'public': 'true'})
    api(review_url, data)


@atexit.register
def cleanup():
    """Clean the git repository."""
    try:
        shell("git clean -fd")
        shell("git reset --hard %s" % HEAD)
    except subprocess.CalledProcessError as err:
        print "Failed command: %s\n\nError: %s" % (err.cmd, err.output)


def verify_review(review_request):
    """Verify a review."""
    print "Verifying review %s" % review_request["id"]
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
                ['cmd', '/c', '%s 2>&1 > %s' % (command, build_output)])
        else:
            # Launch docker build script.

            # TODO(jojy): Launch 'docker_build.sh' in subprocess so that
            # verifications can be run in parallel for various configurations.
            configuration = ("export "
                             "OS='ubuntu:14.04' "
                             "BUILDTOOL='autotools' "
                             "COMPILER='gcc' "
                             "CONFIGURATION='--verbose "
                             "--disable-libtool-wrappers' "
                             "ENVIRONMENT='GLOG_v=1 MESOS_VERBOSE=1'")

            command = "%s; ./support/docker-build.sh" % configuration

            # `tee` the output so that the console can log the whole build
            # output. `pipefail` ensures that the exit status of the build
            # command ispreserved even after tee'ing.
            subprocess.check_call(['bash', '-c',
                                   ('set -o pipefail; %s 2>&1 | tee %s')
                                   % (command, build_output)])

        # Success!
        post_review(
            review_request,
            "Patch looks great!\n\n" \
            "Reviews applied: %s\n\n" \
            "Passed command: %s" % (reviews, command))
    except subprocess.CalledProcessError as err:
        # If we are here because the docker build command failed, read the
        # output from `build_output` file. For all other command failures read
        # the output from `e.output`.
        if os.path.exists(build_output):
            output = open(build_output).read()
        else:
            output = err.output

        if platform.system() == 'Windows':
            # We didn't output anything during the build (because `tee`
            # doesn't exist), so we print the output to stdout upon error.
            print output

        # Truncate the output when posting the review as it can be very large.
        if len(output) > REVIEW_SIZE:
            output = "...<truncated>...\n" + output[-REVIEW_SIZE:]

        output += "\nFull log: "
        output += urlparse.urljoin(os.environ['BUILD_URL'], 'console')

        post_review(
            review_request,
            "Bad patch!\n\n" \
            "Reviews applied: %s\n\n" \
            "Failed command: %s\n\n" \
            "Error:\n%s" % (reviews, err.cmd, output))
    except ReviewError as err:
        post_review(
            review_request,
            "Bad review!\n\n" \
            "Reviews applied: %s\n\n" \
            "Error:\n%s" % (reviews, err.args[0]))

    # Clean up.
    cleanup()


def needs_verification(review_request):
    """Return True if this review request needs to be verified."""
    print "Checking if review: %s needs verification" % review_request["id"]

    # Skip if the review blocks another review.
    if review_request["blocks"]:
        print "Skipping blocking review %s" % review_request["id"]
        return False

    diffs_url = review_request["links"]["diffs"]["href"]
    diffs = api(diffs_url)

    if len(diffs["diffs"]) == 0:  # No diffs attached!
        print "Skipping review %s as it has no diffs" % review_request["id"]
        return False

    # Get the timestamp of the latest diff.
    timestamp = diffs["diffs"][-1]["timestamp"]
    rb_date_format = "%Y-%m-%dT%H:%M:%SZ"
    diff_time = datetime.strptime(timestamp, rb_date_format)
    print "Latest diff timestamp: %s" % diff_time

    # Get the timestamp of the latest review from this script.
    reviews_url = review_request["links"]["reviews"]["href"]
    reviews = api(reviews_url + "?max-results=200")
    review_time = None
    for review in reversed(reviews["reviews"]):
        if review["links"]["user"]["title"] == USER:
            timestamp = review["timestamp"]
            review_time = datetime.strptime(timestamp, rb_date_format)
            print "Latest review timestamp: %s" % review_time
            break

    # TODO: Apply this check recursively up the dependency chain.
    changes_url = review_request["links"]["changes"]["href"]
    changes = api(changes_url)
    dependency_time = None
    for change in changes["changes"]:
        if "depends_on" in change["fields_changed"]:
            timestamp = change["timestamp"]
            dependency_time = datetime.strptime(timestamp, rb_date_format)
            print "Latest dependency change timestamp: %s" % dependency_time
            break

    # Needs verification if there is a new diff, or if the dependencies changed,
    # after the last time it was verified.
    return not review_time or review_time < diff_time or \
        (dependency_time and review_time < dependency_time)


def main():
    """Main function to verify the submitted reviews."""
    review_requests_url = \
        "%s/api/review-requests/%s" % (REVIEWBOARD_URL, QUERY_PARAMS)

    review_requests = api(review_requests_url)
    num_reviews = 0
    for review_request in reversed(review_requests["review_requests"]):
        if (NUM_REVIEWS == -1 or num_reviews < NUM_REVIEWS) and \
           needs_verification(review_request):
            verify_review(review_request)
            num_reviews += 1

if __name__ == '__main__':
    main()
