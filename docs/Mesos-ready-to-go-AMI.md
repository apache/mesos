<font size="3em"><table><tr><td>The most recent functional version of the "ready to go" AMI:</td><td><font size="2em"><b>ami-8a38c8e3</b></font></td></tr></table></font>

There are two main ways to get a Mesos cluster running on EC2 quickly and easily. One way is via the Mesos [[EC2-Scripts]]. The main guts of the EC2 Scripts are the python program in <mesos-download-root-dir>/src/ec2/mesos_ec2.py which will start up a number of Amazon EC2 Instances (these images already contain a copy of mesos at /root/mesos), and then SSH into those machines, set up the configuration files on the slaves to talk to the master, and also set up HDFS, NFS, and more on those nodes!

The other main way to launch a Mesos cluster on EC2 is using the Mesos "Ready-to-go" AMI. We have set up this AMI to make taking Mesos for a spin as easy as launching a some instances in EC2. That is, we have pre-packaged an AMI with /etc/init.d/mesos-master and /etc/init.d/mesos-slave scripts that make running a Mesos master or slave on an instance of this AMI super easy!

<i><b>WARNING:</b> While this feature is in Alpha, this AMI has the public ssh keys of some Mesos developers in the .ssh/authorized_keys file for now which would be a security vulnerability if you use these AMIs and don't want those folks to have access to ssh into your instances! You can always remove these entries after you boot the instance, and even re-bundle the AMI if you plan on reusing this functionality.</i>

## Prerequisite: Have a functional EC2 account

Here are some high level instructions for getting started with Amazon EC2:

