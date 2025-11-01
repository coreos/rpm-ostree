# Build this project from source and write the updated content
# (i.e. /usr/bin/rpm-ostree and related binaries) to a new derived container
# image. See the `Justfile` for an example
#
# Use e.g. --build-arg=base=quay.io/centos-bootc/centos-bootc:stream10 to target
# CentOS instead.

ARG base=quay.io/fedora/fedora-bootc:42

# This first image captures a snapshot of the source code,
# note all the exclusions in .dockerignore.
FROM scratch as src
COPY . /src

# This is basically a no-op now, but we could make any other final tweaks we want
# here.
FROM $base as base

# Fetch sccache
FROM base as sccache
ARG SCCACHE_VERSION=0.8.2
# Install sccache for compiler caching
RUN <<EORUN
set -xeuo pipefail
target=$(arch)-unknown-linux-musl
v=sccache-v${SCCACHE_VERSION}-${target}
curl -fSsL "https://github.com/mozilla/sccache/releases/download/v${SCCACHE_VERSION}/${v}.tar.gz" \
    | tar xz -C /usr/local/bin --strip-components=1 "${v}/sccache"
chmod +x /usr/local/bin/sccache
EORUN

# This image installs build deps, pulls in our source code, and installs updated
# rpm-ostree binaries in /out. The intention is that the target rootfs is extracted from /out
# back into a final stage (without the build deps etc) below.
FROM base as build
# This installs our package dependencies, and we want to cache it independently of the rest.
# Basically we don't want changing a .rs file to blow out the cache of packages. So we only
# copy files necessary for dependency installation.
COPY packaging /tmp/packaging
COPY ci/installdeps.sh ci/libbuild.sh /tmp/ci/
RUN <<EORUN
set -xeuo pipefail
. /usr/lib/os-release
case $ID in
  centos|rhel) dnf config-manager --set-enabled crb;;
  fedora) dnf -y install dnf-utils 'dnf5-command(builddep)';;
esac
# Handle version skew, upgrade core dependencies
dnf -y distro-sync ostree{,-libs} libmodulemd
# Install build requirements
cd /tmp && ./ci/installdeps.sh
rm /tmp/{packaging,ci} -rf
EORUN
COPY --from=sccache /usr/local/bin/* /usr/local/bin/
# Now copy the rest of the source
COPY --from=src /src /src
WORKDIR /src
# See https://www.reddit.com/r/rust/comments/126xeyx/exploring_the_problem_of_faster_cargo_docker/
# We aren't using the full recommendations there, just the simple bits.
# First step, ensure we have the crates downloaded
RUN --mount=type=cache,target=/src/target --mount=type=cache,target=/var/roothome cargo fetch
# Then this all runs without networking
RUN --mount=type=cache,target=/src/target --mount=type=cache,target=/var/roothome --mount=type=cache,target=/var/cache/sccache --network=none <<EORUN
set -xeuo pipefail
# Configure sccache for C/C++ and Rust compilation caching
export SCCACHE_DIR=/var/cache/sccache
export CC="sccache gcc"
export CXX="sccache g++"
export RUSTC_WRAPPER=sccache
sccache --show-stats || true
env NOCONFIGURE=1 ./autogen.sh
./configure --prefix=/usr --libdir=/usr/lib64 --sysconfdir=/etc
make -j $(nproc)
make install DESTDIR=/out
sccache --show-stats
EORUN

# This just does syntax checking and basic validation
FROM build as validate
RUN grep -vEe '^#' ci/packages-build-extra.txt | xargs dnf -y install
RUN --mount=type=cache,target=/src/target --mount=type=cache,target=/var/roothome --mount=type=cache,target=/var/cache/sccache --network=none <<EORUN
set -xeuo pipefail
# Use sccache in validate stage too
export SCCACHE_DIR=/var/cache/sccache
export CC="sccache gcc"
export CXX="sccache g++"
export RUSTC_WRAPPER=sccache
# Only gate on correctness and a few specific lints by default
cargo clippy -- -A clippy::all -D clippy::correctness -D clippy::suspicious -D clippy::disallowed-methods -Dunused_imports -Ddead_code
# Basic syntax checks
make check-local
EORUN

# The final image that derives from the original base and adds the release binaries
FROM base as final
# Create a layer that is our new binaries
COPY --from=build /out/ /
# Only in this containerfile, inject a file which signifies
# this comes from this development image. This can be used in
# tests to know we're doing upstream CI.
RUN touch /usr/lib/.rpm-ostree-dev-stamp

# Integration test build
FROM build as integration-build
RUN <<EORUN
set -xeuo pipefail
grep -vEe '^#' ci/packages-build-extra.txt | xargs dnf -y install
grep -vEe '^#' ci/integration-runtime.txt | xargs dnf -y install
EORUN
# Copy test scripts
COPY ci/test-container.sh /out/usr/bin/rpm-ostree-test-container.sh
# Copy test data if it exists
COPY --from=src /src/tests /usr/share/rpm-ostree/tests
RUN <<EORUN
set -xeuo pipefail
make -C tests/kolainst install DESTDIR=/out
EORUN

FROM final as integration
COPY ci/ /run/ci/
RUN grep -vEe '^#' /run/ci/integration-runtime.txt | xargs dnf -y install
COPY --from=integration-build /out/ /
FROM final
