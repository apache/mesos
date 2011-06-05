Nexus MPICH2 framework readme
--------------------------------------------

Table of Contents:
1) Installing MPICH2
2) Running the Nexus MPICH2 framework

=====================
1) INSTALLING MPICH2:
=====================
This framework was developed using MPICH2 on Linux.

You can install MPICH2 from scratch. You can get MPICH2 as well as installation directions here: http://www.mcs.anl.gov/research/projects/mpich2/

I (Andy) installed MPICH2 using apt-get, but in Ubuntu, I had to add the Debian package mirror to my /etc/apt/sources.list file manuall.

I.e. I added 'deb http://ftp.de.debian.org/debian sid main' to the end of the file.

I also had to muck with keys since 9.04 (Jaunty) Ubuntu is using secure apt, so I did:

gpg --recv-keys 4D270D06F42584E6
gpg --export 4D270D06F42584E6 | apt-key add -

though, theoretically, the following should suffice, it did not for me:

apt-get install debian-keyring debian-archive-keyring
apt-key update

=====================================
2) RUNNING THE NEXUS MPICH2 FRAMEWORK
=====================================

1. Start a Nexus master and slaves see the NEXUS_HOME/QUICKSTART.txt for help
with this.
2. In the NEXUS_HOME/frameworks/mpi directory run the nmpiexec script. Pass the
-h flag to see help options.
   Example: ./nmpiexec -m 104857600 1@127.0.1.1:59608 hostname
