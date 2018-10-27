#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail -o verbose

USER_NAME=$1
USER_ID=$2
GROUP_NAME=$3
GROUP_ID=$4

# Create the group.
if ! getent group "${GROUP_ID}"; then
  groupadd -r -g "${GROUP_ID}" "${GROUP_NAME}"
fi

# Create the user.
if ! getent passwd "${USER_ID}"; then
  useradd -r -m -u "${USER_ID}" "${USER_NAME}" -g "${GROUP_ID}"
fi
