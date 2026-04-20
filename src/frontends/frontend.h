#ifndef FRONTENDS_FRONTEND_H_
#define FRONTENDS_FRONTEND_H_

#include <concepts>
#include <string>

#include "backends/backend.h"
#include "event_loop.h"

namespace tdb::frontends {

// what to debug, usually from the command line
// arguments aren't part of it, those are up to the frontend (e.g. "run" in the cli)
struct Target {
    std::string path;
};

/**
 * A frontend handles everything the user sees and drives the backend
 * start() hooks it into the event loop, and it stops the loop when the user is done
 * it owns the backend's on_event() handler, since the backend only has one
 */
template<typename T, typename B>
concept Frontend = backends::Backend<B> && std::constructible_from<T, EventLoop&, B&> &&
    requires(T& x, Target target) {
        { x.start(target) } -> std::same_as<void>;
    };

} // tdb::frontends

#endif
