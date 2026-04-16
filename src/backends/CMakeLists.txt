add_library(backend STATIC linux64.cc)
target_link_libraries(backend PUBLIC event_loop)
