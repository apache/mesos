#!/usr/bin/env python

from __future__ import with_statement

import boto
import logging
import os
import subprocess
import sys
import time
from boto import ec2
from optparse import OptionParser
from sys import stderr
from tempfile import NamedTemporaryFile


def parse_args():
  parser = OptionParser(usage="nexus-ec2 [options] <action> <cluster_name>"
      + "\n\n<action> can be: launch, shutdown, login",
      add_help_option=False)
  parser.add_option("-h", "--help", action="help",
                    help="Show this help message and exit")
  parser.add_option("-s", "--slaves", type="int", default=1,
      help="Number of slaves to launch (default: 1)")
  parser.add_option("-k", "--key-pair",
      help="Key pair to use on instances")
  parser.add_option("-i", "--identity-file", 
      help="SSH private key file to use for logging into instances")
  parser.add_option("-t", "--instance-type", default="m1.large",
      help="Type of instance to launch (default: m1.large). WARNING: must be 64 bit, thus small instances won't work")
  parser.add_option("-m", "--master-instance-type", default="",
      help="Master instance type (leave empty for same as instance-type)")
  parser.add_option("-z", "--zone", default="us-east-1b",
      help="Availability zone to launch instances in")
  parser.add_option("-a", "--ami", default="ami-dd16f9b4", # nexus-karmic-0.4
      help="Amazon Machine Image ID to use")
  parser.add_option("-o", "--os", default="karmic64",
      help="OS on the Amazon Machine Image (karmic64 or solaris)")
  parser.add_option("-d", "--download", metavar="SOURCE", default="none",
      help="Where to download latest code from: set to 'git' to check out " +
           "from git, 'web' for latest snapshot .tgz, or 'none' to use " +
           "the build of Nexus on the AMI (default)")
  parser.add_option("-b", "--branch", default="master",
      help="if using git, which branch to check out. default is 'master'")
  parser.add_option("-D", metavar="[ADDRESS:]PORT", dest="proxy_port", 
      help="Use SSH dynamic port forwarding to create a SOCKS proxy at " +
            "the given local address (for use with login)")
  parser.add_option("--resume", action="store_true", default=False,
      help="Resume installation on a previously launched cluster " +
           "(for debugging)")
  parser.add_option("-f", "--ft", default="1", 
      help="Number of masters to run. Default is 1. " + 
           "Greater values cause Nexus to run in FT mode with ZooKeeper")
  (opts, args) = parser.parse_args()
  opts.ft = int(opts.ft)
  if len(args) != 2:
    parser.print_help()
    sys.exit(1)
  (action, cluster_name) = args
  if opts.identity_file == None and action in ['launch', 'login']:
    print >> stderr, ("ERROR: The -i or --identity-file argument is " +
                      "required for " + action)
    sys.exit(1)
  if os.getenv('AWS_ACCESS_KEY_ID') == None:
    print >> stderr, ("ERROR: The environment variable AWS_ACCESS_KEY_ID " +
                      "must be set")
    sys.exit(1)
  if os.getenv('AWS_SECRET_ACCESS_KEY') == None:
    print >> stderr, ("ERROR: The environment variable AWS_SECRET_ACCESS_KEY " +
                      "must be set")
    sys.exit(1)
  return (opts, action, cluster_name)


def get_or_make_group(conn, name):
  groups = conn.get_all_security_groups()
  group = [g for g in groups if g.name == name]
  if len(group) > 0:
    return group[0]
  else:
    print "Creating security group " + name
    return conn.create_security_group(name, "Nexus EC2 group")


def wait_for_instances(conn, reservation):
  instance_ids = [i.id for i in reservation.instances]
  while True:
    reservations = conn.get_all_instances(instance_ids)
    some_pending = False
    for res in reservations:
      if len([i for i in res.instances if i.state == 'pending']) > 0:
        some_pending = True
        break
    if some_pending:
      time.sleep(5)
    else:
      for i in reservation.instances:
        i.update()
      return


