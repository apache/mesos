#!/usr/bin/env bash

# Provides a tool to "apply" a review from Review Board or a
# pull request from Github.

# Use 'atexit' for cleanup.
. $(dirname ${0})/atexit.sh

# Use colors for errors.
. $(dirname ${0})/colors.sh

JSONURL=$(dirname ${0})/jsonurl.py
GITHUB_URL="https://github.com/apache/mesos/pull"
REVIEWBOARD_URL="https://reviews.apache.org/r"

function usage {
cat <<EOF
Apache Mesos apply patch tool.

Usage: $0 [-h] [-n] [-r | -g] <ID Number>

  -h   Print this help message and exit
  -n   Don't amend the commit message
  -r   Apply a patch from Review Board (default)
  -g   Apply a patch from Github
EOF
}

AMEND=true
REVIEW_LOCATION='reviewboard'
while getopts ":nhrg" opt; do
  case $opt in
    n)
      AMEND=false
      ;;
    r)
      REVIEW_LOCATION='reviewboard'
      ;;
    g)
      REVIEW_LOCATION='github'
      ;;
    h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: -$OPTARG"
      usage
      exit 1
      ;;
  esac
done

shift $(($OPTIND - 1))
if test ${#} -ne 1; then
  usage
  exit 1
fi

REVIEW=${1}

if [[ "${REVIEW_LOCATION}" == "github" ]]; then
  DIFF_URL="${GITHUB_URL}/${REVIEW}.patch"
else
  DIFF_URL="${REVIEWBOARD_URL}/${REVIEW}/diff/raw/"
fi

atexit "rm -f ${REVIEW}.patch"

wget --no-check-certificate --no-verbose -O ${REVIEW}.patch ${DIFF_URL} || \
  { echo "${RED}Failed to download patch${NORMAL}"; exit 1; }

git apply --index ${REVIEW}.patch || \
  { echo "${RED}Failed to apply patch${NORMAL}"; exit 1; }

if [[ "${REVIEW_LOCATION}" == "reviewboard" ]]; then
  API_URL="https://reviews.apache.org/api/review-requests/${REVIEW}/"

  SUMMARY=$(${JSONURL} ${API_URL} review_request summary)
  DESCRIPTION=$(${JSONURL} ${API_URL} review_request description)

  USERNAME=$(${JSONURL} ${API_URL} review_request links submitter title)
  USER_URL="https://reviews.apache.org/api/users/${USERNAME}/"

  AUTHOR_NAME=$(${JSONURL} ${USER_URL} user fullname)
  AUTHOR_EMAIL=$(${JSONURL} ${USER_URL} user email)
  AUTHOR="${AUTHOR_NAME} <${AUTHOR_EMAIL}>"
  REVIEW_URL="${REVIEWBOARD_URL}/${REVIEW}"

elif [[ "${REVIEW_LOCATION}" == "github" ]]; then
  API_URL="https://api.github.com/repos/apache/mesos/pulls/${REVIEW}"

  SUMMARY=$(${JSONURL} ${API_URL} title)
  DESCRIPTION=$(${JSONURL} ${API_URL} body)

  AUTHOR=$(head -2 ${REVIEW}.patch | grep "From: " | cut -d ' ' -f3-)
  REVIEW_URL="${GITHUB_URL}/${REVIEW}"
  REVIEW_DETAILS=$(cat <<__EOF__
This closes: #${REVIEW}

__EOF__
)
fi

MESSAGE=$(cat <<__EOF__
${SUMMARY}

${DESCRIPTION}

${REVIEW_DETAILS}
Review: ${REVIEW_URL}
__EOF__
)
echo "Successfully applied: ${MESSAGE}"

git commit --author="${AUTHOR}" -am "${MESSAGE}" || \
  { echo "${RED}Failed to commit patch${NORMAL}"; exit 1; }

if $AMEND; then
  git commit --amend
fi
