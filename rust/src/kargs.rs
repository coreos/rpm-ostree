/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use bootc_kernel_cmdline::utf8::{Cmdline, Parameter};

pub fn kargs_delete(kargs: &str, to_delete: &str) -> String {
    let mut cmdline = Cmdline::from(kargs.to_string());
    let to_delete_param = match Parameter::parse(to_delete) {
        Some(p) => p,
        None => return String::new(),
    };

    if cmdline.remove_exact(&to_delete_param) {
        cmdline.to_string()
    } else {
        String::new()
    }
}
