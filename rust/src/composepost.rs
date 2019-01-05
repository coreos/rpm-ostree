/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

use openat;
use failure::Fallible;

// This function is called from rpmostree_postprocess_final(); think of
// it as the bits of that function that we've chosen to implement in Rust.
fn compose_postprocess_final(_rootfs_dfd: &openat::Dir) -> Fallible<()> {
    Ok(())
}

mod ffi {
    use super::*;
    use ffiutil::*;
    use glib_sys;
    use libc;

    #[no_mangle]
    pub extern "C" fn ror_compose_postprocess_final(
        rootfs_dfd: libc::c_int,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        let rootfs_dfd = ffi_view_openat_dir(rootfs_dfd);
        int_glib_error(compose_postprocess_final(&rootfs_dfd), gerror)
    }
}
pub use self::ffi::*;
