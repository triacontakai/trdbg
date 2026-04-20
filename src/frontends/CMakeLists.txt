add_library(frontend STATIC cli.cc)
target_link_libraries(frontend PUBLIC backend)
