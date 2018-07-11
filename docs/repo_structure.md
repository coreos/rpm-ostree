# Repository structure


### Rpm-ostree source code
```
.
└─ src
  ├── app                       rpm-ostree CLI application
  |
  ├── daemon                    rpm-ostree daemon providing D-Bus API
  |
  ├── lib                       Public library: contains APIs for exploring rpmdb in OSTrees
  |
  └── libpriv                   Private API shared between app and daemon

```

### Rust Libraries
```
.
└─ rust                         Contains rust libraries that rpm-ostree uses─
```

### CI
```
.
├── ci                          Contains scripts to install build dependencies and run tests locally
└── .papr.yml                   PAPR(https://github.com/projectatomic/papr) uses it to define the test environment and test scripts
```


### tests

```
.
└── tests                       Contains tests
```

### Documentation

```
.
├── docs                        Contains documentation for this repository
|
├── HACKING.md                  Contains hacking information for developers
|
└── man                         Contains man page for rpm-ostree
```

### Makefiles
These files are used when doing raw build instructions. You can find more info [here](https://github.com/projectatomic/rpm-ostree/blob/master/HACKING.md#raw-build-instructions)
```
.
├── Makefile-daemon.am
├── Makefile-decls.am
├── Makefile-lib-defines.am
├── Makefile-lib.am
├── Makefile-libpriv.am
├── Makefile-libdnf.am
├── Makefile-man.am
├── Makefile-tests.am
├── Makefile.am
├── Makefile-rpm-ostree.am
|
├── configure.ac
└── autogen.sh
```
