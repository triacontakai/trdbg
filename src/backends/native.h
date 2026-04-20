#ifndef BACKENDS_NATIVE_H_
#define BACKENDS_NATIVE_H_

// picks the backend for the platform we're building on
#if defined(__linux__) && defined(__x86_64__)
#include "linux64.h"
namespace tdb::backends {
using NativeBackend = Linux64Backend;
}
#else
#error "no backend for this platform"
#endif

#endif
