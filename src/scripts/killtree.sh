#!/bin/sh

PID=
SIGNAL="TERM"
KILLGROUPS=0
KILLSESS=0
VERBOSE=0


usage() {
    cat <<EOF
usage: $0 -p pid -s signal

Sends a signal to a process tree rooted at the specified pid.

Options:
-h      Display this message.
-p      Root pid of tree.
-s      Signal to send (number or symbolic name), default is TERM
-g      Recursively invoke on all processes that are members
        of any encountered process group.
-x      Same as -g, but invoke on processes of encountered sessions.
-v      Be more verbose.
EOF
}

set_union() {
    A="$1"
    B="$2"

    result=$(printf "${A}\n${B}\n" | sort | uniq)
    echo "${result}"
}

set_intersect() {
    A="$1"
    B="$2"

    result=$(printf "${A}\n${B}" \
        | sort | uniq -c | awk '{ if ($1 == 2) print $2 }')

    echo "${result}"
}

set_diff() {
    A="$1"
    B="$2"

    result=$(printf "${A}\n${B}" \
        | sort | uniq -c | awk '{ if ($1 == 1) print $2 }')

    echo "${result}"
}

while getopts "hp:s:gxv" OPTION
do
    HAVEOPT=1
    case ${OPTION} in
        h)
            usage
            exit 0
            ;;
        p)
            PID="${OPTARG}"
            test ${PID} -eq ${PID} >& /dev/null
            if [[ ${?} != 0 ]]; then
              echo "$(basename ${0}): pid should be a number"
              exit 1
            fi
            if [[ ! ${PID} -eq ${PID} ]]; then
                exit 1
            fi
            ;;
        s)
            SIGNAL="${OPTARG}"
            if [[ -z ${SIGNAL} ]]; then
              SIGNAL="TERM"
            fi
            ;;
        g)
            KILLGROUPS=1
            ;;
	x)
	    KILLSESSIONS=1
	    ;;
        v)
            VERBOSE=1
            ;;
        *)  usage
            exit 1
            ;;
    esac
done

if [[ ! ${HAVEOPT} ]]; then
    usage
    exit 127
fi

if [[ -z ${PID} ]]; then
    echo "$(basename ${0}): must specify pid"
    exit 1
fi

# Confirm we get some output from ps.
ps axo ppid,pid >/dev/null 2>&1
if [[ ${?} -ne 0 ]]; then
    echo "$(basename ${0}): failed to capture process tree using 'ps'"
    exit 1
fi

# Confirm we have awk.
ls | awk {'print $1'} >/dev/null 2>&1
if [[ ${?} -ne 0 ]]; then
    echo "$(basename ${0}): failed to detect awk on the system"
    exit 1
fi


PIDS=${PID} # Already processed pids, global variable hack.

killtree() {
    MYPID=${1}

    # Output pid if requested.
    if [[ ${VERBOSE} -eq 1 ]]; then
        printf "\nkilltree on ${MYPID}\n"
    fi

    # Stop the process to keep it from forking while we are killing it
    # since a forked child might get re-parented by init and become
    # impossible to find.
    kill -STOP ${MYPID} >/dev/null 2>&1

    # There is a concern that even though some process is stopped,
    # sending a signal to any of it's children may cause a SIGCLD to
    # be delivered to it which wakes it up (or any other signal maybe
    # delivered). However, from the Open Group standards on "Signal
    # Concepts":
    #
    #   "While a process is stopped, any additional signals that are
    #    sent to the process shall not be delivered until the process
    #    is continued, except SIGKILL which always terminates the
    #    receiving process."
    #
    # In practice, this is not what has been witnessed. Rather, a
    # process that has been stopped will respond to SIGTERM, SIGINT,
    # etc. That being said, we still continue the process below in the
    # event that it doesn't terminate from the sending signal but it
    # also doesn't get continued (as per the specifications above).

    # Now collect all the children.
    CHILDPIDS=$(ps axo ppid,pid | awk '{ if ($1 == '${MYPID}') print $2 }')

    # Optionally collect all processes that are part of the process group.
    GROUPPIDS=""
    if [[ ${KILLGROUPS} -eq 1 ]]; then

	# First get the process group.
        MYPGID=$(ps axo pid,pgid | awk '{ if ($1 == '${MYPID}') print $2 }')

	if [[ ! -z ${MYPGID} ]]; then
            # Now get all members.
            GROUPPIDS=$(ps axo pgid,pid \
                | awk '{ if ($1 == '${MYPGID}') print $2 }')
	fi
    fi

    # Optionally collect all processes that are part of the same session.
    SESSPIDS=""
    if [[ ${KILLSESSIONS} -eq 1 ]]; then

	# First get the process session id.
        MYPSID=$(ps axo pid,sess | awk '{ if ($1 == '${MYPID}') print $2 }')

	if [[ ! -z ${MYPSID} ]]; then
            # Now get all members.
            SESSPIDS=$(ps axo sess,pid \
                | awk '{ if ($1 == "'${MYPSID}'") print $2 }')
	fi
    fi

    # Get out only the unseen pids.
    # NEW = CHILDPIDS U GROUPPIDS U SESSPIDS
    #NEW=$(printf "${CHILDPIDS}\n${GROUPPIDS}\n${SESSPIDS}" | sort | uniq)
    NEW=$(set_union "$(set_union "${CHILDPIDS}" "${GROUPPIDS}")" "${SESSPIDS}")

    # OLD = NEW ^ PIDS
    #OLD=$(printf "${PIDS}\n${NEW}" \
    #    | sort | uniq -c | awk '{ if ($1 == 2) print $2 }')
    OLD=$(set_intersect "${PIDS}" "${NEW}")

    # NEW = NEW - OLD
    #NEW=$(printf "${NEW}\n${OLD}" \
    #    | sort | uniq -c | awk '{ if ($1 == 1) print $2 }')
    NEW=$(set_diff "${NEW}" "${OLD}")

    # Add all the new pids.
    PIDS=$(printf "${NEW}\n${PIDS}" | sort)

    # Now send the signal.
    kill -${SIGNAL} ${MYPID} >/dev/null 2>&1

    # Try and continue the process in case ${SIGNAL} is
    # non-terminating but doesn't continue the process.
    kill -CONT ${MYPID} >/dev/null 2>&1

    for pid in ${NEW}; do
        killtree $pid
    done
}

killtree ${PID}
