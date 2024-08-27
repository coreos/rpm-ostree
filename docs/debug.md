---
parent: Contributing
nav_order: 1
---

# Debugging rpm-ostree
{: .no_toc }

1. TOC
{:toc}


## Setting verbose debug messages when using the CLI.

The rpm-ostree and ostree code uses gtk's glib for the as a C library. An advantage to using glib is that to enable verbose debug messages we just need to set an environment variable:
[G_MESSAGES_DEBUG=all](https://docs.gtk.org/glib/logging.html#debug-message-output). 

Additionally, part of rpm-ostree code is written in Rust to enable verbose logs on the Rust code the environment variable is:
[RUST_LOG=debug](https://docs.rs/env_logger/latest/env_logger/).

An example of how to set use the environment variables is:

```
env G_MESSAGES_DEBUG=all RUST_LOG=debug rpm-ostree status
```

Since ostree is called from rpm-ostree it will output ostree debug messages too.

## Enabling verbose debug messages when not using the CLI.

If you need output from rpm-ostreed.service, another client such as Zincati or ostree-finalize-staged.service
you might need to override the environment variables for those services.

A way to do this is using the `sudo systemctl edit` command.

For example:

```
systemctl edit rpm-ostreed
```

Then adding:
```
[Service]
Environment="G_MESSAGES_DEBUG=all"
```

and restarting the service.

After that a more verbose output should be seen in the journal:

```
journalctl -b -u rpm-ostreed
```

**Please note** Depending on what you are trying to debug, you may need to override the environment for multiple services or pass the environment variables in ways not specified here.
