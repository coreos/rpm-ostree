all:
	echo "Targets: buildimg build install ofsinstall check clean"

BUILDER_ARGS = /usr/bin/docker run --rm --privileged -v /var/roothome:/root
BUILDER_IMG = rpm-ostree-builder

BUILDER_RUN = $(BUILDER_ARGS) $(BUILDER_IMG)

.PHONY: dockerwait buildimg build install ofsinstall check clean

# necessary precaution in case we're operating on a freshly
# booted VM
dockerwait:
	@echo "Waiting for docker service to start..."; \
	timeout=10; \
	while [ $$timeout -gt 0 ]; do \
		if systemctl show -p SubState docker.service | grep -q =running; then \
			exit 0; \
		fi; \
		timeout=$$((timeout - 1)); \
		sleep 1; \
	done; \
	echo "Waited too long for docker service to start"; \
	exit 1

buildimg: dockerwait
	docker build -t $(BUILDER_IMG) -f Dockerfile.builder .

build: buildimg
	if ! test -f ../configure; then \
	  $(BUILDER_RUN) env NOCONFIGURE=1 ./autogen.sh; \
	fi
	if ! test -f ../Makefile; then \
	  $(BUILDER_RUN) env CFLAGS='$(CFLAGS)' \
	    ./configure --prefix=/usr --libdir=/usr/lib64; \
	fi
	$(BUILDER_RUN) make -j4 CFLAGS="$(CFLAGS)"

install: build
	sh checkout.sh
	$(BUILDER_ARGS) -v /etc:/host/etc \
	                -v /ostree/repo/tmp/vmcheck.ro/usr:/host/usr \
	                $(BUILDER_IMG) make install DESTDIR=/host
	VERSION=$(VERSION) sh commit_and_deploy.sh

ofsinstall: build
	ostree admin unlock || :
	$(BUILDER_ARGS) -v /etc:/host/etc \
	                -v /usr:/host/usr \
	                $(BUILDER_IMG) make install DESTDIR=/host

check: build
	$(BUILDER_RUN) make check

clean: buildimg
	$(BUILDER_RUN) make clean
