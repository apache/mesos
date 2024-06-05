# Mesos docker files for Ubuntu 20.04, Centos 7, Ubuntu 18.04

We use these dockerfiles to update the images that we have hosted on docker hub
which our workflows, like the buildbot, pulls from.

# Building images

To build the dockerfiles inside this directory, run the following command:

```bash
docker build -t mesos/mesos-build:centos-7 -f centos-7.dockerfile --platform=linux/amd64 .
docker build -t mesos-build:ubuntu-20.04 -f ubuntu-20.04.dockerfile --platform=linux/amd64 .
docker build -t mesos-build:ubuntu-20.04-arm -f ubuntu-20.04-arm.dockerfile --platform=linux/arm64 .
```

# Pushing images
Ensure you have your tag for the image as `mesos/mesos-build:<OS version>` as shown above.

```bash
docker push mesos/mesos-build:centos-7
docker push mesos/mesos-build:ubuntu-20.04
docker push mesos/mesos-build:ubuntu-20.04-arm
```

You need to be an authenticated user who is authorized in order to perform docker push successfully
