#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail

# For each cgroup subsystem, Docker does a bind mount from the current
# cgroup to the root of the cgroup subsystem. For instance:
#   /sys/fs/cgroup/memory/docker/<cid> -> /sys/fs/cgroup/memory
#
# This will confuse Mesos agent because it relies on proc file
# '/proc/<pid>/cgroup` to determine the cgroup of a given process, and
# this proc file is not affected by the bind mount.
#
# The following is a workaround to recreate the original cgroup
# environment by doing another bind mount for each subsystem.
MOUNT_TABLE=$(cat /proc/self/mountinfo)
DOCKER_CGROUP_MOUNTS=$(echo "${MOUNT_TABLE}" | grep /sys/fs/cgroup | grep docker)
DOCKER_CGROUP=$(echo "${DOCKER_CGROUP_MOUNTS}" | head -n 1 | cut -d' ' -f 4)
CGROUP_SUBSYSTEMS=$(echo "${DOCKER_CGROUP_MOUNTS}" | cut -d' ' -f 5)

echo "${CGROUP_SUBSYSTEMS}" |
while IFS= read -r SUBSYSTEM; do
  mkdir -p "${SUBSYSTEM}${DOCKER_CGROUP}"
  mount --bind "${SUBSYSTEM}" "${SUBSYSTEM}${DOCKER_CGROUP}"
done

exec /usr/sbin/init
