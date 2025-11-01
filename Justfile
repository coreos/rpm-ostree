# The default entrypoint to working on this project.
# Commands here typically wrap e.g. `podman build` or
# other tools which might launch local virtual machines.
#
# See also `Makefile`. Commands which end in `-local`
# skip containerization (and typically just proxy `make`).
#
# Rules written here are *often* used by the Github Action flows,
# and should support being configurable where that makes sense (e.g.
# the `build` rule supports being provided a base image).

# --------------------------------------------------------------------

# Build the container image from current sources.
# Note commonly you might want to override the base image via e.g.
# `just build --build-arg=base=quay.io/fedora/fedora-bootc:42`
build *ARGS:
    podman build --jobs=4 -t localhost/rpm-ostree {{ARGS}} .

# Perform validation (build, linting) in a container build environment
validate:
    podman build --jobs=4 --target validate .

# Directly run validation using host tools
validate-local:
    make check

# Build the integration test container image
build-integration:
    podman build --jobs=4 --target integration -t localhost/rpm-ostree-integration .

# Run container integration tests
test-container-integration: build-integration
    podman run --rm localhost/rpm-ostree-integration rpm-ostree-test-container.sh

# Build and run a shell in the build environment for debugging
shell:
    podman build --jobs=4 --target build -t localhost/rpm-ostree-build .
    podman run --rm -it localhost/rpm-ostree-build /bin/bash

# Build RPMs in a container
rpm *ARGS:
    podman build --jobs=4 --ignorefile packaging/.dockerignore -f packaging/Containerfile {{ARGS}} -t localhost/rpm-ostree-rpm .
    podman create --name rpm-ostree-rpm-tmp localhost/rpm-ostree-rpm
    podman cp rpm-ostree-rpm-tmp:/ .
    podman rm rpm-ostree-rpm-tmp
