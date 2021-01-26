#pragma once

#include "rust/cxx.h"

namespace rpmostreecxx {

void early_main ();
void main_print_error (rust::Str msg);
void rpmostree_main (rust::Slice<const rust::Str> args);

}
