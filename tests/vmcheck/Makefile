all:
	echo "Targets: buildimg build install"

BUILDER_ARGS = sudo /usr/bin/docker run -ti --privileged -v /var/home:/home
BUILDER_IMG = rpm-ostree-builder

BUILDER_RUN = $(BUILDER_ARGS) $(BUILDER_IMG)

buildimg:
	sudo docker build -t $(BUILDER_IMG) -f Dockerfile.builder .

build:
	$(BUILDER_RUN) ./autogen.sh --prefix=/usr --libdir=/usr/lib64
	$(BUILDER_RUN) make -j 4

install: build
	sudo sh checkout.sh
	$(BUILDER_ARGS) -v /etc:/host/etc \
	                -v /ostree/repo/tmp/vmcheck.ro/usr:/host/usr \
	                $(BUILDER_IMG) sudo make install DESTDIR=/host
	sudo VERSION=$(VERSION) sh commit_and_deploy.sh
