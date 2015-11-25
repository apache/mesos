#!/usr/bin/env python
import argparse
import atexit
import json
import re
import subprocess
import sys
import urllib2


REVIEW_REQUEST_URL = 'https://reviews.apache.org/api/review-requests'
USER_URL = 'https://reviews.apache.org/api/users'
PATCH_URL = 'https://reviews.apache.org/r'


def review_url(review_id):
  """Returns a Review Board URL given a review ID."""
  # Reviewboard REST API expects '/' at the end of the URL.
  return '{base}/{review}/'.format(base=REVIEW_REQUEST_URL, review=review_id)


def user_url(username):
  """Returns a Review Board URL for a user given a username."""
  # Reviewboard REST API expects '/' at the end of the URL.
  return '{base}/{user}/'.format(base=USER_URL, user=username)


def patch_url(review_id):
  """Returns a Review Board URL for a patch given a review ID."""
  # Reviewboard REST API expects '/' at the end of the URL.
  return '{base}/{review}/diff/raw/'.format(base=PATCH_URL, review=review_id)


def url_to_json(url):
  """Performs HTTP request and returns JSON-ified response."""
  json_str = urllib2.urlopen(url)
  return json.loads(json_str.read())


def extract_review_id(url):
  """Extracts review ID from Review Board URL."""
  review_id = re.search(REVIEW_REQUEST_URL + '/(\d+)/', url)
  if review_id:
    return review_id.group(1)


def review_chain(review_id):
  """Returns a parent review chain for a given review ID."""
  json_obj = url_to_json(review_url(review_id))

  # Stop as soon as we stumble upon a submitted request.
  status = json_obj.get('review_request').get('status')
  if status == "submitted":
    return []

  # Verify that the review has exactly one parent.
  depends = json_obj.get('review_request').get('depends_on')

  if len(depends) > 1:
    sys.stderr.write('Error: Review {review} has more than one '
                     'parent\n'.format(review=review_id))
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
  """Runs a command in a shell, unless the dry-run option is present (in which
  case it just prints the command."""
  if options['dry_run']:
    print command
    return

  error_code = subprocess.call(command, stderr=subprocess.STDOUT, shell=True)
  if error_code != 0:
    sys.exit(error_code)


def remove_patch(review_id):
  """Removes the patch file."""
  command = 'rm -f %s.patch' % review_id
  shell(command);


def apply_review(review_id):
  """Applies a review with a given ID locally."""
  atexit.register(lambda: remove_patch(review_id))
  fetch_patch(review_id)
  apply_patch(review_id)

  review = review_data(review_id)

  commit_patch(review)
  if (not options['no_amend']):
    amend()
  remove_patch(review_id)


def fetch_patch(review_id):
  """Fetches patch from the Review Board."""
  command = 'wget --no-check-certificate --no-verbose -O {review_id}.patch '\
            '{url}'.format(review_id=review_id , url=patch_url(review_id))
  shell(command)


def apply_patch(review_id):
  """Applies patch locally."""
  command = 'git apply --index {review_id}.patch'.format(review_id=review_id)
  shell(command)


def quote(string):
  """Quote a variable so it can be safely used in shell."""
  return string.replace("'", "'\\''")


def commit_patch(review):
  """Commit patch locally."""
  command = 'git commit --author \'{author}\' -am \'{message}\''\
            .format(author=quote(review['author']),
                    message=quote(review['message']))
  shell(command)


def review_data(review_id):
  """Fetches review data and populates internal data structure."""
  # Populate review object.
  review = url_to_json(review_url(review_id)).get('review_request')

  url = review_url(review_id)

  # Populate and escape commit message.
  message = '{summary}\n\n{description}\n\nReview: {_url}'\
            .format(summary=review.get('summary'),
                    description=review.get('description'),
                    _url=url)

  # Populate user object.
  user = url_to_json(user_url(review.get('links').get('submitter')\
                              .get('title'))).get('user')

  review_data = {
    'summary': review.get('summary'),
    'description': review.get('description'),
    'url': url,
    'author': user.get('fullname'),
    'email': user.get('email'),
    'message': message
  }

  return review_data


def amend():
  """Amends commit."""
  command = 'git commit --amend'
  shell(command)

# A global dictionary for holding execution options.
options = {}

if __name__ == "__main__":
  # Parse command line.
  parser = argparse.ArgumentParser(description='Recursively apply Review '
                                   'Board reviews.')
  parser.add_argument('-r',
                      '--review-id',
                      metavar='REVIEW_ID',
                      help='Numeric review ID that needs to be applied.',
                      required=True)
  parser.add_argument('-d',
                      '--dry-run',
                      action='store_true',
                      help='Perform a dry run.')
  parser.add_argument('-n', '--no-amend',
                      action='store_true',
                      help='Do not amend commit message.')

  args = parser.parse_args()

  # Populate the global options dictionary.
  options['review_id'] = args.review_id
  options['dry_run'] = args.dry_run
  options['no_amend'] = args.no_amend

  # Retrieve the list of reviews to apply.
  reviews = review_chain(options['review_id'])

  applied = set()
  for review_id, summary in reviews:
    if review_id not in applied:
      applied.add(review_id)
      apply_review(review_id)
