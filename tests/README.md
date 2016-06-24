Tests are divided into two groups:

- Tests in the `check` directory are non-destructive and
  uninstalled. Some of the tests require root privileges.
  Use `make check` to run these.

- Tests in the `vmcheck` directory are destructive and
  installed. They are run inside a VM. Use `make vmcheck` to
  run these (see also `HACKING.md` in the top directory).

The `common` directory contains files used by multiple
tests. The `utils` directory contains helper utilities
required to run the tests.
