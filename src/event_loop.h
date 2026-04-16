#ifndef EVENT_LOOP_H_
#define EVENT_LOOP_H_

#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <system_error>

namespace tdb {

/**
 * Single threaded event loop that the frontend and backends hook into
 * callbacks always run on the thread that called run()/run_until()
 */
class EventLoop {
public:
    using Handle = std::size_t;

    // callback runs when fd is readable (or hung up/errored - callbacks should handle that too)
    Handle watch_fd(int fd, std::function<void()> callback);
    // safe to call from inside a callback
    void unwatch(Handle handle);

    std::expected<void, std::error_code> run();
    std::expected<void, std::error_code> run_until(std::function<bool()> done);
    void stop();

private:
    std::expected<void, std::error_code> run_once();

    struct Watch {
        int fd;
        std::function<void()> callback;
        bool removed;
    };

    // map so adding watches from inside a callback doesn't invalidate anything
    std::map<Handle, Watch> watches_;
    Handle next_handle_ = 0;
    bool stopped_ = false;
};

} // tdb

#endif