def launch_cluster(conn, opts, cluster_name):
  zoo_res = None
  print "Setting up security groups..."
  master_group = get_or_make_group(conn, cluster_name + "-master")
  slave_group = get_or_make_group(conn, cluster_name + "-slaves")
  zoo_group = get_or_make_group(conn, cluster_name + "-zoo")
  if master_group.rules == []: # Group was just now created
    master_group.authorize(src_group=master_group)
    master_group.authorize(src_group=slave_group)
    master_group.authorize(src_group=zoo_group)
    master_group.authorize('tcp', 22, 22, '0.0.0.0/0')
    master_group.authorize('tcp', 8080, 8081, '0.0.0.0/0')
    master_group.authorize('tcp', 50030, 50030, '0.0.0.0/0')
    master_group.authorize('tcp', 50070, 50070, '0.0.0.0/0')
  if slave_group.rules == []: # Group was just now created
    slave_group.authorize(src_group=master_group)
    slave_group.authorize(src_group=slave_group)
    slave_group.authorize(src_group=zoo_group)
    slave_group.authorize('tcp', 22, 22, '0.0.0.0/0')
    slave_group.authorize('tcp', 8080, 8081, '0.0.0.0/0')
    slave_group.authorize('tcp', 50060, 50060, '0.0.0.0/0')
    slave_group.authorize('tcp', 50075, 50075, '0.0.0.0/0')
  if zoo_group.rules == []: # Group was just now created
    zoo_group.authorize(src_group=master_group)
    zoo_group.authorize(src_group=slave_group)
    zoo_group.authorize(src_group=zoo_group)
    zoo_group.authorize('tcp', 22, 22, '0.0.0.0/0')
    zoo_group.authorize('tcp', 2181, 2181, '0.0.0.0/0')
  print "Checking for running cluster..."
  reservations = conn.get_all_instances()
  for res in reservations:
    group_names = [g.id for g in res.groups]
    if master_group.name in group_names or slave_group.name in group_names or zoo_group.name in group_names:
      active = [i for i in res.instances if i.state in ['pending', 'running']]
      if len(active) > 0:
        print >> stderr, ("ERROR: There are already instances running in " +
            "group %s or %s" % (master_group.name, slave_group.name))
        sys.exit(1)
  print "Launching instances..."
  try:
    image = conn.get_all_images(image_ids=[opts.ami])[0]
  except:
    print >> stderr, "Could not find AMI " + opts.ami
    sys.exit(1)
  slave_res = image.run(key_name = opts.key_pair,
                        security_groups = [slave_group],
                        instance_type = opts.instance_type,
                        placement = opts.zone,
                        min_count = opts.slaves,
                        max_count = opts.slaves)
  print "Launched slaves, regid = " + slave_res.id
  master_type = opts.master_instance_type
  if master_type == "":
    master_type = opts.instance_type
  master_res = image.run(key_name = opts.key_pair,
                         security_groups = [master_group],
                         instance_type = master_type,
                         placement = opts.zone,
                         min_count = opts.ft,
                         max_count = opts.ft)
  print "Launched master, regid = " + master_res.id
  if opts.ft > 1:
    zoo_res = image.run(key_name = opts.key_pair,
                        security_groups = [zoo_group],
                        instance_type = opts.instance_type,
                        placement = opts.zone)
    print "Launched zoo, regid = " + zoo_res.id
  return (master_res, slave_res, zoo_res)


def get_existing_cluster(conn, opts, cluster_name):
  print "Searching for existing cluster " + cluster_name + "..."
  reservations = conn.get_all_instances()
  master_res = None
  slave_res = None
  zoo_res = None
  for res in reservations:
    active = [i for i in res.instances if i.state in ['pending', 'running']]
    if len(active) > 0:
      group_names = [g.id for g in res.groups]
      if group_names == [cluster_name + "-master"]:
        master_res = res
      elif group_names == [cluster_name + "-slaves"]:
        slave_res = res
      elif group_names == [cluster_name + "-zoo"]:
        zoo_res = res
  if master_res != None and slave_res != None:
    print "Found master regid: " + master_res.id
    print "Found slave regid: " + slave_res.id
    if zoo_res != None:
      print "Found slave regid: " + zoo_res.id
    return (master_res, slave_res, zoo_res)
  else:
    if master_res == None and slave_res != None:
      print "ERROR: Could not find master in group " + cluster_name + "-master"
    elif master_res != None and slave_res == None:
      print "ERROR: Could not find slaves in group " + cluster_name + "-slaves"
    else:
      print "ERROR: Could not find any existing cluster"
    sys.exit(1)


