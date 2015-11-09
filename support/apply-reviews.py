#!/usr/bin/env python
import argparse
import json
import re
import subprocess
import sys
import urllib2


REVIEW_REQUEST_URL = 'https://reviews.apache.org/api/review-requests'


def review_url(review_id):
  """Returns a Review Board URL given a review ID."""
  # Reviewboard REST API expects '/' at the end of the URL.
  return '{base}/{review}/'.format(base=REVIEW_REQUEST_URL, review=review_id)


def review_json(url):
  """Performs HTTP request and returns JSON-ified response."""
  # Will throw in case of error.
  json_str = urllib2.urlopen(url).read()
  return json.loads(json_str)


def extract_review_id(url):
  """Extracts Review ID from Review Board URL."""
  review_id = re.search(REVIEW_REQUEST_URL + '/(\d+)/', url)
  if review_id:
    return review_id.group(1)


def review_chain(review_id):
  """Returns a list of parent reviews of a given Review ID."""
  json_obj = review_json(review_url(review_id))

  # Stop as soon as we stumble upon a submitted request.
  status = json_obj.get('review_request').get('status')
  if status == "submitted":
    return []

  depends = json_obj.get('review_request').get('depends_on')

  # Verify that the review has exactly one parent.
  if len(depends) > 1:
    sys.stderr.write('Error: Review {review} has more than one parent\n'\
                     .format(review=review_id))
    sys.exit(1)
  elif len(depends) == 0:
    return [(review_id, json_obj.get('review_request').get('summary'))]
  else:
    # The review has exactly one parent.
    review = (review_id, json_obj.get('review_request').get('summary'))
    review_list = review_chain(extract_review_id(depends[0].get('href')))

    if review not in review_list:
      return review_list + [review]
    else:
      sys.stderr.write('Found a circular dependency in the chain starting at '
                       '{review}\n'.format(review=review_id))
      sys.exit(1)


def shell(command):
  """Runs a command in a shell."""
  print command
  return subprocess.check_output(command, stderr=subprocess.STDOUT, shell=True)


def apply_review(review_id, summary, dry_run):
  """Applies a review with a given ID locally."""
  print 'Applying review {review}: {message}'.format(review=review_id,
                                                     message=summary)
  command = './support/apply-review.sh -n -r {review}'.format(review=review_id)

  if dry_run:
    print command
    return

  try:
    return shell(command)
  except subprocess.CalledProcessError as e:
    sys.stderr.write(e.output)
    sys.exit(e.returncode)


if __name__ == "__main__":
  # Parse command line.
  parser = argparse.ArgumentParser(description='Recursively apply Review '
                                   'Board reviews.')
  parser.add_argument('-r',
                      '--review-id',
                      metavar='REVIEW_ID',
                      help='Numeric Review ID that needs to be applied.',
                      required=True)
  parser.add_argument('-d',
                      '--dry-run',
                      action='store_true',
                      help='Perform a dry run.')

  args = parser.parse_args()

  # Retrieve the list of reviews to apply.
  reviews = review_chain(args.review_id)
  dry_run = args.dry_run
  applied = set()

  for review_id, summary in reviews:
    if review_id not in applied:
      applied.add(review_id)
      apply_review(review_id, summary, dry_run)
