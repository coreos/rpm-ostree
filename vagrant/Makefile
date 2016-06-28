all:
	echo "Targets: buildimg build install"

BUILDER_ARGS = sudo /usr/bin/docker run --rm -ti --privileged -v /var/roothome:/root
BUILDER_IMG = rpm-ostree-builder

BUILDER_RUN = $(BUILDER_ARGS) $(BUILDER_IMG)

buildimg:
	sudo docker build -t $(BUILDER_IMG) -f Dockerfile.builder .

build:
	if ! test -f ../configure; then \
	  $(BUILDER_RUN) env NOCONFIGURE=1 ./autogen.sh; \
	fi
	if ! test -f ../Makefile; then \
	  $(BUILDER_RUN) ./configure --prefix=/usr --libdir=/usr/lib64; \
	fi
	$(BUILDER_RUN) sh -c make -j 4

install: build
	sudo sh checkout.sh
	$(BUILDER_ARGS) -v /etc:/host/etc \
	                -v /ostree/repo/tmp/vmcheck.ro/usr:/host/usr \
	                $(BUILDER_IMG) sudo make install DESTDIR=/host
	sudo VERSION=$(VERSION) sh commit_and_deploy.sh

ofsinstall: build
	ostree admin unlock || :
	$(BUILDER_ARGS) -v /etc:/host/etc \
	                -v /usr:/host/usr \
	                $(BUILDER_IMG) sudo make install DESTDIR=/host
