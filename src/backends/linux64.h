#ifndef BACKENDS_LINUX_H_
#define BACKENDS_LINUX_H_

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <sys/user.h>

#include "backend.h"

namespace tdb::backends {

class Amd64Registers {
public:
    using max_register_size = std::uint64_t;
    class Iterator {
        using value_type = std::tuple<std::string_view, max_register_size>;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::forward_iterator_tag;

        value_type& operator*();
        Iterator& operator++();
        void operator++(int);
        bool operator=(Iterator& other);
    };
    
    max_register_size get_register(std::string_view reg_name);
    std::expected<std::monostate, RegisterError> set_register(std::string_view reg_name, max_register_size value);
    std::size_t register_size(std::string_view reg_name);

private:
    struct user_regs_struct regs_;
};

class Linux64Backend {
    using registers = Amd64Registers;

    void run();

    void start();
    void stop();
    std::error_code terminate();
    std::error_code kill();

    std::error_code send_signal();
    void read_registers();
private:

};

}

#endif
