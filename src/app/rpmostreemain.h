#pragma once

#include "rust/cxx.h"

namespace rpmostreecxx {

void early_main ();
void rpmostree_process_global_teardown ();
int rpmostree_main (rust::Slice<const rust::Str> args);

}
