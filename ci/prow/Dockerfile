FROM registry.svc.ci.openshift.org/coreos/cosa-buildroot:latest as builder
WORKDIR /src
COPY . .
RUN ./ci/build.sh && make install DESTDIR=/cosa/component-install
# Uncomment this to fake a build to test the code below
# RUN mkdir -p /cosa/component-install/usr/bin && echo foo > /cosa/component-install/usr/bin/foo

FROM registry.svc.ci.openshift.org/coreos/coreos-assembler:latest
WORKDIR /srv
# Install our built binaries as overrides for the target build
COPY --from=builder /cosa/component-install /srv/overrides/rootfs/
COPY --from=builder /src/ci/prow/fcos-e2e.sh /usr/bin/fcos-e2e
