DIR := mesos-tidy
USER := mesos
IMAGE := ${USER}/${DIR}
DOCKERFILE := Dockerfile

# TODO(bbannier): Consider making this depend on the current
# dir instead of the image dir if we end up composing images.
BRANCH := `git rev-list -1 HEAD -- $(DIR)`

# Construct a name for the tagged image. We allow the image name
# to contain literal `:` and fall back to `_` in that case.
ifneq (,$(findstring $(FOO), :))
  DEL := _
else
  DEL := :
endif
TAGGED := $(IMAGE)$(DEL)$(BRANCH)

THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))

# FIXME(bbannier): add a NO_CACHE variable to force building without cache use.

all:
	@$(MAKE) -f $(THIS_MAKEFILE) pull || $(MAKE) -f $(THIS_MAKEFILE) publish

pull:
	@docker pull $(TAGGED)
	@docker pull $(IMAGE)

build:
	@(cd $(DIR) && docker build -t $(IMAGE) -f $(DOCKERFILE) .)
	@docker tag $(IMAGE) $(TAGGED)

publish: build
	@docker push $(IMAGE)
	@docker push $(TAGGED)
