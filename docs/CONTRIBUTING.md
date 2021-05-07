---
nav_order: 10
---

# Contributing
{: .no_toc }

1. TOC
{:toc}

## Submitting patches

Submit a pull request against [coreos/rpm-ostree][rpm-ostree].

Please look at `git log` and match the commit log style.

## Running the test suite

There is `make check` as well as `make vmcheck`. See also what the
[Jenkinsfile][jenkinsfile] file does.

## Coding style

See the [OSTree CONTRIBUTING][contributing] coding style.

[rpm-ostree]: https://github.com/coreos/rpm-ostree
[jenkinsfile]: https://github.com/coreos/rpm-ostree/blob/main/.cci.jenkinsfile
[contributing]: https://github.com/ostreedev/ostree/blob/master/docs/CONTRIBUTING.md
