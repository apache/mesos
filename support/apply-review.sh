#!/usr/bin/env bash

# Provides a tool to "apply" a review from Review Board.

# Use 'atexit' for cleanup.
. $(dirname ${0})/atexit.sh

# Use colors for errors.
. $(dirname ${0})/colors.sh

test ${#} -eq 1 || \
  { echo "Usage: `basename ${0}` [review]"; exit 1; }

REVIEW=${1}

REVIEW_URL="https://reviews.apache.org/r/${REVIEW}"
DIFF_URL="${REVIEW_URL}/diff/raw/"

atexit "rm -f ${REVIEW}.patch"

wget --no-check-certificate -O ${REVIEW}.patch ${DIFF_URL} || \
  { echo "${RED}Failed to download patch${NORMAL}"; exit 1; }

git apply --index ${REVIEW}.patch || \
  { echo "${RED}Failed to apply patch${NORMAL}"; exit 1; }

API_URL="https://reviews.apache.org/api/review-requests/${REVIEW}/"

JSONURL=$(dirname ${0})/jsonurl.py

SUMMARY=$(${JSONURL} ${API_URL} review_request summary)
DESCRIPTION=$(${JSONURL} ${API_URL} review_request description)
SUBMITTER=$(${JSONURL} ${API_URL} review_request links submitter title)

USER_URL="https://reviews.apache.org/api/users/${SUBMITTER}/"

REVIEWER=$(${JSONURL} ${USER_URL} user fullname)
REVIEWER_EMAIL=$(${JSONURL} ${USER_URL} user email)

MESSAGE=$(cat <<__EOF__
${SUMMARY}

${DESCRIPTION}

From: ${REVIEWER} <${REVIEWER_EMAIL}>
Review: ${REVIEW_URL}
__EOF__
)

git commit -am "${MESSAGE}" || \
  { echo "${RED}Failed to commit patch${NORMAL}"; exit 1; }

git commit --amend
