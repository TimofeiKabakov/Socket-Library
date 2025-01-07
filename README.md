This project is a **cross-platform socket programming library** for **Windows and Linux (64-bit)** designed to simplify the creation of socket-based applications by abstracting platform-specific details. It provides an easy-to-use API that allows developers to focus on application logic without worrying about low-level socket programming.

The library supports **little-endian architectures** and comes with two example programs: 
- A **TCP server** that handles client requests on dedicated threads, simulating computationally expensive tasks.
- A **TCP client** for sending requests.

While the example server uses Linux-specific threading (`pthreads`), the library itself is platform-agnostic, and developers can integrate their own cross-platform threading if needed.

The library was tested on internal Linux servers and is ideal for anyone building efficient, scalable socket-based applications on Windows and Linux platforms.