def deploy_files(conn, root_dir, instance, opts, master_res, slave_res, zoo_res):
  # TODO: Speed up deployment by creating a temp directory with the
  # template-transformed files and then rsyncing it

  master = master_res.instances[0].public_dns_name

  template_vars = {
    "master" : '\n'.join([i.public_dns_name for i in master_res.instances]),
    "slave_list" : '\n'.join([i.public_dns_name for i in slave_res.instances])
  }

  if opts.ft > 1:
    zoo = zoo_res.instances[0].public_dns_name
    template_vars[ "zoo" ] = '\n'.join([i.public_dns_name for i in zoo_res.instances])

  for path, dirs, files in os.walk(root_dir):
    dest_dir = os.path.join('/', path[len(root_dir):])
    if len(files) > 0: # Only mkdir for low-level directories since we use -p
      ssh(master, opts, 'mkdir -p "%s"' % dest_dir)
    for filename in files:
      if filename[0] not in '#.~' and filename[-1] != '~':
        dest_file = os.path.join(dest_dir, filename)
        print dest_file
        with open(os.path.join(path, filename)) as file:
          text = file.read()
          for key in template_vars:
            text = text.replace("{{" + key + "}}", template_vars[key])
          temp_file = NamedTemporaryFile()
          temp_file.write(text)
          temp_file.flush()
          scp(master, opts, temp_file.name, dest_file)
          temp_file.close()


def scp(host, opts, local_file, dest_file):
  subprocess.check_call(
      "scp -q -o StrictHostKeyChecking=no -i %s '%s' 'root@%s:%s'" %
      (opts.identity_file, local_file, host, dest_file), shell=True)


def ssh(host, opts, command):
  subprocess.check_call(
      "ssh -t -o StrictHostKeyChecking=no -i %s root@%s '%s'" %
      (opts.identity_file, host, command), shell=True)


def main():
  (opts, action, cluster_name) = parse_args()
  print "Connecting to EC2..."
  conn = boto.connect_ec2()
  if action == "launch":
    if opts.resume:
      (master_res, slave_res, zoo_res) = get_existing_cluster(conn, opts, cluster_name)
    else:
      (master_res, slave_res, zoo_res) = launch_cluster(conn, opts, cluster_name)
      print "Waiting for instances to start up..."
      time.sleep(5)
      wait_for_instances(conn, master_res)
      wait_for_instances(conn, slave_res)
      if opts.ft > 1:
        wait_for_instances(conn, zoo_res)
      print "Waiting 20 more seconds..."
      time.sleep(20)
    print "Deploying files to master..."
    deploy_files(conn, "deploy." + opts.os, master_res.instances[0],
        opts, master_res, slave_res, zoo_res)
    print "Copying SSH key %s to master..." % opts.identity_file
    master = master_res.instances[0].public_dns_name
    ssh(master, opts, 'mkdir -p /root/.ssh')
    scp(master, opts, opts.identity_file, '/root/.ssh/id_rsa')
    print "Running setup on master..."
    ssh(master, opts, "chmod u+x nexus-ec2/setup")
    ssh(master, opts, "nexus-ec2/setup %s %s %s" % (opts.os, opts.download, opts.branch))
    print "Done!"
  elif action == "shutdown":
    response = raw_input("Are you sure you want to shut down the cluster " +
        cluster_name + "? (y/N) ")
    if response == "y":
      (master_res, slave_res, zoo_res) = get_existing_cluster(conn, opts, cluster_name)
      print "Shutting down master..."
      master_res.stop_all()
      print "Shutting down slaves..."
      slave_res.stop_all()
      if opts.ft > 1:
        print "Shutting down zoo..."
        zoo_res.stop_all()
  elif action == "login":
    (master_res, slave_res, zoo_res) = get_existing_cluster(conn, opts, cluster_name)
    master = master_res.instances[0].public_dns_name
    print "Logging into master " + master + "..."
    proxy_opt = ""
    if opts.proxy_port != None:
      proxy_opt = "-D " + opts.proxy_port
    subprocess.check_call("ssh -o StrictHostKeyChecking=no -i %s %s root@%s" %
        (opts.identity_file, proxy_opt, master), shell=True)


if __name__ == "__main__":
  logging.basicConfig()
  main()
