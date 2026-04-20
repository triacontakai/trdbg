#include <print>
#include <utility>
#include "backends/native.h"
#include "event_loop.h"
#include "frontends/cli.h"

int main(int argc, char *argv[]) {
    if (argc > 2) {
        std::println("usage: {} [program]", argv[0]);
        return 1;
    }

    tdb::frontends::Target target;
    if (argc == 2)
        target.path = argv[1];

    tdb::EventLoop loop;
    tdb::backends::NativeBackend backend(loop);
    tdb::frontends::CliFrontend frontend(loop, backend);

    frontend.start(std::move(target));

    if (auto ret = loop.run(); !ret) {
        std::println("event loop failed: {}", ret.error().message());
        return 1;
    }

    return 0;
}
