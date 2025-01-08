# Project Description

This project is a **cross-platform socket programming library** for **Windows and Linux (64-bit)** designed to simplify the creation of socket-based applications by abstracting platform-specific details. It provides an easy-to-use API that allows developers to focus on application logic without worrying about low-level socket programming.

The library supports **little-endian architectures** and comes with two example programs: 
- A **TCP server** that handles client requests on dedicated threads, simulating computationally expensive tasks.
- A **TCP client** for sending requests.

While the example server uses Linux-specific threading (`pthreads`), the library itself is platform-agnostic, and developers can integrate their own cross-platform threading if needed.

The library was tested on internal Linux servers and is ideal for anyone building efficient, scalable socket-based applications on Windows and Linux platforms.

# Build & Run Instructions

The provided Makefile can be used to build the Linux example server as well as either a linux or windows example client program. Alternatively, prebuilt binaries are provided in the prebuilt directory. 

The example server requires only a single argument - the port that the server should listen on. An example invocation:

* `$ ./example-server-linux <port>`

The example client requires two arguments - a domain name or IP address to connect to, as well as the listen port chosen for the server:

* `$ ./example-client-windows <domain> <port>`

* `$ ./example-client-linux <domain> <port>`

Note that if a user were to link with this static library for their own application, they would be required to link their windows clients to the ws2 32 library.
