FROM registry.fedoraproject.org/fedora:36

# Make sure this matches what you use in the Makefile when building manually
ARG WORKDIR=/project
WORKDIR $WORKDIR

RUN ./ci/installdeps.sh; \
    dnf install -y cpio; \
    rm -rf /var/cache/dnf
