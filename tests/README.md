Tests are divided into three groups:

- Tests in the `check` directory are non-destructive and
  uninstalled. Some of the tests require root privileges.
  Use `make check` to run these.

- The `composecheck` tests currently require uid 0 capabilities -
  the default in Docker, or you can run them via a user namespace.
  They are non-destructive, but are installed.

  To use them, you might do a `make && sudo make install` inside a
  Docker container.

  Then invoke `./tests/compose`.  Alternatively of course, you
  can simply run the tests on a host system or in an existing
  container, without doing a build.

  Note: This is intentionally *not* a `Makefile` target because
  it doesn't require building and doesn't use uninstalled binaries.

- Tests in the `vmcheck` directory are oriented around testing
  in disposable virtual machines.  Use `make vmcheck` to run them.
  See also `HACKING.md` in the top directory.

The `common` directory contains files used by multiple
tests. The `utils` directory contains helper utilities
required to run the tests.
