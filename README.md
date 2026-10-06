# TRdbg

A simple modular debugger written in C++.
Heavily inspired by gdb - mainly written by me for educational purposes.
Currently supports Linux x86-64 binaries.

## Features

* running/pausing/stepping through binary execution
* software breakpoints at addresses or symbols
* reading/writing registers
* reading/writing memory

## Architecture/Components

* `src/event_loop.cc`
  * simple subscriptable event loop that different components can use for asynchronous state updates
* `src/backends/`
  * classes for managing functionality of the actual debugger (i.e. ptrace logic)
* `src/frontends`
  * classes for managing the user-interactible portion of the application
* `src/symbols/`
  * classes for managing parsing/retrieval of debug symbols from binaries

## Build

Release:
```
./scripts/build.sh
```

Debug:
```
./scripts/build.sh debug
```
