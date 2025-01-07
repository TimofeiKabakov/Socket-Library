// This is a simple example of utilizing the library to implement a simple
// client. The client connects to a single server, whose information is given
// via the commandline. The client sends requests to the server, then blocks
// until it receives a response. The idea is that this simple example models a
// real-world use case where clients need to offload some very expensive
// computation onto a much more powerful server.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "net.h"

#define MAX_CLIENT_MSG_SIZE 10

int main(int argc, const char **argv) {
  if (argc < 3) {
    printf(
        "Error: Not enough arguments! You must provide a server address and "
        "port to the commandline for the server you wish to connect to.\n");
    printf("Example: ./example-client tux8.usask.ca 30553\n");
    exit(-1);
  }
  srand(time(NULL));

  // Clients only connect to one server.
  const char *IPs[1];
  const char *ports[1];
  IPs[0] = argv[1];
  ports[0] = argv[2];

  // Initialize the library
  Initialize();
  conn_opt opt = {0};
  opt.timeout = 10000000;
  opt.ver = IPV4;
  tcp_connection *conn = create_tcp_connection(opt);
  if (conn == NULL) {
    printf("Failed to allocate new tcp_connection object!\n");
    exit(-1);
  }

  // Connect to remote server and retrieve our currently active connection
  remote_ips servers = process_tcp_sock_addresses(conn, IPs, ports, 1);
  if (servers.len == 0) {
    // Couldn't process the address...
    printf("Couldn't process given server address!\n");
    exit(-1);
  }
  tcp_connect_remote(conn, servers);
  free_sock_addresses(servers);
  remote_ips active = tcp_active_connects(conn);
  if (active.len == 0) {
    // Failed to connect to the server...
    printf("Failed to connect to any server!\n");
    exit(-1);
  }

  while (1) {
    // Send a request. We choose a random value to perform the computation on.
    // The server simply computes the request value multiplied by 3, as a
    // placeholder for some real, genuinely expensive computation. We pick a
    // random value between 0 and 4000 to send to the server, translate it into
    // ASCII, and deliver that payload.
    int reqVal = rand() % 4000;
    char sendMsg[MAX_CLIENT_MSG_SIZE];
    sprintf(sendMsg, "%d", reqVal);
    send_tcp_message(conn, active, sendMsg, strlen(sendMsg));
    printf("Sent work request with request value: %d\n", reqVal);

    // Now the client just has to sit back and wait for a response. In a
    // real-world use case, you would preferably want to perform some
    // client-side work here instead of just stalling while waiting for the
    // response.
    void *data = NULL;
    // size_t len = 0;
    receive_tcp_message(conn, active, 0, &data);
    printf("Received result: %ld\n", strtol((char *)data, NULL, 10));
  }
}
