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
This is something useful for example if you want to try reproducing a bug on
a recent vanilla Linux kernel to make sure this is not something coming from a
Fedora specific patch.

First, we setup the custom RPM repository, in this case for the
mainline-wo-mergew repo of the Vanilla Kernel repositories:

```
$ curl -s https://copr.fedorainfracloud.org/coprs/g/kernel-vanilla/mainline-wo-mergew/repo/fedora-rawhide/group_kernel-vanilla-mainline-wo-mergew-fedora-rawhide.repo | sudo tee /etc/yum.repos.d/kernel-vanilla-mainline-wo-mergew.repo
```

Then we ask rpm-ostree to override the current kernel package with the ones from the repository:

```
$ sudo rpm-ostree override replace --experimental --freeze --from repo='copr:copr.fedorainfracloud.org:group_kernel-vanilla:mainline-wo-mergew' kernel kernel-core kernel-modules kernel-modules-core kernel-modules-extra
Inactive base replacements:
  kernel-modules-core-6.2.0-0.rc7.20230208gt0983f6bf.251.vanilla.fc37.x86_64
Checking out tree 8c16620... done
Enabled rpm-md repositories: fedora-cisco-openh264 fedora-modular updates-modular updates fedora copr:copr.fedorainfracloud.org:group_kernel-vanilla:next copr:copr.fedorainfracloud.org:group_kernel-vanilla:mainline-wo-mergew coprdep:copr.fedorainfracloud.org:group_kernel-vanilla:fedora coprdep:copr.fedorainfracloud.org:group_kernel-vanilla:stable-rc coprdep:copr.fedorainfracloud.org:group_kernel-vanilla:stable updates-archive
Importing rpm-md... done
rpm-md repo 'fedora-cisco-openh264' (cached); generated: 2022-10-06T11:01:40Z solvables: 4
rpm-md repo 'fedora-modular' (cached); generated: 2022-11-05T07:58:03Z solvables: 1454
rpm-md repo 'updates-modular' (cached); generated: 2023-01-03T01:27:52Z solvables: 1464
rpm-md repo 'updates' (cached); generated: 2023-02-10T00:30:00Z solvables: 18414
rpm-md repo 'fedora' (cached); generated: 2022-11-05T08:04:38Z solvables: 66822
rpm-md repo 'copr:copr.fedorainfracloud.org:group_kernel-vanilla:next' (cached); generated: 2023-02-08T20:43:20Z solvables: 99
rpm-md repo 'copr:copr.fedorainfracloud.org:group_kernel-vanilla:mainline-wo-mergew' (cached); generated: 2023-02-10T03:54:23Z solvables: 132
rpm-md repo 'coprdep:copr.fedorainfracloud.org:group_kernel-vanilla:fedora' (cached); generated: 2023-02-09T12:51:18Z solvables: 30
rpm-md repo 'coprdep:copr.fedorainfracloud.org:group_kernel-vanilla:stable-rc' (cached); generated: 2023-02-07T15:56:26Z solvables: 40
rpm-md repo 'coprdep:copr.fedorainfracloud.org:group_kernel-vanilla:stable' (cached); generated: 2023-01-21T14:43:13Z solvables: 0
rpm-md repo 'updates-archive' (cached); generated: 2023-02-10T00:59:42Z solvables: 23391
Resolving dependencies... done
Will download: 1 package (38,7 MB)
Downloading from 'copr:copr.fedorainfracloud.org:group_kernel-vanilla:mainline-wo-mergew'... done
Importing packages... done
Relabeling... done
Applying 4 overrides
Processing packages... done
Running pre scripts... done
Running post scripts... done
Running posttrans scripts... done
Writing rpmdb... done
Generating initramfs... done
Writing OSTree commit... done
Staging deployment... done
Freed: 191,3 MB (pkgcache branches: 0)
Upgraded:
  kernel 6.1.8-200.fc37 -> 6.2.0-0.rc7.20230208gt0983f6bf.251.vanilla.fc37
  kernel-core 6.1.8-200.fc37 -> 6.2.0-0.rc7.20230208gt0983f6bf.251.vanilla.fc37
  kernel-modules 6.1.8-200.fc37 -> 6.2.0-0.rc7.20230208gt0983f6bf.251.vanilla.fc37
  kernel-modules-extra 6.1.8-200.fc37 -> 6.2.0-0.rc7.20230208gt0983f6bf.251.vanilla.fc37
Added:
  kernel-modules-core-6.2.0-0.rc7.20230208gt0983f6bf.251.vanilla.fc37.x86_64
Use "rpm-ostree override reset" to undo overrides
Run "systemctl reboot" to start a reboot

$ rpm-ostree status
State: idle
...
Deployments:
  fedora:fedora/37/x86_64/silverblue
                  Version: 37.20230201.0 (2023-02-01T02:01:44Z)
               BaseCommit: 8c16620566268918906174992bfacfe41e9f47dd433393fa95e3439736961791
             GPGSignature: Valid signature by ACB5EE4E831C74BB7C168D27F55AD3FB5323552A
                     Diff: 4 upgraded, 1 added
           LocalOverrides: kernel-core kernel-modules kernel kernel-modules-extra 6.1.8-200.fc37 -> 6.2.0-0.rc7.20230208gt0983f6bf.251.vanilla.fc37
...
```

