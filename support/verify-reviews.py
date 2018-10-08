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
import time
import datetime
import json
import os
import platform
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

from common import ReviewBoardHandler, ReviewError, REVIEWBOARD_URL

REVIEW_SIZE = 1000000  # 1 MB in bytes.
# This is the mesos repo ID obtained from querying the reviews.apache.org API
MESOS_REPOSITORY_ID = 122


def parse_parameters():
    """Method to parse the arguments for argparse."""
    parser = argparse.ArgumentParser(
        description="Reviews that need verification from the Review Board")
    parser.add_argument("-u", "--user", type=str, required=True,
                        help="Review Board user name")
    parser.add_argument("-p", "--password", type=str, required=True,
                        help="Review Board user password")
    parser.add_argument("-r", "--reviews", type=int, required=False,
                        default=-1, help="The number of reviews to fetch,"
                                         " that will need verification")
    parser.add_argument("--skip-verify", action='store_true', required=False,
                        help="Skip the verification and just write the review"
                             " ids that need verification")
    default_hours_behind = 8
    datetime_before = (datetime.datetime.now() -
                       datetime.timedelta(hours=default_hours_behind))
    datetime_before_string = datetime_before.isoformat()
    default_query = {"status": "pending", "repository": MESOS_REPOSITORY_ID,
                     "last-updated-from": datetime_before_string.split(".")[0]}
    parser.add_argument("-q", "--query", type=str, required=False,
                        help="Query parameters, passed as string in JSON"
                             " format. Example: '%s'" % json.dumps(
                                 default_query),
                        default=json.dumps(default_query))
    parser.add_argument("-o", "--out-file", type=str, required=False,
                        help="The out file with the reviews IDs that"
                             " need verification")

    return parser.parse_args()


def shell(command):
    """Run a shell command."""
    print(command)
    return subprocess.check_output(
        command, stderr=subprocess.STDOUT, shell=True)


def apply_review(review_id):
    """Apply a review using the script apply-reviews.py."""
    print("Applying review %s" % review_id)
    shell("%s support/apply-reviews.py -n -r %s" % (sys.executable, review_id))


def apply_reviews(review_request, reviews, handler):
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
        print("Dependent review: %s" % review_url)
        apply_reviews(handler.api(review_url)["review_request"],
                      reviews, handler)

    # Now apply this review if not yet submitted.
    if review_request["status"] != "submitted":
        apply_review(review_request["id"])


def post_review(review_request, message, handler):
    """Post a review on the review board."""
    print("Posting review: %s" % message)

    review_url = review_request["links"]["reviews"]["href"]
    data = urllib.parse.urlencode({'body_top': message, 'public': 'true'})
    handler.api(review_url, data)


# @atexit.register
def cleanup():
    """Clean the git repository."""
    try:
        shell("git clean -fd")
        HEAD = shell("git rev-parse HEAD")
        print(HEAD)
        shell("git checkout HEAD -- %s" % HEAD)
    except subprocess.CalledProcessError as err:
        print("Failed command: %s\n\nError: %s" % (err.cmd, err.output))


def verify_review(review_request, handler):
    """Verify a review."""
    print("Verifying review %s" % review_request["id"])
    build_output = "build_" + str(review_request["id"])

    try:
        # Recursively apply the review and its dependents.
        reviews = []
        apply_reviews(review_request, reviews, handler)

        reviews.reverse()  # Reviews are applied in the reverse order.

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
                                   'set -o pipefail; %s 2>&1 | tee %s'
                                   % (command, build_output)])

        # Success!
        post_review(
            review_request,
            "Patch looks great!\n\n"
            "Reviews applied: %s\n\n"
            "Passed command: %s" % (reviews, command), handler)
    except subprocess.CalledProcessError as err:
        # If we are here because the docker build command failed, read the
        # output from `build_output` file. For all other command failures read
        # the output from `e.output`.
        #
        # Decode the RHS so that `output` is always a string.
        if os.path.exists(build_output):
            output = open(build_output).read().decode(sys.stdout.encoding)
        else:
            output = err.output.decode(sys.stdout.encoding)

        if platform.system() == 'Windows':
            # We didn't output anything during the build (because `tee`
            # doesn't exist), so we print the output to stdout upon error.

            # Pylint raises a no-member error on that line due to a bug
            # fixed in pylint 1.7.
            # TODO(ArmandGrillet): Remove this once pylint updated to >= 1.7.
            # pylint: disable=no-member
            sys.stdout.buffer.write(output.encode())

        # Truncate the output when posting the review as it can be very large.
        if len(output) > REVIEW_SIZE:
            output = "...<truncated>...\n" + output[-REVIEW_SIZE:]

        output += "\nFull log: "
        output += urllib.parse.urljoin(os.environ['BUILD_URL'], 'console')

        post_review(
            review_request,
            "Bad patch!\n\n" \
            "Reviews applied: %s\n\n" \
            "Failed command: %s\n\n" \
            "Error:\n%s" % (reviews, err.cmd, output), handler)
    except ReviewError as err:
        post_review(
            review_request,
            "Bad review!\n\n" \
            "Reviews applied: %s\n\n" \
            "Error:\n%s" % (reviews, err.args[0]), handler)

    # Clean up.
    # cleanup()


def verification_needed_write(review_ids, parameters):
    """Write the IDs of the review requests that need verification."""
    num_reviews = len(review_ids)
    print("%s review requests need verification" % num_reviews)
    if parameters.out_file is not None:
        with open(parameters.out_file, 'w') as f:
            f.write('\n'.join(review_ids))
    else:
        print('\n'.join(review_ids))


def main():
    """Main function to verify the submitted reviews."""
    parameters = parse_parameters()
    print("\n%s - Running %s" % (time.strftime('%m-%d-%y_%T'),
                                 os.path.abspath(__file__)))
    # The colon from timestamp gets encoded and we don't want it to be encoded.
    # Replacing %3A with colon.
    query_string = urllib.parse.urlencode(
        json.loads(parameters.query)).replace("%3A", ":")
    review_requests_url = "%s/api/review-requests/?%s" % (REVIEWBOARD_URL,
                                                          query_string)
    handler = ReviewBoardHandler(parameters.user, parameters.password)
    num_reviews = 0
    review_ids = []
    review_requests = handler.api(review_requests_url)
    for review_request in reversed(review_requests["review_requests"]):
        if parameters.reviews == -1 or num_reviews < parameters.reviews:
            try:
                needs_verification = handler.needs_verification(review_request)
                if not needs_verification:
                    continue
                # An exception is raised if cyclic dependencies are found
                handler.get_dependent_review_ids(review_request)
            except ReviewError as err:
                message = ("Bad review!\n\n"
                           "Error:\n%s" % (err.args[0]))
                handler.post_review(review_request, message, handler)
                continue
            except Exception as err:
                print("Error occured: %s" % err)
                needs_verification = False
                print("WARNING: Cannot find if review %s needs"
                      " verification" % (review_request["id"]))
            if not needs_verification:
                continue
            review_ids.append(str(review_request["id"]))
            num_reviews += 1
            if not parameters.skip_verify:
                verify_review(review_request, handler)

    verification_needed_write(review_ids, parameters)


if __name__ == '__main__':
    main()
