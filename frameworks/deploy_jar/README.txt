haproxy load balancer + Apache web server Mesos framework Readme
----------------------------------------------------------------

First you will need to be able to run apache on the slave nodes in your cluster.

In ubuntu, you can run 'sudo apt-get install apache2'

Then 'sudo /etc/init.d/apache2 restart'


You need to have haproxy installed, currently it is assumed (in haproxy+apache.py) to be in /root/haproxy-1.3.20/haproxy.

installation instructions are here: http://www.lastengine.com/99/installing-haproxy-load-balancing-for-http-and-https/

