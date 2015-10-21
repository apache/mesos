# mesos-website-container
Docker build scripts for generating mesos.apache.org from the local repo.

To build the docker image:

```
docker build -t mesos/website support/site-docker
```

To build and run the website inside a docker container please substitute *<path-to-mesos-source>* with the actual location of Mesos source code tree in the command below and run it.

```
sudo docker run -it --rm -p 4567:4567 -v <path-to-mesos-source>:/mesos mesos/website
```

This will start a container, generate the website from your local Mesos Git repository, and make it available:
 - On Linux, the site will be available at http://localhost:4567.
 - On OS X, run `docker-machine ls` to find the IP address of your boot-to-docker VM; the site will be available at that IP, port 4567.

Any changes to the `site/source` directory will cause middleman to reload and regenerate the website, so you can just edit, save, refresh.
When you are done with the webserver, hit Ctrl-C in the docker terminal to kill the middleman webserver and destroy/remove the container.
