Jenkins on Mesos
----------------

This Jenkins plugin allows Jenkins to dynamically launch Jenkins slaves on a
Mesos cluster depending on the workload!

### Building the plugin ###

First configure the jenkins target:

        $ ./bootstrap
        $ mkdir -p build && cd build
        $ ../configure

Now build the plugin:

        $ cd jenkins
        $ make jenkins

This should build the Mesos plugin (mesos.hpi) in the 'target' folder.


### Installing the plugin ###

Go to 'Manage Plugins' page in Jenkins Web UI and manually upload and
install the plugin.

Alternatively, you can just copy the plugin to your Jenkins plugins directory
(this might need a restart of Jenkins).

        $ cp target/mesos.hpi ${JENKINS_HOME}/plugins


### Configuring the plugin ###

Now go to 'Configure' page in Jenkins. If the plugin is successfully installed
you should see an option to 'Add a new cloud' at the bottom of the page. Add
the 'Mesos Cloud' and give it the address (HOST:PORT) of a running Mesos master.
Click 'Save' for the plugin to connect to Mesos.

Login to the Mesos master's Web UI to verify that the plugin is registered as
'Jenkins Framework'.


### Configuring Jenkins jobs ###

Finally, just add 'mesos' label to the jobs that you want to be run on a
Jenkins slave launched on Mesos.

Thats it!


_Please email user@mesos.apache.org with questions!_
