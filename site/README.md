# Apache Mesos website generator
This will generate the Mesos website locally. We use docker
to simplify our generation and development workflows.

## Generating the site

1. Compile Mesos following the [build instructions](http://mesos.apache.org/documentation/latest/building/).

2. Run this script to generate the endpoint help pages.
```
../support/generate-endpoint-help.py
```

3. Run this script to generate the website.

```
./site/mesos-website-dev.sh
```

This will start a container, generate the website from your local Mesos git
repository, and make it available. To view the site, go to: http://localhost:4567.

If you are running the container on a remote machine and need to tunnel it to
localhost, you can run the following command to make the site available locally:

```
ssh -NT -L 4567:localhost:4567 -L 35729:localhost:35729 <remote-machine>
```

The generation includes `doxygen` and `javadoc` as well. We could check out them
under `/api/latest/c++/index.html` and `/api/latest/java/index.html` endpoints
once the generation finishes.

## Development

Any changes to the `site/source` directory will cause middleman to reload and
regenerate the website, so you can just edit, save, refresh. When you are done
with the webserver, hit Ctrl-C in the docker terminal to kill the middleman
webserver, clean up generation documents under the `site/source` directory and
destroy/remove the container.

## Publishing the Site

Developers are not expected to publish the website. There is a CI job on ASF
Jenkins (Mesos-Websitebot) that automatically publishes the website when there
are changes detected in the Mesos repository. See
`support/jenkins/websitebot.sh` for details.
