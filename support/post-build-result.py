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
This file is used to post review results to the ReviewBoard.
"""

import argparse
import sys
import urllib.request as urllib2

from common import ReviewBoardHandler, REVIEWBOARD_URL  # noqa

LOG_TAIL_LIMIT = 30


def parse_parameters():
    """Method for parsing arguments for argparse."""
    parser = argparse.ArgumentParser(
        description="Post review results to Review Board")
    parser.add_argument("-u", "--user", type=str, required=True,
                        help="Review board user name")
    parser.add_argument("-p", "--password", type=str, required=True,
                        help="Review board user password")
    parser.add_argument("-r", "--review-id", type=str, required=True,
                        help="Review ID")
    parser.add_argument("-m", "--message", type=str, required=True,
                        help="The post message")
    parser.add_argument("-o", "--outputs-url", type=str, required=True,
                        help="The output build artifacts URL")
    parser.add_argument("-l", "--logs-urls", type=str, required=False,
                        help="The URLs for the logs to be included in the"
                             " posted build message")
    parser.add_argument("--applied-reviews", type=str, required=False,
                        help="The Review IDs that have been applied for the"
                             " current patch")
    parser.add_argument("--failed-command", type=str, required=False,
                        help="The command that failed during the build"
                             " process")
    return parser.parse_args()


def get_build_message(message, outputs_url, logs_urls=None,
                      applied_reviews=None,
                      failed_command=None):
    """Retrieves build message."""
    if logs_urls is None:
        logs_urls = []
    if applied_reviews is None:
        applied_reviews = []
    build_msg = "%s\n\n" % message
    if applied_reviews:
        build_msg += "Reviews applied: `%s`\n\n" % applied_reviews
    if failed_command:
        build_msg += "Failed command: `%s`\n\n" % failed_command
    build_msg += ("All the build artifacts available"
                  " at: %s\n\n" % (outputs_url))
    logs_msg = ''
    for url in logs_urls:
        response = urllib2.urlopen(url)
        log_content = response.read().decode(sys.getdefaultencoding())
        if log_content == '':
            continue
        file_name = url.split('/')[-1]
        logs_msg += "- [%s](%s):\n\n" % (file_name, url)
        logs_msg += "```\n"
        log_tail = log_content.split("\n")[-LOG_TAIL_LIMIT:]
        logs_msg += "\n".join(log_tail)
        logs_msg += "```\n\n"
    if logs_msg == '':
        return build_msg
    build_msg += "Relevant logs:\n\n%s" % (logs_msg)
    return build_msg.encode(sys.getdefaultencoding())


def main():
    """Posts build result to the ReviewBoard"""
    parameters = parse_parameters()
    review_request_url = "%s/api/review-requests/%s/" % (REVIEWBOARD_URL,
                                                         parameters.review_id)
    handler = ReviewBoardHandler(parameters.user, parameters.password)
    review_request = handler.api(review_request_url)["review_request"]
    logs_urls = []
    if parameters.logs_urls:
        logs_urls = parameters.logs_urls.split('|')
    applied_reviews = []
    if parameters.applied_reviews:
        applied_reviews = parameters.applied_reviews.split('|')
    message = get_build_message(message=parameters.message,
                                logs_urls=logs_urls,
                                applied_reviews=applied_reviews,
                                outputs_url=parameters.outputs_url,
                                failed_command=parameters.failed_command)
    handler.post_review(review_request, message)


if __name__ == '__main__':
    main()
