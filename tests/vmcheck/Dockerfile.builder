FROM centos/tools
ADD atomic-centos-continuous.repo /etc/yum.repos.d/atomic-centos-continuous.repo
RUN yum -y install yum-plugin-priorities sudo && \
    yum -y install bash bzip2 coreutils cpio diffutils system-release findutils gawk gcc gcc-c++ \
      grep gzip info make patch redhat-rpm-config rpm-build sed shadow-utils tar unzip util-linux \
      which xz python gcc \
    && yum-builddep -y rpm-ostree
RUN groupadd -g 1000 vagrant && useradd -u 1000 -g vagrant -G wheel vagrant
RUN echo "%wheel	ALL=(ALL)	NOPASSWD: ALL" >> /etc/sudoers
ADD host-install.sh /usr/local/bin/host-install
LABEL RUN "/usr/bin/docker run --privileged -ti -v /var/home:/home -v /etc:/host/etc -v /usr:/host/usr \${IMAGE}"
USER vagrant
WORKDIR /home/vagrant/sync