In this second example, we will override the `podman` rpm package with the version of the package used in the Fedora *updates-testing* repository. The repository can be found in `/etc/yum.repos.d/fedora-updates-testing.repo` but is not enabled by default:
```
[updates-testing]
name=Fedora $releasever - $basearch - Test Updates
#baseurl=http://download.example/pub/fedora/linux/updates/testing/$releasever/Everything/$basearch/
metalink=https://mirrors.fedoraproject.org/metalink?repo=updates-testing-f$releasever&arch=$basearch
enabled=0
countme=1
repo_gpgcheck=0
type=rpm
gpgcheck=1
metadata_expire=6h
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-fedora-$releasever-$basearch
skip_if_unavailable=False

[...]
```
To enable the repository, we will use the `sed` command to set the first occurrence of the `enabled` flag to true:
```
$ sudo sed -i '0,/enabled=0/s//enabled=1/' /etc/yum.repos.d/fedora-updates-testing.repo
```

```
$ sudo rpm-ostree override replace podman --experimental --from repo='updates-testing'
Checking out tree e88367e... done
Enabled rpm-md repositories: fedora-cisco-openh264 fedora-modular updates-modular updates fedora updates-repo updates-archive
Importing rpm-md... done
rpm-md repo 'fedora-cisco-openh264' (cached); generated: 2023-03-14T10:56:46Z solvables: 4
rpm-md repo 'fedora-modular' (cached); generated: 2023-04-13T20:30:47Z solvables: 1082
rpm-md repo 'updates-modular' (cached); generated: 2018-02-20T19:18:14Z solvables: 0
rpm-md repo 'updates' (cached); generated: 2023-06-01T03:25:51Z solvables: 13930
rpm-md repo 'fedora' (cached); generated: 2023-04-13T20:37:10Z solvables: 69222
rpm-md repo 'updates-repo' (cached); generated: 2023-06-01T03:05:18Z solvables: 13082
rpm-md repo 'updates-archive' (cached); generated: 2023-06-01T03:49:37Z solvables: 14720
Resolving dependencies... done
Will download: 1 package (15.2 MB)
Downloading from 'updates-repo'... done
Importing packages... done
Applying 1 override
Processing packages... done
Running pre scripts... done
Running post scripts... done
Running posttrans scripts... done
Writing rpmdb... done
Writing OSTree commit... done
Staging deployment... done
Freed: 2.1 MB (pkgcache branches: 0)
Upgraded:
  podman 5:4.5.0-1.fc38 -> 5:4.5.1-1.fc38
Run "systemctl reboot" to start a reboot
```
Running the `rpm-ostree status` command shows that the `podman` package has undergone a remote override using the updates-repo repository we previously defined.
```
$ rpm-ostree status
State: idle
Deployments:
  fedora:fedora/x86_64/coreos/testing-devel
                  Version: 38.20230601.dev.0 (2023-06-01T18:17:16Z)
               BaseCommit: e88367e123c1858979bccb19ead16a3f353cd5fdfcf014218f25a5fd28cab611
             GPGSignature: (unsigned)
                     Diff: 1 upgraded
          RemoteOverrides: repo=updates-repo
                           └─ podman 5:4.5.0-1.fc38 -> 5:4.5.1-1.fc38

```
## Rolling back

You can rollback to the previous deployment:

```
$ rpm-ostree rollback
```

or reset the overrides with:

```
$ rpm-ostree override reset kernel kernel-core kernel-modules kernel-modules-core kernel-modules-extra 
```

or reset all overrides:

```
$ rpm-ostree override reset --all
```

[#1265]: https://github.com/coreos/rpm-ostree/issues/1265
[Vanilla Kernel Repositories]: https://fedoraproject.org/wiki/Kernel_Vanilla_Repositories
