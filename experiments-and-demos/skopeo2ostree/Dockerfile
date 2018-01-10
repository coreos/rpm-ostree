# This generates a Docker/OCI image that looks a lot like an
# Atomic Host ostree (in terms of filesystem layout).
#
# See https://github.com/ostreedev/ostree-releng-scripts/pull/14
# and in particular its `skopeo2ostree` tool.
#
# One could in theory actually combine the whole chain of
# `docker build` on this Dockerfile with `skopeo2ostree`
# and then `rpm-ostree ex commit2jigdo`.
FROM fedora:26
RUN yum -y install kernel{,-core,-modules} dracut-config-generic @core \
                   lvm2 cryptsetup audit policycoreutils \
                   rpm-ostree ostree{,-grub2} nss-altfiles \
    && yum -y remove cronie plymouth \
    && semodule -nB \
    && yum clean all \
    && rpm -evh dnf libdnf dnf-{yum,conf} dnf-plugins-core \
              libcomps deltarpm rpm-plugin-systemd-inhibit python3-{dnf,dnf-plugins-core,hawkey,gpg,libcomps,librepo} \
    && rm -rf /var/lib/dnf \
    && rpm -qa|sort
# https://bugzilla.redhat.com/show_bug.cgi?id=1265295
RUN echo 'Storage=persistent' >> /etc/systemd/journald.conf
# Undo the container base changes
RUN systemctl unmask systemd-remount-fs.service dev-hugepages.mount sys-fs-fuse-connections.mount systemd-logind.service getty.target console-getty.service
# FIXME - not starting for some reason
RUN systemctl mask firewalld
# https://ostree.readthedocs.io/en/latest/manual/adapting-existing/
RUN for x in srv home media mnt opt; do mv /${x} /var/${x} && ln -sr /var/${x} /${x}; done \
    && rm /root -rf && ln -sr /var/roothome /root \
    && rm /usr/local -rf && ln -sr /var/usrlocal /usr/local \
    && mkdir -p /sysroot && ln -sr /sysroot/ostree /ostree \
    && rm /tmp -rf && ln -sr /sysroot/tmp /tmp \
&& rm -rf /run/*
