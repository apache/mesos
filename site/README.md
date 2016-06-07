# Apache Mesos website generator
This will generate the site available at http://mesos.apache.org. We use docker
to simplify our generation and development workflows.


## Build the docker image

Under the root of your local Mesos git repository, run the following command
to build the docker image.

```
docker build -t mesos/website site
```

## Generating the site

Before we start the docker container, we need to generate the help endpoint
documentation. Running `make` is needed to generate the latest version of
the master and agent binaries.

```
make -jN
../support/generate-endpoint-help.py
```

To build and run the website inside a docker container please substitute
*<path-to-mesos-source>* with the actual location of Mesos source code tree
and *<path-to-mesos-website-svn-source>* with the actual location of Mesos
website svn source code tree in the command below and run it.

```
sudo docker run -it --rm \
    -p 4567:4567 \
    -p 35729:35729 \
    -v <path-to-mesos-source>:/mesos \
    -v <path-to-mesos-website-svn-source>/publish:/mesos/site/publish \
    mesos/website
```

If you don't intend to update the svn source code of the Mesos website, you
could run the command below under the root of Mesos source code instead.

```
sudo docker run -it --rm \
    -p 4567:4567 \
    -p 35729:35729 \
    -v `pwd`:/mesos \
    mesos/website
```

This will start a container, generate the website from your local Mesos git
repository, and make it available:
 - On Linux, the site will be available at http://localhost:4567.
 - On OS X, run `docker-machine ls` to find the IP address of your
   boot-to-docker VM; the site will be available at that IP, port 4567.

If you are running the container on a remote machine and need to tunnel it to
localhost, you can run the following command to make the site available locally:

```
ssh -NT -L 4567:localhost:4567 -L 35729:localhost:35729 <remote-machine>
```

The generation includes `doxygen` and `javadoc` as well. We could check out them
under `/api/latest/c++/index.html` and `/api/latest/java/index.html` endpoints
once the generation finishs.

## Development

Any changes to the `site/source` directory will cause middleman to reload and
regenerate the website, so you can just edit, save, refresh. When you are done
with the webserver, hit Ctrl-C in the docker terminal to kill the middleman
webserver, clean up generation documents under the `site/source` directory and
destroy/remove the container.

## Publishing the Site

Because we mount the `publish` folder under Mesos website svn source code into
the docker container we launched above, all website contents would be ready
under the `publish` folder when the generation finishs.

The website uses svnpubsub. The `publish` folder contains the websites content
and when committed to the svn repository it will be automatically deployed to
the live site.