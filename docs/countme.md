---
nav_order: 4
---

# DNF Count Me support
{: .no_toc }

1. TOC
{:toc}

Classic DNF based operating systems can use the [DNF Count Me feature][countme]
to anonymously report how long a system has been running without impacting the
user privacy. This is implemented as an additional `countme` variable added to
requests made to fetch RPM repository metadata. On those systems, this value is
added randomly to requests made automatically via the `dnf-makecache.timer` or
via explicit calls to `dnf update` or `dnf install`.

However, this does not work for `rpm-ostree` based systems as in the default
case (no package overlayed on top of the base commit), `rpm-ostree` will not
fetch any RPM repository metadata at all.

Thus `rpm-ostree` includes a distinct timer (`rpm-ostree-countme.timer`),
triggered weekly, that implement the DNF Count Me functionality in a
standalone way.

## Disabling DNF Count Me on a system

To disable this feature, you need to stop the `rpm-ostree-countme.timer` and
mask the corresponding unit as a precaution:

```
$ systemctl mask --now rpm-ostree-countme.timer
```

[countme]: https://fedoraproject.org/wiki/Changes/DNF_Better_Counting
