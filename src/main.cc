#include "backends/linux64.h"
#include <array>
#include <print>
#include <string>

int main() {
    /*
    tdb::backends::Amd64Registers registers;

    auto ret = registers.set_register("rax", 727);
    if (!ret.has_value()) {
        std::println("error!");
        return 1;
    }

    std::println("{}", registers.get_register("rax").value());

    for (auto [name, val] : registers) {
        std::println("{}: {}", name, val.get());
    }
    */
    tdb::backends::Linux64Backend backend;

    std::array<std::string, 1> args {"-al"};
    if (auto ret = backend.launch("./wow", args); !ret) {
        std::println("failure: {}", ret.error().message());
        return 1;
    }

    if (auto ret = backend.resume(); !ret) {
        std::println("failure: {}", ret.error().message());
        return 1;
    }

    auto event = backend.wait();
    if (!event) {
        std::println("failure: {}", event.error().message());
        return 1;
    }

    return 0;
}
