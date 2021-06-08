---
has_children: true
has_toc: false
nav_order: 7
---

# Contributing
{: .no_toc }

1. [Hacking on rpm-ostree](HACKING.md)
1. [Repository structure](repo_structure.md)
1. [Releasing rpm-ostree](RELEASE.md)

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
[contributing]: https://github.com/ostreedev/ostree/blob/main/docs/CONTRIBUTING.md
