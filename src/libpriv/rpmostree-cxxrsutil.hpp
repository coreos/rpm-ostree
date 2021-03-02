/* 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <string>
#include <exception>
#include <sstream>
#include <gio/gio.h>

#include "rust/cxx.h"

// Helpers corresponding to cxxrsutil.rs
namespace rpmostreecxx {

// Wrapper for an array of GObjects.  This is a hack until
// cxx-rs gains support for either std::vector<> or Vec<T>
// with nontrivial types.
class CxxGObjectArray final {
public:
    CxxGObjectArray(GPtrArray *arr_p) : arr(arr_p) {
        g_ptr_array_ref(arr);
    };
    ~CxxGObjectArray() {
        g_ptr_array_unref(arr);
    }

    unsigned int length() {
        return (unsigned int)arr->len;
    }

    ::GObject& get(unsigned int i) {
        g_assert_cmpuint(i, <, arr->len);
        return *(::GObject*)arr->pdata[i];
    }
    GPtrArray* arr;
};

} // namespace