1. Set up you Amazon EC2 user account
1. Download the [[Amazon EC2 API tools|http://www.amazon.com/gp/redirect.html/ref=aws_rc_ec2tools?location=http://s3.amazonaws.com/ec2-downloads/ec2-api-tools.zip&token=A80325AA4DAB186C80828ED5138633E3F49160D9]] or install [[Elasticfox|http://aws.amazon.com/developertools/609?_encoding=UTF8&jiveRedirect=1]]
1. Set up your EC2 credentials and try out the tools by running: `ec2-describe-instances`
1. Learn about security groups (which are basically how you set up a firewall on your node) because you'll need to open some ports in order for your Mesos slaves and master to talk to each other (and also to view the webui of the Master)

## How to use the "ready to go" AMI to run a Mesos Master node

1. Run `ec2-run-instances <AMI-IDNUM>` -- see above for most recent AMI-IDNUM to use
1. SSH into that machine as root and run: `service mesos-master start`
1. Your master should be running now. You should be able to look at the mesos-master log file at /mnt/mesos-master.INFO. You should do that right now and look for the libprocess PID (the PID should look something like <Integer>@<ip-address|hostname>:<port-num>, e.g. `1@ip-10-126-43-201.ec2.internal:5050`) of the master so that you can pass it to the slaves you're going to start next! You should also now be able to view the Master webui, which by default is viewable at `http://<instance public DNS name>:8080`.

<b>Note:</b> We currently use the same AMI for running a master or running a slave, or both! The /etc/init.d/mesos-slave script will run automatically when the virtual machine boots up, and it will read /usr/local/mesos/conf/mesos.conf. If this file does not contain a line which looks like "url=<lib process PID of mesos master>" then the slave will not run successfully. Thus, the way to use the AMI as a master is simply to <i>not pass any user data to the AMI at bootup</i>, then SSH into the master and start the mesos-master daemon with a single command as described above.

## How to use the "ready to go" AMI to run a Mesos Slave node

1. Run the slave AMI and pass in [[user data|http://docs.amazonwebservices.com/AWSEC2/2007-03-01/DeveloperGuide/AESDG-chapter-instancedata.html]] containing the url of a running Mesos master: `ec2-run-instances <AMI-IDNUM> -d url=1@<master-ip or hostname>:<master port> #see above for most recent AMI-IDNUM to use`

## History of Mesos AMIs
The table below contains more details on the version history of this "ready to go" AMI, which is updated regularly, so check back often!

<table>
  <tr>
    <th>Date/Time</th><th>AMI ID</th><th>S3 bucket and/or URL</th><th>Description and Notes</th><th>Bugs/Issues</th>
  </tr>
  <tr>
    <td><b>2/20/11 15:00, Sun</b></td><td><b>ami-44ce3d2d</b></td><td>http://andyk-mesos-images.s3.amazonaws.com/mesos-slave-master-v3.1</td><td>Updated /etc/init.d/mesos-slave to export MESOS_PUBLIC_DNS=<magic that wgets the public dns name from ec2> env var before launching the slave daemon so now the externally accessible ULRs are reported to the master (and shown on the master's webui). Also updated the spark installation per Justin's instructions.</td><td></td>
  </tr>
  <tr>
    <td><b>2/4/11 18:12, Fri</b></td><td><b>ami-967b8bff</b></td><td>http://andyk-mesos-images.s3.amazonaws.com/mesos-slave-master-v6</td><td>Fixed <i>~/.deploylib_tags</i> bug in last 2 AMIs. <b>DON'T USE. Slave webui crashes.</b></td><td>We are seeing the following error when trying to connect to the webui for slaves:Error 500: Internal Server Error. Sorry, the requested URL http://ec2-174-129-58-11.compute-1.amazonaws.com:8080/framework/201102090027-0-0000 caused an error:Unhandled exception</td>
  </tr>
  <tr>
    <td><b>2/4/11 14:43, Fri</b></td><td><b>ami-767a8a1f</b></td><td>http://andyk-mesos-images.s3.amazonaws.com/mesos-slave-master-v5</td><td>Fixed ~/.tags bug in last AMI</td><td>The file is actually called .deploylib_tags, so this didn't fix the bug afterall.</td>
  </tr>
  <tr>
    <td><b>2/4/11, Fri.</b></td><td><b>ami-b87383d1</b></td><td>http://mesos-slave-master-v4.s3.amazonaws.com/</td><td>Andy rolled a new AMI with mesos Event History functionality installed and enabled by default. Check out the new line in the config file at /usr/local/mesos/conf/mesos.conf which says "event_history_sqlite=1". Also check out the two new files (one txt and one sqlite3) storing task and framework history events: /mnt/event_history_db.sqlite3 and /mnt/event_history_log.txt</td><td>The ~/.tags director(/file?) was left on the image, needs to be removed by EC2Instance.bundleNewAMI() before ec2-bundle-volume is called.</td>
  </tr>
  <tr>
    <td><b>1/30/11, Sat.</b></td><td><b>ami-8a38c8e3</b></td><td>http://andyk-mesos-images.s3.amazonaws.com/mesos-slave-master-v3</td><td>Andy and Michael created a new AMI using the shiney new deploylib functionality!</td><td></td>
  </tr>
  <tr>
    <td><b>1/29/11, Sat.</b><td><b>ami-6a37c703</b></td></td><td>http://andyk-mesos-images.s3.amazonaws.com/mesos-slave-master-v2</td><td>This image has nginx added (which was set up by Justin Ma) and the most recent version of Mesos (using the radlab-demo branch)</td><td><i>DON'T USE THIS</i>: andyk forgot to run `make install`</td>
  </tr>
  <tr>
    <td><b>1/6/11, Thu.</b></td><td><b>ami-5a26d733</b></td><td>andyk-mesos-images/mesos-slave-master-v1<</td><td>Michael and Beth updated Mesos on that image. I added the /etc/init.d/mesos-master script as well as the /etc/default/mesos file that <b>ENABLE</b>s mesos. It doesn't run mesos-master at OS startup, but you should be able to run a master.</td><td></td>
  </tr>
  <tr>
    <td><b>Dec 2010.</b></td><td><b>ami-58798d31</b></td><td>andyk-mesos-images/mesos-slave-v6</td><td>Bundled, uploaded, and registered image in s3 bucket (see image.manifest.xml inside of that for the meta data about AMI parts)</td><td></td>
  </tr>
  <tr>
    <td><b>Fall 2010</b></td><td><b>ami-60798d09</b></td><td>mesos_images5</td><td>Created new AMI in s3 bucket (see mesos_images5/image.manifest.xml)</td><td></td>
  </tr>
  <tr>
</table>