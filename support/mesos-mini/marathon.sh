#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail -o verbose

exec /usr/local/marathon/bin/marathon \
    -Duser.dir=/var/lib/marathon \
    -J-server \
    -J-verbose:gc \
    -J-XX:+PrintGCDetails \
    -J-XX:+PrintGCTimeStamps \
    --master "localhost:5050" \
    --default_accepted_resource_roles "*" \
    --mesos_role "marathon" \
    --max_instances_per_offer 100 \
    --task_launch_timeout 86400000 \
    --decline_offer_duration 300000 \
    --revive_offers_for_new_apps \
    --mesos_leader_ui_url "/mesos" \
    --enable_features "task_killing" \
    --mesos_user "root" \
    --mesos_authentication_principal "marathon" \
    --internal_store_backend "mem" \
    --disable_ha
