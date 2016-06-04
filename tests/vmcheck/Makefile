all:
	echo "Targets: buildimg build install"

buildimg:
	sudo docker build -t rpm-ostree-builder -f Dockerfile.builder .

runbuilder=sudo /usr/bin/docker run --privileged -ti -v /var/home:/home -v /etc:/host/etc -v /usr:/host/usr rpm-ostree-builder

build:
	$(runbuilder) /bin/sh -c 'if ! test -f configure; then env NOCONFIGURE=1 ./autogen.sh; fi'
	$(runbuilder) /bin/sh -c 'if ! test -f Makefile; then ./configure --prefix=/usr --libdir=/usr/lib64; fi'
	$(runbuilder) /bin/sh -c make -j 4

install: build
	$(runbuilder) sudo /usr/local/bin/host-install


