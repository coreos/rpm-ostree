# Repository structure


### Rpm-ostree source code
```
.
└─ src                          Contains source code for rpm-ostree
```

#### File structures under src
```
.
└─ src
  ├── app                       Contains the "frontend" logic (display CLI commands, function calls to daemon etc) for each command
  |
  ├── daemon                    Usually has the "backend" logic for each command that involves with dbus daemon and transactions
  |
  ├── lib                       Contains package and db libraries
  |
  └── libpriv                   Underlying API libraries used by files from both daemon and app

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
└── test                        Contains tests
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
