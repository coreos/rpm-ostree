// This is an awful hack necessary because this header file
// is generated in the libdnf build, but we don't want to serialize
// our C++ build waiting for that.  Since we don't define the macros
// like LIBDNF_MAJOR_VERSION, if anything depends on them it will
// fail at build time.
#pragma once

