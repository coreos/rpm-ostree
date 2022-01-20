---
parent: Experimental features
nav_order: 1
---

# override replace --experimental

To improve the experience when overriding packages directly from a RPM
repository, rpm-ostree has experimental support for fetching RPMs and
overriding them. Full support for this feature is tracked in [#1265].

## Overriding packages with the versions from a given repo

For this example, we will replace the kernel shipped as part of the base image
in Fedora CoreOS with the one provided by the [Vanilla Kernel Repositories].
This is something usefull for example if you want to try reproduciong a bug on
a recent vanilla Linux kernel to make sure this is not something coming from a
Fedora specific patch.

First, we setup the custom RPM repository:

```
$ curl -s https://repos.fedorapeople.org/repos/thl/kernel-vanilla.repo | sudo tee /etc/yum.repos.d/kernel-vanilla.repo
```

Then we manually enable the `kernel-vanilla-mainline` repo by editing
`/etc/yum.repos.d/kernel-vanilla.repo`:

```
$ $EDITOR /etc/yum.repos.d/kernel-vanilla.repo
[kernel-vanilla-mainline]
name=Linux vanilla kernels from mainline series
baseurl=http://repos.fedorapeople.org/repos/thl/kernel-vanilla-mainline/fedora-$releasever/$basearch/
enabled=1
skip_if_unavailable=1
gpgcheck=1
gpgkey=https://repos.fedorapeople.org/repos/thl/RPM-GPG-KEY-knurd-kernel-vanilla
metadata_expire=2h

...
```

Then we ask rpm-ostree to override the current kernel package with the ones from the repository:

```
$ rpm-ostree override replace --experimental --freeze --from repo=kernel-vanilla-mainline kernel kernel-core kernel-modules
Enabled rpm-md repositories: fedora-cisco-openh264 fedora-modular updates-modular updates fedora kernel-vanilla-mainline updates-archive
Importing rpm-md... done
rpm-md repo 'fedora-cisco-openh264' (cached); generated: 2021-09-21T18:07:30Z solvables: 4
rpm-md repo 'fedora-modular' (cached); generated: 2021-10-26T05:08:36Z solvables: 1283
rpm-md repo 'updates-modular' (cached); generated: 2022-01-20T03:00:41Z solvables: 1361
rpm-md repo 'updates' (cached); generated: 2022-01-19T01:42:17Z solvables: 14915
rpm-md repo 'fedora' (cached); generated: 2021-10-26T05:31:27Z solvables: 65732
rpm-md repo 'kernel-vanilla-mainline' (cached); generated: 2022-01-19T17:32:20Z solvables: 18
rpm-md repo 'updates-archive' (cached); generated: 2022-01-19T02:49:26Z solvables: 21161
Checking out tree a90b696... done
Enabled rpm-md repositories: fedora-cisco-openh264 fedora-modular updates-modular updates fedora kernel-vanilla-mainline updates-archive
Importing rpm-md... done
rpm-md repo 'fedora-cisco-openh264' (cached); generated: 2021-09-21T18:07:30Z solvables: 4
rpm-md repo 'fedora-modular' (cached); generated: 2021-10-26T05:08:36Z solvables: 1283
rpm-md repo 'updates-modular' (cached); generated: 2022-01-20T03:00:41Z solvables: 1371
rpm-md repo 'updates' (cached); generated: 2022-01-19T01:42:17Z solvables: 17285
rpm-md repo 'fedora' (cached); generated: 2021-10-26T05:31:27Z solvables: 65732
rpm-md repo 'kernel-vanilla-mainline' (cached); generated: 2022-01-19T17:32:20Z solvables: 18
rpm-md repo 'updates-archive' (cached); generated: 2022-01-19T02:49:26Z solvables: 21161
Resolving dependencies... done
Applying 5 overrides
Processing packages... done
Running pre scripts... done
Running post scripts... done
Running posttrans scripts... done
Writing rpmdb... done
Generating initramfs... done
Writing OSTree commit... done
Staging deployment... done
Upgraded:
  kernel 5.15.10-200.fc35 -> 5.17.0-0.rc0.20220118gitfe81ba137ebc.69.vanilla.1.fc35
  kernel-core 5.15.10-200.fc35 -> 5.17.0-0.rc0.20220118gitfe81ba137ebc.69.vanilla.1.fc35
  kernel-modules 5.15.10-200.fc35 -> 5.17.0-0.rc0.20220118gitfe81ba137ebc.69.vanilla.1.fc35
Use "rpm-ostree override reset" to undo overrides
Run "systemctl reboot" to start a reboot

$ rpm-ostree status
State: idle
...
Deployments:
  fedora:fedora/x86_64/coreos/next
                   Version: 35.20220116.1.0 (2022-01-17T16:50:49Z)
                BaseCommit: a90b69600554ca68ab5304c9fe9be53488970b5cefa15cb3d46942e157796c60
                    Commit: ed8e9dc881dba12840a4a85549e37c7e869ee4780010d5bfebeaf7960794ae91
              GPGSignature: Valid signature by 787EA6AE1147EEE56C40B30CDB4639719867C58F
                      Diff: 3 upgraded
      ReplacedBasePackages: kernel kernel-modules kernel-core 5.15.10-200.fc35 -> 5.17.0-0.rc0.20220118gitfe81ba137ebc.69.vanilla.1.fc35
...
```

## Rolling back

You can rollback to the previous deployment:

```
$ rpm-ostree rollback
```

or reset the overrides with:

```
$ rpm-ostree override reset kernel kernel-core kernel-modules
```

or reset all overrides:

```
$ rpm-ostree override reset --all
```

[#1265]: https://github.com/coreos/rpm-ostree/issues/1265
[Vanilla Kernel Repositories]: https://fedoraproject.org/wiki/Kernel_Vanilla_Repositories
