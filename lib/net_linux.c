#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "net.h"

struct tcp_connection {
  int listenSockFD;  // Optional, only required if tcp_listen is called.
  int *incomingFDs;  // One filedescriptor for each incoming connection through
                     // accept
  size_t numIncomingFDs;
  size_t maxIncomingFDs;
  int *outgoingFDs;  // One filedescriptor for each outgoing connection through
                     // connect
  size_t numOutgoingFDs;
  size_t maxOutgoingFDs;
  conn_opt options;
  int uid;  // Unique ID for this identifier object.
            // A value of -1 denotes this slot in activeConnections
            // is free.
  int protocolVerPlatSpecific;
  pthread_rwlock_t mutex;
};

tcp_connection activeConnections[MAX_CONNECTION_OBJECTS];
size_t connObjects = 0;
int nextUID = 0;

struct remote_ip_handle {
  int protocolVer;
  int fd;
  union {
    struct sockaddr_in ipv4;
    struct sockaddr_in6 ipv6;
  } ipData;
};

// Internal functions for comparing sockaddr structs.
int compare_addr(struct sockaddr_in *first, struct sockaddr_in *second) {
  return (first->sin_addr.s_addr == second->sin_addr.s_addr) &&
         (first->sin_port == second->sin_port);
}
int compare_addr6(struct sockaddr_in6 *first, struct sockaddr_in6 *second) {
  int match = 1;
  for (int i = 0; i < 4; i++) {
    if (first->sin6_addr.__in6_u.__u6_addr32[i] !=
        second->sin6_addr.__in6_u.__u6_addr32[i]) {
      match = 0;
      break;
    }
  }
  if (first->sin6_port == second->sin6_port) {
    match = 0;
  }
  return match;
}

