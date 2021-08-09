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
#include <memory>

#include <ostree.h>

#include "rust/cxx.h"

namespace rpmostreecxx {

class RPMDiff final {
public:
    int n_removed() const {
        return removed_->len;
    }
    int n_added() const {
        return added_->len;
    }
    int n_modified() const {
        return modified_old_->len + modified_new_->len;
    }
    ~RPMDiff();
    RPMDiff(GPtrArray *removed, GPtrArray *added, GPtrArray *modified_old, GPtrArray *modified_new);

    // TODO(cgwalters) enhance this with options
    void print() const;

private:
    GPtrArray *removed_;
    GPtrArray *added_;
    GPtrArray *modified_old_;
    GPtrArray *modified_new_;
};

std::unique_ptr<RPMDiff> rpmdb_diff(OstreeRepo &repo,
                                    const std::string &src,
                                    const std::string &dest,
                                    bool allow_noent);

} /* namespace */