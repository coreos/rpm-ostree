FROM registry.svc.ci.openshift.org/coreos/cosa-buildroot:latest
WORKDIR /src
COPY . .
USER root
RUN env SKIP_INSTALLDEPS=1 ./ci/build-check.sh
