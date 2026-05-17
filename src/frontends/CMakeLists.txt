find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEDIT REQUIRED IMPORTED_TARGET libedit)

add_library(frontend STATIC cli.cc)
target_link_libraries(frontend PUBLIC backend PRIVATE PkgConfig::LIBEDIT)
