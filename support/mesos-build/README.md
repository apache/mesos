# Mesos docker files for Ubuntu 20.04, Centos 7, Ubuntu 18.04

We use these dockerfiles to update the images that we have hosted on docker hub
which our workflows, like the buildbot, pulls from.

# Building images

To build a dockerfile inside this directory, run the following command:
```bash
$ docker build -t <tag_name> -f <path_to_file> .
```

If you intend on uploading this image to dockerhub mesos/mesos-build:
For Ubuntu 20.04, please run docker build with the `--platform linux/amd64` flag.

# Pushing images
Ensure you have your tag for the image as `mesos/mesos-build:<OS version>`
Run: `docker push mesos/mesos-build:<OS version>`
You need to be an authenticated user who is authorized in order to perform docker push successfully