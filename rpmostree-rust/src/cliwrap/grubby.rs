use anyhow::Result;

/// Primary entrypoint to running our wrapped `grubby` handling.
pub(crate) fn main(_argv: &[&str]) -> Result<()> {
    eprintln!(
        "This system is rpm-ostree based; grubby is not used.
Use `rpm-ostree kargs` instead."
    );
    std::process::exit(1);
}
