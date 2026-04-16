#include <cerrno>
#include <poll.h>
#include <utility>
#include <vector>
#include "event_loop.h"

namespace tdb {

auto EventLoop::watch_fd(int fd, std::function<void()> callback) -> Handle {
    Handle handle = next_handle_++;
    watches_.emplace(handle, Watch{fd, std::move(callback), false});
    return handle;
}

void EventLoop::unwatch(Handle handle) {
    // actual removal happens in run_once, since we might be in the middle of calling this watch
    if (auto found = watches_.find(handle); found != watches_.end())
        found->second.removed = true;
}

std::expected<void, std::error_code> EventLoop::run() {
    stopped_ = false;
    while (!stopped_) {
        if (auto ok = run_once(); !ok)
            return ok;
    }
    return {};
}

std::expected<void, std::error_code> EventLoop::run_until(std::function<bool()> done) {
    stopped_ = false;
    while (!stopped_ && !done()) {
        if (auto ok = run_once(); !ok)
            return ok;
    }
    return {};
}

void EventLoop::stop() {
    stopped_ = true;
}

std::expected<void, std::error_code> EventLoop::run_once() {
    std::vector<pollfd> fds;
    std::vector<Handle> handles;
    for (auto const& [handle, watch] : watches_) {
        if (watch.removed)
            continue;
        fds.push_back({watch.fd, POLLIN, 0});
        handles.push_back(handle);
    }

    if (poll(fds.data(), fds.size(), -1) == -1) {
        if (errno == EINTR)
            return {};
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    for (std::size_t i = 0; i < fds.size(); i++) {
        if (fds[i].revents == 0)
            continue;

        // an earlier callback might have unwatched this one
        auto found = watches_.find(handles[i]);
        if (found != watches_.end() && !found->second.removed)
            found->second.callback();
    }

    std::erase_if(watches_, [](auto const& entry) { return entry.second.removed; });
    return {};
}

} // tdb
