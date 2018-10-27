#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail -o verbose

CGROUP=$(grep memory /proc/1/cgroup | cut -d: -f3)

cat <<EOF > "/etc/docker/env"
CGROUP_PARENT=${CGROUP}/docker
EOF
