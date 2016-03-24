# Example invocation:
# docker run --privileged --net=host -v /srv/work/centos-atomic-host:/srv --workdir /srv/ --rm -ti cgwalters/rpm-ostree compose tree --repo=repo --proxy=http://127.0.0.1:8123 centos-atomic-host.json
FROM fedora
RUN yum -y update && yum -y install rpm-ostree && yum clean all
VOLUME /srv/rpm-ostree
ENTRYPOINT ["rpm-ostree"]
