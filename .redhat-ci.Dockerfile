FROM fedora:25

# We could use the upstream spec file here, but anyway for
# runtime reqs, we're at the mercy of whatever in the
# updates repo.

RUN dnf install -y @buildsys-build && \
    dnf install -y 'dnf-command(builddep)' && \
    dnf builddep -y rpm-ostree && \
    dnf install -y rpm-ostree && \
    rpm -e rpm-ostree

# These are test-only reqs

RUN dnf install -y \
        createrepo_c \
        clang \
        libubsan \
        sudo  \
        gnome-desktop-testing

# create an unprivileged user for testing
RUN adduser testuser
