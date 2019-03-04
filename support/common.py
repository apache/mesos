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
These common helper classes help to manage the connection between the
machine and ReviewBoard.
"""

from datetime import datetime
import json
import sys
import urllib.request as urllib2
from urllib.parse import urlencode

REVIEWBOARD_URL = "https://reviews.apache.org"


class ReviewError(Exception):
    """Custom exception raised when a review is bad"""


class ReviewBoardHandler():
    """Handler class for ReviewBoard API operations."""

    def __init__(self, user=None, password=None):
        self.user = user
        self.password = password
        self._opener_installed = False

    def _review_ids(self, review_request, review_ids=None):
        """Helper function for the 'get_review_ids' method."""
        if review_ids is None:
            review_ids = []
        if review_request["status"] != "submitted":
            review_ids.append(review_request["id"])
        else:
            print("The review request %s is already "
                  "submitted" % (review_request["id"]))
        for review in review_request["depends_on"]:
            review_url = review["href"]
            print("Dependent review: %s" % review_url)
            dependent_review = self.api(review_url)["review_request"]
            if dependent_review["id"] in review_ids:
                raise ReviewError("Circular dependency detected for "
                                  "review %s. Please fix the 'depends_on' "
                                  "field." % review_request["id"])
            self._review_ids(dependent_review, review_ids)

    def api(self, url, data=None):
        """Calls the ReviewBoard API."""
        if self._opener_installed is False:
            auth_handler = urllib2.HTTPBasicAuthHandler()
            auth_handler.add_password(
                realm="Web API",
                uri="reviews.apache.org",
                user=self.user,
                passwd=self.password)
            opener = urllib2.build_opener(auth_handler)
            urllib2.install_opener(opener)
            self._opener_installed = True
        if data is not None:
            data = data.encode(sys.getdefaultencoding())
        try:
            return json.loads(urllib2.urlopen(url, data=data).read().decode(
                sys.getdefaultencoding()))
        except Exception as err:
            print("Error handling URL %s: %s" % (url, err))
            # raise the error after printing the message
            raise

    def get_dependent_review_ids(self, review_request):
        """Returns the review requests' ids (together with any potential
           dependent review requests' ids) that need to be applied for the
           current review request. Their order is ascending with respect to
           how they should be applied. This function raises a ReviewError
           exception if a cyclic dependency is found."""
        review_ids = []
        self._review_ids(review_request, review_ids)
        return list(reversed(review_ids))

    def post_review(self, review_request, message, text_type='markdown'):
        """Posts a review on the review board."""
        valid_text_types = ['markdown', 'plain']
        if text_type not in valid_text_types:
            raise Exception("Invalid %s text type when trying"
                            " to post review. Valid text"
                            " types are: %s" % (text_type, valid_text_types))
        review_request_url = "%s/r/%s" % (REVIEWBOARD_URL,
                                          review_request['id'])
        print("Posting to review request: %s\n%s" % (review_request_url,
                                                     message))
        review_url = review_request["links"]["reviews"]["href"]
        data = urlencode({'body_top': message,
                          'body_top_text_type': text_type,
                          'public': 'true'})
        self.api(review_url, data)

    def needs_verification(self, review_request):
        """Returns True if this review request needs to be verified."""
        print("Checking if review %s needs verification" % (
            review_request["id"]))
        rb_date_format = "%Y-%m-%dT%H:%M:%SZ"

        # Now apply this review if not yet submitted.
        if review_request["status"] == "submitted":
            print("The review is already submitted")
            return False

        # Skip if the review blocks another review.
        if review_request["blocks"]:
            print("Skipping blocking review %s" % review_request["id"])
            return False

        # Get the timestamp of the latest review from this script.
        reviews_url = review_request["links"]["reviews"]["href"]
        reviews = self.api(reviews_url + "?max-results=200")
        review_time = None
        for review in reversed(reviews["reviews"]):
            if review["links"]["user"]["title"] == self.user:
                timestamp = review["timestamp"]
                review_time = datetime.strptime(timestamp, rb_date_format)
                print("Latest review timestamp: %s" % review_time)
                break
        if not review_time:
            # Never reviewed, the review request needs to be verified.
            print("Patch never verified, needs verification")
            return True

        # Every patch must have a diff.
        latest_diff = self.api(review_request["links"]["diffs"]["href"])

        # Get the timestamp of the latest diff.
        timestamp = latest_diff["diffs"][-1]["timestamp"]
        diff_time = datetime.strptime(timestamp, rb_date_format)
        print("Latest diff timestamp: %s" % diff_time)

        # NOTE: We purposefully allow the bot to run again on empty reviews
        # so that users can re-trigger the build.
        if review_time < diff_time:
            # There is a new diff, needs verification.
            print("This patch has been updated since its last review, needs"
                  " verification.")
            return True

        # TODO(dragoshsch): Apply this check recursively up the dependency
        # chain.
        changes_url = review_request["links"]["changes"]["href"]
        changes = self.api(changes_url)
        dependency_time = None
        for change in changes["changes"]:
            if "depends_on" in change["fields_changed"]:
                timestamp = change["timestamp"]
                dependency_time = datetime.strptime(timestamp, rb_date_format)
                print("Latest dependency change timestamp: %s" %
                      dependency_time)
                break

        # Needs verification if there is a new diff, or if the
        # dependencies changed, after the last time it was verified.
        return dependency_time and review_time < dependency_time
