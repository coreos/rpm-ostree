FROM centos
ADD centos-atomic-7.repo /etc/yum.repos.d/centos-atomic-7.repo
ADD local.repo /etc/yum.repos.d/local.repo
ADD repo /var/tmp/repo
ADD pkgblob.tar /var/tmp/centos
ADD test-centos.json /var/tmp/test/test-centos.json
ADD test-centos.repo /var/tmp/test/test-centos.repo
ADD container-build.sh /usr/bin/container-build
RUN /usr/bin/container-build
