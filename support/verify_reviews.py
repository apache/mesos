#!/usr/bin/env python2.7

# Provides a tool to verify Mesos reviews that are submitted to
# Review Board.

import atexit
import json
import os
import subprocess
import sys
import urllib
import urllib2

from datetime import datetime, timedelta

REVIEWBOARD_URL = "https://reviews.apache.org"
REVIEW_SIZE = 1000000 # 1 MB in bytes.

# TODO(vinod): Use 'argparse' module.
# Get the user and password from command line.
if len(sys.argv) < 3:
    print "Usage: ./verify-reviews.py <user> <password> [num-reviews] [query-params]"
    sys.exit(1)

USER = sys.argv[1]
PASSWORD = sys.argv[2]

# Number of reviews to verify.
NUM_REVIEWS = -1 # All possible reviews.
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
  pass


def shell(command):
    print command
    return subprocess.check_output(
        command, stderr=subprocess.STDOUT, shell=True)


HEAD = shell("git rev-parse HEAD")


def api(url, data=None):
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
    except urllib2.HTTPError as e:
        print "Error handling URL %s: %s (%s)" % (url, e.reason, e.read())
        exit(1)
    except urllib2.URLError as e:
        print "Error handling URL %s: %s" % (url, e.reason)
        exit(1)


def apply_review(review_id):
    print "Applying review %s" % review_id
    shell("./support/apply-review.sh -n -r %s" % review_id)


def apply_reviews(review_request, reviews):
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
    print "Posting review: %s" % message

    review_url = review_request["links"]["reviews"]["href"]
    data = urllib.urlencode({'body_top' : message, 'public' : 'true'})
    api(review_url, data)


@atexit.register
def cleanup():
    try:
        shell("git clean -fd")
        shell("git reset --hard %s" % HEAD)
    except subprocess.CalledProcessError as e:
        print "Failed command: %s\n\nError: %s" % (e.cmd, e.output)


def verify_review(review_request):
    print "Verifying review %s" % review_request["id"]
    build_output = "build_" + str(review_request["id"])

    try:
        # Recursively apply the review and its dependents.
        reviews = []
        apply_reviews(review_request, reviews)

        reviews.reverse() # Reviews are applied in the reverse order.

        # Launch docker build script.

        # TODO(jojy): Launch 'docker_build.sh' in subprocess so that
        # verifications can be run in parallel for various configurations.
        configuration = ("export "
                         "OS='ubuntu:14.04' "
                         "BUILDTOOL='autotools' "
                         "COMPILER='gcc' "
                         "CONFIGURATION='--verbose' "
                         "ENVIRONMENT='GLOG_v=1 MESOS_VERBOSE=1'")

        command = "%s; ./support/docker_build.sh" % configuration


        # `tee` the output so that the console can log the whole build output.
        # `pipefail` ensures that the exit status of the build command is
        # preserved even after tee'ing.
        subprocess.check_call(['bash', '-c', 'set -o pipefail; %s 2>&1 | tee %s'
                               % (command, build_output)])

        # Success!
        post_review(
            review_request,
            "Patch looks great!\n\n" \
            "Reviews applied: %s\n\n" \
            "Passed command: %s" % (reviews, command))
    except subprocess.CalledProcessError as e:
        # If we are here because the docker build command failed, read the
        # output from `build_output` file. For all other command failures read
        # the output from `e.output`.
        output = open(build_output).read() if os.path.exists(build_output) else e.output

        # Truncate the output when posting the review as it can be very large.
        output = output if len(output) <= REVIEW_SIZE else "...<truncated>...\n" + output[-REVIEW_SIZE:]
        output = output + "\nFull log: " + os.path.join(os.environ['BUILD_URL'], 'console')

        post_review(
            review_request,
            "Bad patch!\n\n" \
            "Reviews applied: %s\n\n" \
            "Failed command: %s\n\n" \
            "Error:\n%s" % (reviews, e.cmd, output))
    except ReviewError as e:
        post_review(
            review_request,
            "Bad review!\n\n" \
            "Reviews applied: %s\n\n" \
            "Error:\n%s" % (reviews, e.args[0]))

    # Clean up.
    cleanup()


# Returns true if this review request needs to be verified.
def needs_verification(review_request):
    print "Checking if review: %s needs verification" % review_request["id"]

    # Skip if the review blocks another review.
    if review_request["blocks"]:
        print "Skipping blocking review %s" % review_request["id"]
        return False

    diffs_url = review_request["links"]["diffs"]["href"]
    diffs = api(diffs_url)

    if len(diffs["diffs"]) == 0: # No diffs attached!
        print "Skipping review %s as it has no diffs" % review_request["id"]
        return False

    RB_DATE_FORMAT = "%Y-%m-%dT%H:%M:%SZ"

    # Get the timestamp of the latest diff.
    timestamp = diffs["diffs"][-1]["timestamp"]
    diff_time = datetime.strptime(timestamp, RB_DATE_FORMAT)
    print "Latest diff timestamp: %s" % diff_time

    # Get the timestamp of the latest review from this script.
    reviews_url = review_request["links"]["reviews"]["href"]
    reviews = api(reviews_url + "?max-results=200")
    review_time = None
    for review in reversed(reviews["reviews"]):
        if review["links"]["user"]["title"] == USER:
            timestamp = review["timestamp"]
            review_time = datetime.strptime(timestamp, RB_DATE_FORMAT)
            print "Latest review timestamp: %s" % review_time
            break

    # TODO: Apply this check recursively up the dependency chain.
    changes_url = review_request["links"]["changes"]["href"]
    changes = api(changes_url)
    dependency_time = None
    for change in changes["changes"]:
        if "depends_on" in change["fields_changed"]:
            timestamp = change["timestamp"]
            dependency_time = datetime.strptime(timestamp, RB_DATE_FORMAT)
            print "Latest dependency change timestamp: %s" % dependency_time
            break

    # Needs verification if there is a new diff, or if the dependencies changed,
    # after the last time it was verified.
    return not review_time or review_time < diff_time or \
        (dependency_time and review_time < dependency_time)


if __name__=="__main__":
    review_requests_url = "%s/api/review-requests/%s" % (REVIEWBOARD_URL, QUERY_PARAMS)

    review_requests = api(review_requests_url)
    num_reviews = 0
    for review_request in reversed(review_requests["review_requests"]):
        if (NUM_REVIEWS == -1 or num_reviews < NUM_REVIEWS) and \
            needs_verification(review_request):
            verify_review(review_request)
            num_reviews += 1
