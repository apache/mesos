#!/bin/bash

# Usage: pushToBintray.sh RPM
# The following environment variables must be set:
#   BINTRAY_CREDS (In the form <username>:<api-key>)
#   MESOS_TAG (1.4.0-rc1, 1.4.0, etc.)

# The following default to "mesos" if not specified.
#   BINTRAY_ORG
#   BINTRAY_PKG


# Sample yum repo file:
# #bintray-mesos-el - packages by mesos from Bintray
# [bintray-mesos-el]
# name=bintray-mesos-el
# baseurl=https://dl.bintray.com/mesos/el/7/x86_64
# gpgcheck=0
# repo_gpgcheck=0
# enabled=1
#
# #bintray-mesos-el-srpm - source rpm packages by mesos from Bintray
# [bintray-mesos-el-srpm]
# name=bintray-mesos-el-srpm
# baseurl=https://dl.bintray.com/mesos/el/7/SRPMS
# gpgcheck=0
# repo_gpgcheck=0
# enabled=1

set -o errexit -o nounset

API=https://api.bintray.com

BINTRAY_ORG=${BINTRAY_ORG:-mesos}
BINTRAY_PKG=${BINTRAY_PKG:-mesos}

PKG_PATH=$1
PKG_FILENAME=$(basename $PKG_PATH)

REPO_SUFFIX=""
MESOS_VERSION=${MESOS_TAG%[-]*}
case "$PKG_FILENAME" in
  *.pre.*git*) REPO_SUFFIX="-unstable" ;;
  *-rc*)       REPO_SUFFIX="-testing" ;;
esac

REPO_BASE=""
if [ "${PKG_PATH##*.}" = "rpm" ]; then
  REPO="el${REPO_SUFFIX}"
fi

REPO_PATH=""
case "$PKG_PATH" in
  *.el7.src.rpm)    REPO_PATH=7/SRPMS ;;
  *.el6.src.rpm)    REPO_PATH=6/SRPMS ;;

  *.el7.x86_64.rpm) REPO_PATH=7/x86_64 ;;
  *.el6.x86_64.rpm) REPO_PATH=6/x86_64 ;;
esac


# Refer to https://bintray.com/docs/api/#_upload_content for Bintray upload API.
# The following command is inspired from an example script provided by bintray:
#  https://github.com/bintray/bintray-examples/blob/master/bash-example/pushToBintray.sh

echo "Uploading ${PKG_FILENAME} to ${API}/content/${BINTRAY_ORG}/${REPO}/${REPO_PATH}/${PKG_FILENAME}..."

result=$(curl                           \
  -u${BINTRAY_CREDS}                    \
  -H Content-Type:application/json      \
  -H Accept:application/json            \
  --write-out %{http_code}              \
  --silent --output /dev/null           \
  -T ${PKG_PATH}                        \
  -H X-Bintray-Package:${BINTRAY_PKG}   \
  -H X-Bintray-Version:${MESOS_VERSION} \
  -H X-Bintray-Publish:1                \
  ${API}/content/${BINTRAY_ORG}/${REPO}/${REPO_PATH}/${PKG_FILENAME})

if [ $result -ne 201 ]; then
  echo "Package ${PKG_FILENAME} upload to ${API}/content/${BINTRAY_ORG}/${REPO}/${REPO_PATH}/${PKG_FILENAME} failed with status ${result}"
  exit 1
fi