// Internal function to generate a local socket and optionally bind it.
int generate_socket(remote_ip ip, int listen, int doBind) {
  int protocol = ip.handle->protocolVer == IPV4 ? AF_INET : AF_INET6;
  struct addrinfo hints = {0};
  struct addrinfo *res = NULL;
  hints.ai_family = protocol;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = listen != 0 ? AI_PASSIVE : 0;

  if (listen != 0) {
    int rc = -1;
    char portStr[PORT_STRLEN];
    sprintf(portStr, "%d", listen);
    if ((rc = getaddrinfo(NULL, portStr, &hints, &res)) != 0) {
      return -1;
    }
  } else {
    int rc = -1;
    // If its not a listen socket, we don't need to care what port we use for
    // this local socket
    if ((rc = getaddrinfo(ip.addr, "0", &hints, &res)) != 0) {
      return -1;
    }
  }

  // Got a linked list of candidate addresses
  struct addrinfo *candidate = NULL;
  for (candidate = res; candidate != NULL; candidate = candidate->ai_next) {
    if (candidate->ai_protocol != IPPROTO_TCP) {
      continue;
    }
    if (candidate->ai_socktype != SOCK_STREAM) {
      continue;
    }
    if (protocol == IPV4) {
      if (candidate->ai_family != AF_INET) {
        continue;
      }
    }
    if (protocol == IPV6) {
      if (candidate->ai_family != AF_INET6) {
        continue;
      }
    }

    int candidateSocket = 0;
    if ((candidateSocket = socket(candidate->ai_family, candidate->ai_socktype,
                                  candidate->ai_protocol)) != -1) {
      int rc = 0;
      // Don't wait for connection to time out after finishing.
      int yes = 1;
      setsockopt(candidateSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
      // Not all local sockets should actually be bound
      if (doBind) {
        if ((rc = bind(candidateSocket, candidate->ai_addr,
                       candidate->ai_addrlen)) != -1) {
          return candidateSocket;
        }
      } else {
        return candidateSocket;
      }
      close(candidateSocket);
    }
  }
  return -1;
}

// Special internal function that just calls generate_socket to create a socket
// specifically for listening.
int generate_listen_socket(int ver, int portNum) {
  remote_ip ip;
  ip.handle = malloc(sizeof(remote_ip_handle));
  memset(ip.handle, 0, sizeof(remote_ip_handle));

  ip.handle->protocolVer = ver;
  ip.addr = NULL;  // If its our own IP, just use NULL;
  ip.port = NULL;
  return generate_socket(ip, portNum, 1);
}

void Initialize() {
  // Initialize all slots to unused.
  for (int i = 0; i < MAX_CONNECTION_OBJECTS; i++) {
    activeConnections[i].uid = -1;
    activeConnections[i].listenSockFD = -1;
    activeConnections[i].numIncomingFDs = 0;
    activeConnections[i].numOutgoingFDs = 0;
  }
}

void free_sock_addresses(remote_ips ips) {
  for (size_t i = 0; i < ips.len; i++) {
    free(ips.ips[i].addr);
    free(ips.ips[i].port);
    free(ips.ips[i].handle);
  }
  free(ips.ips);
}

remote_ips process_tcp_sock_addresses(tcp_connection *conn, const char **ips,
                                      const char **ports, int len) {
  remote_ips ipList = {0};
  ipList.ips = malloc(sizeof(remote_ip) * len);

  int rc = -1;
  for (int i = 0; i < len; i++) {
    struct addrinfo *res = NULL;
    struct addrinfo hints = {0};

    hints.ai_socktype = SOCK_STREAM;
    // query the connection to get the requested IP version.
    switch (conn->options.ver) {
      case IPV4:
        hints.ai_family = AF_INET;
        break;
      case IPV6:
        hints.ai_family = AF_INET6;
        break;
      case DONT_CARE:
      default:
        hints.ai_family = AF_INET;
        break;
    }

    // Get address info for each requested ip/port combination.
    if ((rc = getaddrinfo(ips[i], ports[i], &hints, &res)) == 0) {
      // Got a linked list of candidate addresses
      struct addrinfo *candidate = NULL;
      for (candidate = res; candidate != NULL; candidate = candidate->ai_next) {
        if (candidate->ai_protocol != IPPROTO_TCP) {
          continue;
        }
        if (candidate->ai_socktype != SOCK_STREAM) {
          continue;
        }
        // We need to copy the sockaddr_in into our new array buffer,
        // since we are going to free the linked list we got from the OS
        // at the end of this function call.
        if (candidate->ai_family == AF_INET) {
          remote_ip ip = {0};
          ip.handle = malloc(sizeof(remote_ip_handle));
          memset(ip.handle, 0, sizeof(remote_ip_handle));
          ip.handle->protocolVer = IPV4;
          memcpy(&ip.handle->ipData.ipv4,
                 (struct sockaddr_in *)candidate->ai_addr,
                 sizeof(struct sockaddr_in));
          // Store the string representation into the remote_ip object
          ip.addr = malloc(INET_ADDRSTRLEN + 1);
          ip.port = malloc(PORT_STRLEN);
          strcpy(ip.addr, ips[i]);
          strcpy(ip.port, ports[i]);
          ipList.ips[ipList.len] = ip;
          ipList.len += 1;
          break;
        } else {
          remote_ip ip = {0};
          ip.handle = malloc(sizeof(remote_ip_handle));
          memset(ip.handle, 0, sizeof(remote_ip_handle));
          ip.handle->protocolVer = IPV6;
          memcpy(&ip.handle->ipData.ipv6,
                 (struct sockaddr_in6 *)candidate->ai_addr,
                 sizeof(struct sockaddr_in6));
          // Store the string representation into the remote_ip object
          ip.addr = malloc(INET6_ADDRSTRLEN + 1);
          ip.port = malloc(PORT_STRLEN);
          strcpy(ip.addr, ips[i]);
          strcpy(ip.port, ports[i]);
          ipList.ips[ipList.len] = ip;
          ipList.len += 1;
          break;
        }
      }
      freeaddrinfo(res);
    }
  }
  return ipList;
}

tcp_connection *create_tcp_connection(conn_opt opt) {
  for (int i = 0; i < MAX_CONNECTION_OBJECTS; i++) {
    if (activeConnections[i].uid == -1) {
      // Found a free slot
      activeConnections[i].uid = nextUID++;
      activeConnections[i].options = opt;
      if (opt.ver == DONT_CARE) {
        // Default to IPV4
        activeConnections[i].options.ver = IPV4;
      }
      activeConnections[i].incomingFDs = malloc(sizeof(int) * 10);
      activeConnections[i].maxIncomingFDs = 10;
      activeConnections[i].outgoingFDs = malloc(sizeof(int) * 10);
      activeConnections[i].maxOutgoingFDs = 10;
      activeConnections[i].listenSockFD =
          generate_listen_socket(activeConnections[i].options.ver,
                                 activeConnections[i].options.port_num);
      activeConnections[i].protocolVerPlatSpecific =
          activeConnections[i].options.ver == IPV4 ? AF_INET : AF_INET6;
      pthread_rwlock_init(&activeConnections[i].mutex, NULL);
      if (activeConnections[i].listenSockFD == -1) {
        return NULL;
      }
      return &activeConnections[i];
    }
  }
  return NULL;
}

int destroy_tcp_connection(tcp_connection *conn) {
  if (conn == NULL) {
    return -1;
  }

  for (size_t i = 0; i < conn->numIncomingFDs; i++) {
    close(conn->incomingFDs[i]);
  }
  for (size_t i = 0; i < conn->numOutgoingFDs; i++) {
    close(conn->outgoingFDs[i]);
  }
  close(conn->listenSockFD);

  free(conn->incomingFDs);
  free(conn->outgoingFDs);
  pthread_rwlock_destroy(&conn->mutex);
  memset(conn, 0, sizeof(tcp_connection));
  conn->uid = -1;  // Mark this slot as free
  return 0;
}

int tcp_listen(tcp_connection *conn) {
  int rc = listen(conn->listenSockFD, SOMAXCONN);
  return (rc == 0) ? 1 : 0;  // legacy api choice made this backwards
}

remote_ip *accept_remote_connection(tcp_connection *conn) {
  remote_ip *ip = malloc(sizeof(remote_ip));
  ip->handle = malloc(sizeof(remote_ip_handle));
  memset(ip->handle, 0, sizeof(remote_ip_handle));
  ip->handle->protocolVer = conn->protocolVerPlatSpecific;
  int newFD = -1;
  if (ip->handle->protocolVer == AF_INET) {
    socklen_t addrlen = sizeof(ip->handle->ipData.ipv4);
    newFD = accept(conn->listenSockFD,
                   (struct sockaddr *)&ip->handle->ipData.ipv4, &addrlen);
    // Store string representation in ip struct.
    ip->addr = malloc(INET_ADDRSTRLEN + 1);
    ip->port = malloc(PORT_STRLEN);
    inet_ntop(AF_INET,
              &((struct sockaddr_in *)&ip->handle->ipData.ipv4)->sin_addr,
              ip->addr, INET_ADDRSTRLEN + 1);
    sprintf(ip->port, "%u", ip->handle->ipData.ipv4.sin_port);
  } else {
    socklen_t addrlen = sizeof(ip->handle->ipData.ipv6);
    newFD = accept(conn->listenSockFD,
                   (struct sockaddr *)&ip->handle->ipData.ipv6, &addrlen);
    // Store string representation in ip struct.
    ip->addr = malloc(INET6_ADDRSTRLEN + 1);
    ip->port = malloc(PORT_STRLEN);
    inet_ntop(AF_INET6,
              &((struct sockaddr_in6 *)&ip->handle->ipData.ipv6)->sin6_addr,
              ip->addr, INET6_ADDRSTRLEN + 1);
    sprintf(ip->port, "%u", ip->handle->ipData.ipv6.sin6_port);
  }
  if (newFD == -1) {
    return NULL;
  }
  // Add to current connections, increasing size of array if necessary
  pthread_rwlock_wrlock(&conn->mutex);
  conn->numIncomingFDs += 1;
  if (conn->numIncomingFDs > conn->maxIncomingFDs) {
    conn->maxIncomingFDs *= 2;
    conn->incomingFDs =
        realloc(conn->incomingFDs, sizeof(int) * conn->maxIncomingFDs);
  }
  conn->incomingFDs[conn->numIncomingFDs - 1] = newFD;
  pthread_rwlock_unlock(&conn->mutex);
  // Also store the fd in the ip struct so we associate these structs with fds.
  ip->handle->fd = newFD;

  return ip;
}

int tcp_connect_remote(tcp_connection *conn, remote_ips remotes) {
  int succeedAll = 1;
  for (size_t i = 0; i < remotes.len; i++) {
    int rc = -1;
    int sockfd = generate_socket(remotes.ips[i], 0, 0);
    if (remotes.ips[i].handle->protocolVer == IPV4) {
      struct sockaddr *convert =
          (struct sockaddr *)(&(remotes.ips[i].handle->ipData.ipv4));
      socklen_t addrlen = sizeof(*convert);
      if ((rc = connect(sockfd, convert, addrlen)) == -1) {
        succeedAll = 0;
        close(sockfd);
        continue;
      }
      // Add to current connections
      pthread_rwlock_wrlock(&conn->mutex);
      conn->numOutgoingFDs += 1;
      if (conn->numOutgoingFDs > conn->maxOutgoingFDs) {
        conn->maxOutgoingFDs *= 2;
        conn->outgoingFDs =
            realloc(conn->outgoingFDs, sizeof(int) * conn->maxOutgoingFDs);
      }
      conn->outgoingFDs[conn->numOutgoingFDs - 1] = sockfd;
      pthread_rwlock_unlock(&conn->mutex);
      remotes.ips[i].handle->fd = sockfd;
    } else {
      struct sockaddr *convert =
          (struct sockaddr *)(&remotes.ips[i].handle->ipData.ipv6);
      socklen_t addrlen = sizeof(*convert);
      if ((rc = connect(sockfd, convert, addrlen)) == -1) {
        succeedAll = 0;
        close(sockfd);
        continue;
      }
      // Add to current connections
      pthread_rwlock_wrlock(&conn->mutex);
      conn->numOutgoingFDs += 1;
      if (conn->numOutgoingFDs > conn->maxOutgoingFDs) {
        conn->maxOutgoingFDs *= 2;
        conn->outgoingFDs =
            realloc(conn->outgoingFDs, sizeof(int) * conn->maxOutgoingFDs);
      }
      conn->outgoingFDs[conn->numOutgoingFDs - 1] = sockfd;
      pthread_rwlock_unlock(&conn->mutex);
      remotes.ips[i].handle->fd =
          sockfd;  // Again, associate fd with ip struct.
    }
  }
  return succeedAll;
}

remote_ips tcp_active_accepts(tcp_connection *conn) {
  remote_ips ips;
  ips.len = conn->numIncomingFDs;
  ips.ips = malloc(sizeof(remote_ip) * ips.len);
  // Loop over every fd, get peer info, construct an ip struct and add it.
  for (size_t i = 0; i < ips.len; i++) {
    remote_ip ip;
    ip.handle = malloc(sizeof(remote_ip_handle));
    memset(ip.handle, 0, sizeof(remote_ip_handle));
    ip.handle->protocolVer = conn->protocolVerPlatSpecific;
    struct sockaddr addr = {0};
    socklen_t addrlen = sizeof(addr);
    pthread_rwlock_rdlock(&conn->mutex);
    getpeername(conn->incomingFDs[i], &addr, &addrlen);
    pthread_rwlock_unlock(&conn->mutex);
    if (ip.handle->protocolVer == AF_INET) {
      ip.handle->ipData.ipv4 = *(struct sockaddr_in *)&addr;
      ip.addr = malloc(INET_ADDRSTRLEN + 1);
      ip.port = malloc(PORT_STRLEN);
      inet_ntop(AF_INET,
                &((struct sockaddr_in *)&ip.handle->ipData.ipv4)->sin_addr,
                ip.addr, INET_ADDRSTRLEN + 1);
      sprintf(ip.port, "%u", ip.handle->ipData.ipv4.sin_port);
    } else {
      ip.handle->ipData.ipv6 = *(struct sockaddr_in6 *)&addr;
      ip.addr = malloc(INET6_ADDRSTRLEN + 1);
      ip.port = malloc(PORT_STRLEN);
      inet_ntop(AF_INET6,
                &((struct sockaddr_in6 *)&ip.handle->ipData.ipv6)->sin6_addr,
                ip.addr, INET6_ADDRSTRLEN + 1);
      sprintf(ip.port, "%u", ip.handle->ipData.ipv6.sin6_port);
    }
    pthread_rwlock_wrlock(&conn->mutex);
    ip.handle->fd = conn->incomingFDs[i];
    pthread_rwlock_unlock(&conn->mutex);
    ips.ips[i] = ip;
  }
  return ips;
}

remote_ips tcp_active_connects(tcp_connection *conn) {
  remote_ips ips;
  ips.len = conn->numOutgoingFDs;
  ips.ips = malloc(sizeof(remote_ip) * ips.len);
  // Loop over every fd, get peer info, construct an ip struct and add it.
  for (size_t i = 0; i < ips.len; i++) {
    remote_ip ip;
    ip.handle = malloc(sizeof(remote_ip_handle));
    memset(ip.handle, 0, sizeof(remote_ip_handle));
    ip.handle->protocolVer = conn->protocolVerPlatSpecific;
    struct sockaddr addr = {0};
    socklen_t addrlen = sizeof(addr);
    pthread_rwlock_rdlock(&conn->mutex);
    getpeername(conn->outgoingFDs[i], &addr, &addrlen);
    pthread_rwlock_unlock(&conn->mutex);
    if (ip.handle->protocolVer == IPV4) {
      ip.handle->ipData.ipv4 = *(struct sockaddr_in *)&addr;
      ip.addr = malloc(INET_ADDRSTRLEN + 1);
      ip.port = malloc(PORT_STRLEN);
      inet_ntop(AF_INET,
                &((struct sockaddr_in *)&ip.handle->ipData.ipv4)->sin_addr,
                ip.addr, INET_ADDRSTRLEN + 1);
      sprintf(ip.port, "%u", ip.handle->ipData.ipv4.sin_port);
    } else {
      ip.handle->ipData.ipv6 = *(struct sockaddr_in6 *)&addr;
      ip.addr = malloc(INET6_ADDRSTRLEN + 1);
      ip.port = malloc(PORT_STRLEN);
      inet_ntop(AF_INET6,
                &((struct sockaddr_in6 *)&ip.handle->ipData.ipv6)->sin6_addr,
                ip.addr, INET6_ADDRSTRLEN + 1);
      sprintf(ip.port, "%u", ip.handle->ipData.ipv6.sin6_port);
    }
    pthread_rwlock_wrlock(&conn->mutex);
    ip.handle->fd = conn->outgoingFDs[i];
    pthread_rwlock_unlock(&conn->mutex);
    ips.ips[i] = ip;
  }
  return ips;
}

int send_tcp_message(tcp_connection *conn, remote_ips remotes, void *data,
                     size_t len) {
  int numSends = 0;
  for (size_t i = 0; i < remotes.len; i++) {
    int rc = send(remotes.ips[i].handle->fd, data, len, 0);
    if (rc > 0) {
      numSends++;
    } else if (rc == -1) {
      if (errno == ECONNRESET) {
        pthread_rwlock_wrlock(&conn->mutex);
        // connection closed, we need to remove the fd
        for (size_t j = 0; j < conn->numOutgoingFDs; j++) {
          if (conn->outgoingFDs[j] == remotes.ips[i].handle->fd) {
            // Remove from the list of file descriptors in tcp_connection
            conn->outgoingFDs[j] = conn->outgoingFDs[conn->numOutgoingFDs - 1];
            conn->numOutgoingFDs--;
            rc = -1;
            continue;
          }
        }
        for (size_t j = 0; j < conn->numIncomingFDs; j++) {
          if (conn->incomingFDs[j] == remotes.ips[i].handle->fd) {
            // Remove from the list of file descriptors in tcp_connection
            conn->incomingFDs[j] = conn->incomingFDs[conn->numIncomingFDs - 1];
            conn->numIncomingFDs--;
            rc = -1;
            continue;
          }
        }
        pthread_rwlock_unlock(&conn->mutex);
      }
    }
  }
  return numSends;
}

int receive_tcp_message(tcp_connection *conn, remote_ips ips, int senderIdx,
                        void **data) {
  *data = malloc(RECV_BUFLEN);
  if ((size_t)senderIdx > ips.len - 1) {
    return -1;
  }
  remote_ip *sender = &ips.ips[senderIdx];
  int rc = recv(sender->handle->fd, *data, RECV_BUFLEN, 0);
  // We have to invalidate the fd thats no longer needed, if the connection gets
  // closed.
  if (rc == 0) {
    pthread_rwlock_wrlock(&conn->mutex);
    for (size_t j = 0; j < conn->numOutgoingFDs; j++) {
      if (conn->outgoingFDs[j] == ips.ips[senderIdx].handle->fd) {
        // Remove from the list of file descriptors in tcp_connection
        conn->outgoingFDs[j] = conn->outgoingFDs[conn->numOutgoingFDs - 1];
        conn->numOutgoingFDs--;
      }
    }
    for (size_t j = 0; j < conn->numIncomingFDs; j++) {
      if (conn->incomingFDs[j] == ips.ips[senderIdx].handle->fd) {
        // Remove from the list of file descriptors in tcp_connection
        conn->incomingFDs[j] = conn->incomingFDs[conn->numIncomingFDs - 1];
        conn->numIncomingFDs--;
      }
    }
    pthread_rwlock_unlock(&conn->mutex);
    return 0;
  }

  return rc;
}

int receive_tcp_message_async(tcp_connection *conn, remote_ips ips,
                              int senderIdx, void **data) {
  *data = malloc(RECV_BUFLEN);
  if ((size_t)senderIdx > ips.len - 1) {
    return -1;
  }
  remote_ip *sender = &ips.ips[senderIdx];

  struct timeval timeVal = {0};
  timeVal.tv_sec = 0;
  timeVal.tv_usec = conn->options.timeout;
  fd_set singleSet;

  FD_ZERO(&singleSet);
  FD_SET(sender->handle->fd, &singleSet);

  int res = select(sender->handle->fd + 1, &singleSet, NULL, NULL, &timeVal);
  if (res > 0 && FD_ISSET(sender->handle->fd, &singleSet)) {
    // We have new data to return to the library user.
    int rc = recv(sender->handle->fd, *data, RECV_BUFLEN, 0);
    // We have to invalidate the fd thats no longer needed, if the connection
    // gets closed.
    if (rc == 0) {
      pthread_rwlock_wrlock(&conn->mutex);
      for (size_t j = 0; j < conn->numOutgoingFDs; j++) {
        if (conn->outgoingFDs[j] == ips.ips[senderIdx].handle->fd) {
          // Remove from the list of file descriptors in tcp_connection
          conn->outgoingFDs[j] = conn->outgoingFDs[conn->numOutgoingFDs - 1];
          conn->numOutgoingFDs--;
        }
      }
      for (size_t j = 0; j < conn->numIncomingFDs; j++) {
        if (conn->incomingFDs[j] == ips.ips[senderIdx].handle->fd) {
          // Remove from the list of file descriptors in tcp_connection
          conn->incomingFDs[j] = conn->incomingFDs[conn->numIncomingFDs - 1];
          conn->numIncomingFDs--;
        }
      }
      pthread_rwlock_unlock(&conn->mutex);
      return 0;
    }
    return rc;
  } else {
    return 0;
  }
}