#!/bin/bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
set -xeuo pipefail

# Prow jobs don't support adding emptydir today
export COSA_SKIP_OVERLAY=1
# And suppress depcheck since we didn't install via RPM
export COSA_SUPPRESS_DEPCHECK=1

ls -al /usr/bin/rpm-ostree
rpm-ostree --version
cd $(mktemp -d)

# Let's force XDG vars to make sure we got permissions
# when writing to the directories
# https://specifications.freedesktop.org/basedir-spec/latest/
mkdir -p "$PWD/.state" "$PWD/.cache" "$PWD/.config"
chmod 700 "$PWD/.state" "$PWD/.config"
chmod 755 "$PWD/.cache"
export XDG_STATE_HOME="$PWD/.state"
export XDG_CACHE_HOME="$PWD/.cache"
export XDG_CONFIG_HOME="$PWD/.config"

cosa init https://github.com/coreos/fedora-coreos-config/
cp /cosa/component-rpms/*.rpm overrides/rpm
cosa fetch
cosa build
# Cosa no longer creates  QEMU image by default see:
# https://github.com/coreos/coreos-assembler/issues/4304
cosa buildextend-qemu
cosa kola run 'ext.rpm-ostree.*'
