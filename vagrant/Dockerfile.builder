FROM centos/tools
ADD atomic-centos-continuous.repo /etc/yum.repos.d/atomic-centos-continuous.repo
RUN yum -y install yum-plugin-priorities sudo && \
    yum -y install bash bzip2 coreutils cpio diffutils system-release findutils gawk gcc gcc-c++ \
      grep gzip info make patch redhat-rpm-config rpm-build sed shadow-utils tar unzip util-linux \
      which xz python gcc \
    && yum-builddep -y rpm-ostree
LABEL RUN "/usr/bin/docker run --privileged -ti -v /var/roothome:/root -v /etc:/host/etc -v /usr:/host/usr \${IMAGE}"
WORKDIR /root/sync
