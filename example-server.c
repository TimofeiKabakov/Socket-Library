// This is a simple example of utilizing the library to implement a simple
// server. The server produces a dedicated thread where it simply listens for
// incoming connections from clients On the main thread, it patiently waits for
// requests from clients who would like the server to perform some work. When it
// receives a request to perform work, it spins up a child thread to perform the
// costly computation, to ensure that it can continue listening for new client
// requests immediately, instead of having them fill up in a queue at the
// transport layer.

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "net.h"

#define MAX_CLIENT_MSG_SIZE 10

tcp_connection *conn = NULL;
remote_ips connectedClients;
pthread_rwlock_t mutex;

// Represents a single record of requested work by a client.
typedef struct work_record {
  char *addr;
  char *port;
  char *inputData;
} work_record;

// This thread entry point simply runs in a loop forever, waiting for
// new clients to connect to it.
void *accept_connections() {
  while (1) {
    accept_remote_connection(conn);
    pthread_rwlock_wrlock(&mutex);
    free_sock_addresses(connectedClients);
    connectedClients = tcp_active_accepts(conn);
    pthread_rwlock_unlock(&mutex);
  }
  return NULL;
}

// This function executes when the server receives a new work request
// In a real-world application, this function would perform some intensive
// computation that can't be done either on the server main thread or on the
// client. We fake this expensive computation by sleeping a random period
// between 3 and 9 seconds In reality, it simply multiplies the given input
// value by 3.
void *doWork(void *args) {
  // Fake some work being done...very complicated calculation!
  // Range between 3 and 9 seconds
  int sleepTime = 0;
  while ((sleepTime = rand() % 10) < 3) {
  }
  sleep(sleepTime);

  // Get the client request
  work_record *record = (work_record *)args;
  long inputData = strtol(record->inputData, NULL, 10);
  int result = inputData * 3;

  // Prepare message for transmission
  char resultMsg[MAX_CLIENT_MSG_SIZE];
  sprintf(resultMsg, "%d", result);

  pthread_rwlock_rdlock(&mutex);
  // We need to find the client that requested this work
  // If its no longer connected to the server, the work just gets dropped.
  for (size_t i = 0; i < connectedClients.len; i++) {
    if (strcmp(record->addr, connectedClients.ips[i].addr) == 0 &&
        strcmp(record->port, connectedClients.ips[i].port) == 0) {
      // Found the client we need to respond to
      remote_ips sendTo;
      sendTo.len = 1;
      sendTo.ips = malloc(sizeof(remote_ip));
      memcpy(&sendTo.ips[0], &connectedClients.ips[i], sizeof(remote_ip));
      send_tcp_message(conn, sendTo, resultMsg, strlen(resultMsg));
      break;
    }
  }
  pthread_rwlock_unlock(&mutex);

  // Cleanup, this request is done.
  free(record->addr);
  free(record->port);
  free(record->inputData);
  free(record);
  return NULL;
}

int main(int argc, const char **argv) {
  if (argc != 2) {
    printf(
        "Error: You must specify exactly one argument - the port for this "
        "server to listen on.\n");
    exit(-1);
  }
  pthread_rwlock_init(&mutex, NULL);

  // Initialize the library
  Initialize();
  conn_opt options = {0};
  options.ver = IPV4;
  options.port_num = strtol(argv[1], NULL, 10);
  conn = create_tcp_connection(options);
  if (conn == NULL) {
    printf("Fatal error!\n");
  }

  // Since this is a server, we need to open a listen port
  if (tcp_listen(conn) == 0) {
    printf("Failed to open listen server!\n");
  }

  // Have a seperate thread listen exclusively for accepted connections.
  pthread_t acceptConnThread;
  pthread_create(&acceptConnThread, NULL, accept_connections, NULL);

  // Our main loop, which receives messages from the clients and processes them
  // in a non-blocking manner
  while (1) {
    // Loop through every currently connected client
    // Inefficient locking since this is just an api example - a real world
    // application would want to perform something more efficient.
    pthread_rwlock_rdlock(&mutex);
    for (size_t i = 0; i < connectedClients.len; i++) {
      void *buf = NULL;
      int res = receive_tcp_message_async(conn, connectedClients, i, &buf);
      if (res > 0) {
        // Received some data, farm it off to a child thread
        pthread_t workThread;
        // All this allocated memory will be freed by the child thread
        work_record *record = malloc(sizeof(work_record));
        record->addr = malloc(strlen(connectedClients.ips[i].addr) + 1);
        record->port = malloc(strlen(connectedClients.ips[i].port) + 1);
        strcpy(record->addr, connectedClients.ips[i].addr);
        strcpy(record->port, connectedClients.ips[i].port);
        record->inputData = buf;
        pthread_create(&workThread, NULL, doWork, record);
      }
    }
    pthread_rwlock_unlock(&mutex);
  }
}
