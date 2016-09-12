# Example invocation:
# mkdir cache
# ostree --repo=repo-build init --mode=bare-user
# ostree --repo=repo init --mode=archive
# alias run-rpmostree=docker run --privileged --net=host -v /srv/work/centos-atomic-host:/srv --workdir /srv/ --rm -ti registry.centos.org/rpm-ostree'
# run-rpmostree rpm-ostree compose tree --repo=repo-build --cachedir=cache centos-atomic-host.json
FROM centos
ADD atomic7-testing.repo /etc/yum.repos.d/atomic7-testing.repo
RUN yum -y update && yum -y install rpm-ostree && yum clean all
