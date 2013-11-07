# Helper for printing out a message and then the "usage" then exiting.
def usage(message, parser):
    import sys
    sys.stderr.write(message + '\n')
    parser.print_help()
    sys.exit(-1)


# Helper that uses 'mesos-resolve' to resolve a master IP:port from
# one of:
#     zk://host1:port1,host2:port2,.../path
#     zk://username:password@host1:port1,host2:port2,.../path
#     file://path/to/file (where file contains one of the above)
def resolve(master):
    import subprocess

    process = subprocess.Popen(
        ['mesos-resolve', master],
        stdin=None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        shell=False)

    status = process.wait()
    if status != 0:
        raise Exception('Failed to execute \'mesos-resolve %s\':\n%s'
                        % (master, process.stderr.read()))

    result = process.stdout.read()
    process.stdout.close()
    process.stderr.close()
    return result
