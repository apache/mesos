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
This file is used to get the dependent review IDs of a specific review
request on the ReviewBoard.
"""

import argparse

from common import ReviewBoardHandler
from common import REVIEWBOARD_URL


def parse_parameters():
    """Method for parsing CLI arguments using argparse."""
    parser = argparse.ArgumentParser(
        description="Get all dependent review IDs")
    parser.add_argument("-r", "--review-id", type=str, required=True,
                        help="Review ID")
    parser.add_argument("-o", "--out-file", type=str, required=False,
                        help="The out file with the reviews IDs")
    return parser.parse_args()


def main():
    """
    Main method to get dependent review IDs of a specific review request
    on the ReviewBoard.
    """
    parameters = parse_parameters()
    review_request_url = "%s/api/review-requests/%s/" % (REVIEWBOARD_URL,
                                                         parameters.review_id)
    handler = ReviewBoardHandler()
    review_request = handler.api(review_request_url)["review_request"]
    review_ids = handler.get_dependent_review_ids(review_request)
    if parameters.out_file:
        with open(parameters.out_file, 'w') as f:
            for r_id in review_ids:
                f.write("%s\n" % (str(r_id)))
    else:
        for r_id in review_ids:
            print("%s\n" % (str(r_id)))


if __name__ == '__main__':
    main()
