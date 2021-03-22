# Extended OSTree APIs for host systems

The OSTree project upstream will continue to be written
in C for the near future.  The goal of this project
is to contain new Rust APIs that extend the "ostree-based host system"
case. In other words, some new OSTree functionality may be Rust only.

At this time, this project only contains:

 - Extension traits for `ostree::Repo` and `ostree::Sysroot` that
   have a few methods prefixed with `x_` that fix incorrect bindings.
 - An extension trait for `ostree::Sysroot` that adds small new
   API.

In the future though, more complex functionality may live here
such as the model for `apply-live` changes.

## License

Licensed under either of

 * Apache License, Version 2.0, ([LICENSE-APACHE](LICENSE-APACHE) or http://www.apache.org/licenses/LICENSE-2.0)
 * MIT license ([LICENSE-MIT](LICENSE-MIT) or http://opensource.org/licenses/MIT)

at your option.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally submitted
for inclusion in the work by you, as defined in the Apache-2.0 license, shall be dual licensed as above, without any
additional terms or conditions.
