rpm-ostree should support layering packages on top of a base tree.  Use cases:

* As a system administrator, I need to use a
  profiling/debugging/tracing tool like perf, strace, nmap

* As a system administrator, I prefer Emacs over vi/nano

The design here is this:

$ rpm-ostree add emacs

This will

* Resolve dependencies between the requested emacs package and what we have installed
* Download all packages
* Check out via hardlinks a *new* copy of the base tree
* Unpack and install all requested layered RPMs on top.  This requires
  that the %post scripts are whitelisted
  http://lists.rpm.org/pipermail/rpm-maint/2014-April/003682.html
* Commit that tree as a local branch
* Deploy that branch for the *next* boot

In this model, you will then have to reboot to install Emacs.  A
future step for rpm-ostree will be applying partial live updates:
http://blog.verbum.org/2014/02/26/ostree-rigorous-and-reliable-deployment/


