#include <stdlib.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <stdio.h>

#include "net.h"

struct tcp_connection {
  int listenSockFD;

  int *incomingFDs;
  size_t numIncomingFDs;
  size_t maxIncomingFDs;

  int *outgoingFDs; 
  size_t numOutgoingFDs;
  size_t maxOutgoingFDs;

  conn_opt options;
  int uid;
  int protocolVerPlatSpecific;

  SRWLOCK mutex;

  WSADATA wsaData;
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

void Initialize() {
  for (int i = 0; i < MAX_CONNECTION_OBJECTS; i++) {
    activeConnections[i].uid = -1;
    activeConnections[i].listenSockFD = -1;
    activeConnections[i].numIncomingFDs = 0;
    activeConnections[i].numOutgoingFDs = 0;
  }
}

void free_sock_addresses(remote_ips ips) {
  free(ips.ips);
}

remote_ips process_tcp_sock_addresses(tcp_connection *conn, const char **ips, const char **ports, int len) {
  remote_ips ipList = {0};
  ipList.ips = malloc(sizeof(remote_ip) * len);

  int rc = -1;
  for (int i = 0; i < len; i++) {
    struct addrinfo *res = NULL, hints = {0};

    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    switch (conn->options.ver) {
    case IPV4:
      hints.ai_family = AF_INET;
      break;
    case IPV6:
      hints.ai_family = AF_INET6;
      break;
    case DONT_CARE:
      hints.ai_family = AF_UNSPEC;
    }

    rc = getaddrinfo(ips[i], ports[i], &hints, &res);
    if (rc == 0) {
      remote_ip ip = {0};
      ip.handle = calloc(1, sizeof(remote_ip_handle));

      if (res->ai_family == AF_INET) {
        ip.handle->protocolVer = IPV4;
        memcpy(&ip.handle->ipData.ipv4, (struct sockaddr_in *)res->ai_addr, sizeof(struct sockaddr_in));
        ip.addr = malloc(INET_ADDRSTRLEN + 1);
      } else {
        ip.handle->protocolVer = IPV6;
        memcpy(&ip.handle->ipData.ipv6, (struct sockaddr_in6 *)res->ai_addr, sizeof(struct sockaddr_in6));
        ip.addr = malloc(INET6_ADDRSTRLEN + 1);
      }

      ip.port = malloc(PORT_STRLEN);
      strcpy(ip.port, ports[i]);
      ipList.ips[ipList.len] = ip;
      ipList.len++;

      freeaddrinfo(res);
    }
  }
  return ipList;
}

tcp_connection *create_tcp_connection(conn_opt opt) {
  for (int i = 0; i < MAX_CONNECTION_OBJECTS; i++) {
    if (activeConnections[i].uid == -1) {
      activeConnections[i].uid = nextUID++;
      activeConnections[i].options = opt;
      if (opt.ver == DONT_CARE) {
        activeConnections[i].options.ver = IPV4;
      }
      activeConnections[i].incomingFDs = malloc(sizeof(int) * 10);
      activeConnections[i].outgoingFDs = malloc(sizeof(int) * 10);
      activeConnections[i].maxIncomingFDs = 10;
      activeConnections[i].maxOutgoingFDs = 10;
      activeConnections[i].protocolVerPlatSpecific = activeConnections[i].options.ver == IPV4 ? AF_INET : AF_INET6 ;

      /* WSAStartup() initiates use of WS2_32.dll */
      WSAStartup(MAKEWORD(2,2), &activeConnections[i].wsaData);

      /* initiate socket file descriptor */
      int iCandidate, iResult;
      struct addrinfo *result = NULL, hints = {0};

      hints.ai_family = activeConnections[i].options.ver;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_protocol = IPPROTO_TCP;
      hints.ai_flags = AI_PASSIVE;
     
      /* convert provided port num to string for further method calls */
      char portNumStr[PORT_STRLEN];
      sprintf(portNumStr, "%d", opt.port_num);

      /* get info for a particular port number provided in the opt parameter */
      iResult = getaddrinfo(NULL, portNumStr, &hints, &result);
      if (iResult != 0) {
          printf("getaddrinfo failed: %d\n", iResult);
          return NULL;
      }

      /* generate a local listening socket and bind it */
      SOCKET ListenSocket = INVALID_SOCKET;
      struct addrinfo *candidate = NULL;

      for (candidate = result; candidate != NULL; candidate = candidate->ai_next) {
        
        ListenSocket = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);

        if (ListenSocket == INVALID_SOCKET) {
          printf("Error at socket(): %d\n", WSAGetLastError());
          continue;
        }

        /* bind the listening socket */
        iCandidate = bind(ListenSocket, candidate->ai_addr, (int) candidate->ai_addrlen);

        if (iCandidate == SOCKET_ERROR) {
            printf("bind failed with error: %d\n", WSAGetLastError());
            closesocket(ListenSocket);
            continue;
        }
        
        break;
      }

      freeaddrinfo(result);

      if(candidate == NULL || iCandidate == SOCKET_ERROR || ListenSocket == INVALID_SOCKET) {
        printf("Failed to bind socket\n");
        return NULL;
      }

      activeConnections[i].listenSockFD = ListenSocket;

      InitializeSRWLock(&activeConnections[i].mutex);
      if (activeConnections[i].listenSockFD == -1) {
        printf("Error generating a socket for this connection\n");
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
    closesocket(conn->incomingFDs[i]);
  }
  for (size_t i = 0; i < conn->numOutgoingFDs; i++) {
    closesocket(conn->outgoingFDs[i]);
  }
  closesocket(conn->listenSockFD);

  free(conn->incomingFDs);
  free(conn->outgoingFDs);
  memset(conn, 0, sizeof(tcp_connection));
  conn->uid = -1;  // Mark this slot as free
  WSACleanup();
  return 0;
}

int tcp_listen(tcp_connection *conn) {
  int listenReturn = listen(conn->listenSockFD, SOMAXCONN);

  if (listenReturn == SOCKET_ERROR) {
    printf("Listen failed with error: %d\n", WSAGetLastError() );
    closesocket(conn->listenSockFD);
    return 1;
  }

  return listenReturn;
}

int tcp_connect_remote(tcp_connection *conn, remote_ips remotes) {
  int succeedAll = 1;
  /* Iterate over the remote_ips struct and establish connection with every ip inside of it */
  for(size_t i = 0; i < remotes.len; i++) {
    int sockaddr_length;
    struct sockaddr *sockaddr_to_connect;

    /*
      Extract the ip address of the required version from the remote_ip union
      and cast it to the sockaddr pointer. 
    */
    SOCKET ConnectSocket = INVALID_SOCKET;
    if (remotes.ips[i].handle->protocolVer == IPV4) {
      sockaddr_to_connect = (struct sockaddr*) &remotes.ips[i].handle->ipData.ipv4;
      sockaddr_length = sizeof(struct sockaddr_in);
      ConnectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); 
    } else {
      sockaddr_to_connect = (struct sockaddr*) &remotes.ips[i].handle->ipData.ipv6;
      sockaddr_length = sizeof(struct sockaddr_in6);
      ConnectSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP); 
    }
  
    if (ConnectSocket == INVALID_SOCKET) {
      printf("Error at socket(): %d\n", WSAGetLastError());
      continue;
    }

    if (connect(ConnectSocket, sockaddr_to_connect, sockaddr_length) == SOCKET_ERROR) {
      closesocket(ConnectSocket);
      ConnectSocket = INVALID_SOCKET;
      printf("Error at connect()\n");
      continue;
    }
    
    /* If got here, it means we connected, we need to add the connection to outgoingFDs */
    AcquireSRWLockExclusive(&conn->mutex);
    conn->numOutgoingFDs += 1;
    if (conn->numOutgoingFDs > conn->maxOutgoingFDs) {
      conn->maxOutgoingFDs *= 2;
      conn->outgoingFDs = realloc(conn->outgoingFDs, sizeof(int) * conn->maxOutgoingFDs);
    }
    conn->outgoingFDs[conn->numOutgoingFDs - 1] = ConnectSocket;
    ReleaseSRWLockExclusive(&conn->mutex);
    remotes.ips[i].handle->fd = ConnectSocket;
  }

  return succeedAll;
}

int send_tcp_message(tcp_connection *conn, remote_ips remotes, void *data,
                     size_t len) {
  int nBytesSent, nSent = 0;
  for (size_t i = 0; i < remotes.len; i++) {
    nBytesSent = send(remotes.ips[i].handle->fd, data, len, 0);

    /* connection with this ip is closed */
    if (nBytesSent == SOCKET_ERROR) {
      int disconnectedFD = remotes.ips[i].handle->fd;
      
      /* remove the disconnected fd from outgoing fds */
      AcquireSRWLockExclusive(&conn->mutex);
      for (size_t j = 0; j < conn->numOutgoingFDs; j++) {
        if (conn->outgoingFDs[j] == disconnectedFD) {
          conn->outgoingFDs[j] = conn->outgoingFDs[conn->numOutgoingFDs - 1];
          conn->numOutgoingFDs--;
        }
      }

      /* if the disconnected fd is also an incoming fd, remove it from incoming as well */
      for (size_t j = 0; j < conn->numIncomingFDs; i++) {
        if (conn->incomingFDs[j] == disconnectedFD) {
          conn->incomingFDs[j] = conn->incomingFDs[conn->numIncomingFDs - 1];
          conn->numIncomingFDs--;
        }
      }

      ReleaseSRWLockExclusive(&conn->mutex);
    } else {
      nSent++;
    }
  }
  return nSent;
}

int receive_tcp_message_async(tcp_connection *conn, remote_ips ips, int senderIdx, void **data) {
  *data = malloc(RECV_BUFLEN);
  if ((size_t) senderIdx > ips.len -1) {
    return -1;
  }

  remote_ip *sender = &ips.ips[senderIdx];

  TIMEVAL timeval = {0};
  timeval.tv_sec = 0;
  timeval.tv_usec = conn->options.timeout;
  fd_set singleset;

  FD_ZERO(&singleset);
  FD_SET(sender->handle->fd, &singleset);

  int nBytesRecved, res = select(sender->handle->fd + 1, &singleset, NULL, NULL, &timeval);
  if (res > 0 && FD_ISSET(sender->handle->fd, &singleset)) {
    nBytesRecved = recv(sender->handle->fd, *data, RECV_BUFLEN, 0);
    
    /* 0 or SOCKET_ERROR */
    if (nBytesRecved <= 0) {
      AcquireSRWLockExclusive(&conn->mutex);
      for (size_t j = 0; j < conn->numOutgoingFDs; j++) {
        if (conn->outgoingFDs[j] == ips.ips[senderIdx].handle->fd) {
          conn->outgoingFDs[j] = conn->outgoingFDs[conn->numOutgoingFDs - 1];
          conn->numOutgoingFDs--;
        }
      }

      for (size_t j = 0; j < conn->numIncomingFDs; j++) {
        if (conn->incomingFDs[j] == ips.ips[senderIdx].handle->fd) {
          conn->incomingFDs[j] = conn->incomingFDs[conn->numIncomingFDs - 1];
          conn->numIncomingFDs--;
        }
      }

      ReleaseSRWLockExclusive(&conn->mutex);
    }
    return nBytesRecved;
  } else {
    return 0;
  }
}

int receive_tcp_message(tcp_connection *conn, remote_ips ips, int senderIdx, void **data) {
  *data = malloc(RECV_BUFLEN);
  if ((size_t) senderIdx > ips.len - 1) {
    return -1;
  }

  remote_ip *sender = &ips.ips[senderIdx];

  int nBytesRecved = recv(sender->handle->fd, *data, RECV_BUFLEN, 0);
  if (nBytesRecved == 0) {
    AcquireSRWLockExclusive(&conn->mutex);
    for (size_t j = 0; j < conn->numOutgoingFDs; j++) {
      if (conn->outgoingFDs[j] == ips.ips[senderIdx].handle->fd) {
        conn->outgoingFDs[j] = conn->outgoingFDs[conn->numOutgoingFDs - 1];
        conn->numOutgoingFDs--;
      }
    }

    for (size_t j = 0; j < conn->numIncomingFDs; j++) {
      if (conn->incomingFDs[j] == ips.ips[senderIdx].handle->fd) {
        conn->incomingFDs[j] = conn->incomingFDs[conn->numIncomingFDs - 1];
        conn->numIncomingFDs--;
      }
    }

    ReleaseSRWLockExclusive(&conn->mutex);
  }
  return nBytesRecved;
}

remote_ips tcp_active_connects(tcp_connection *conn) {
  remote_ips ips;
  ips.len = conn->numOutgoingFDs;
  ips.ips = malloc(sizeof(remote_ip) * ips.len);
  for (size_t i = 0; i < ips.len; i++) {
    remote_ip ip;
    ip.handle = calloc(1, sizeof(remote_ip_handle));
    ip.handle->protocolVer = conn->protocolVerPlatSpecific;
    struct sockaddr addr = {0};
    socklen_t addrlen = sizeof(addr);
    getpeername(conn->outgoingFDs[i], &addr, &addrlen);
    if (ip.handle->protocolVer == IPV4) {
      ip.handle->ipData.ipv4 = *(struct sockaddr_in *)&addr;
      ip.addr = malloc(INET_ADDRSTRLEN+1);
      ip.port = malloc(PORT_STRLEN);
      inet_ntop(AF_INET, &((struct sockaddr_in *)&ip.handle->ipData.ipv4)->sin_addr, ip.addr, INET_ADDRSTRLEN+1);
      sprintf(ip.port, "%u", ip.handle->ipData.ipv4.sin_port);
    } else {
      ip.handle->ipData.ipv6 = *(struct sockaddr_in6 *)&addr;
      ip.addr = malloc(INET6_ADDRSTRLEN+1);
      ip.port = malloc(6);
      inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ip.handle->ipData.ipv6)->sin6_addr, ip.addr, INET6_ADDRSTRLEN+1);
      sprintf(ip.port, "%u", ip.handle->ipData.ipv6.sin6_port);
    }
    ip.handle->fd = conn->outgoingFDs[i];
    ips.ips[i] = ip;
  }
  return ips;
}

remote_ips tcp_active_accepts(tcp_connection *conn) {
  remote_ips ips;
  ips.len = conn->numIncomingFDs;
  ips.ips = malloc(sizeof(remote_ip) * ips.len);
  for (size_t i = 0; i < ips.len; i++) {
    remote_ip ip;
    ip.handle = calloc(1, sizeof(remote_ip_handle));
    ip.handle->protocolVer = conn->protocolVerPlatSpecific;
    struct sockaddr addr = {0};
    socklen_t addrlen = sizeof(addr);
    getpeername(conn->incomingFDs[i], &addr, &addrlen);
    if (ip.handle->protocolVer == AF_INET) {
      ip.handle->ipData.ipv4 = *(struct sockaddr_in *)&addr;
      ip.addr = malloc(INET_ADDRSTRLEN+1);
      ip.port = malloc(PORT_STRLEN);
      inet_ntop(AF_INET, &((struct sockaddr_in *)&ip.handle->ipData.ipv4)->sin_addr, ip.addr, INET_ADDRSTRLEN+1);
      sprintf(ip.port, "%u", ip.handle->ipData.ipv4.sin_port);
    } else {
      ip.handle->ipData.ipv6 = *(struct sockaddr_in6 *)&addr;
      ip.addr = malloc(INET6_ADDRSTRLEN+1);
      ip.port = malloc(PORT_STRLEN);
      inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ip.handle->ipData.ipv6)->sin6_addr, ip.addr, INET6_ADDRSTRLEN+1);
      sprintf(ip.port, "%u", ip.handle->ipData.ipv6.sin6_port);
    }
    ip.handle->fd = conn->incomingFDs[i];
    ips.ips[i] = ip;
  }
  return ips;
}

remote_ip *accept_remote_connection(tcp_connection *conn) {
  /* allocate memory for new ip*/
  remote_ip *ip = malloc(sizeof(remote_ip));
  ip->handle = calloc(1, sizeof(remote_ip_handle));
  ip->handle->protocolVer = conn->protocolVerPlatSpecific;
  long long unsigned int newFD = -1;

  if (ip->handle->protocolVer == AF_INET) {
    socklen_t addrlen = sizeof(ip->handle->ipData.ipv4);
    newFD = accept(conn->listenSockFD, (struct sockaddr *) &ip->handle->ipData.ipv4, &addrlen);

    if (newFD == INVALID_SOCKET) {
      printf("accept failed: %d\n", WSAGetLastError());
      closesocket(newFD);
      return NULL;
    }

    ip->addr = malloc(INET_ADDRSTRLEN + 1);
    ip->port = malloc(PORT_STRLEN);
    inet_ntop(AF_INET, &((struct sockaddr_in *) &ip->handle->ipData.ipv4)->sin_addr, ip->addr, INET_ADDRSTRLEN+1);
    sprintf(ip->port, "%u", ip->handle->ipData.ipv4.sin_port);
  } else {
    socklen_t addrlen = sizeof(ip->handle->ipData.ipv6);
    newFD = accept(conn->listenSockFD, (struct sockaddr *)&ip->handle->ipData.ipv6, &addrlen);

    if (newFD == INVALID_SOCKET) {
      printf("accept failed: %d\n", WSAGetLastError());
      closesocket(newFD);
      return NULL;
    }

    ip->addr = malloc(INET6_ADDRSTRLEN + 1);
    ip->port = malloc(PORT_STRLEN);
    inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ip->handle->ipData.ipv6)->sin6_addr, ip->addr, INET6_ADDRSTRLEN+1);
    sprintf(ip->port, "%u", ip->handle->ipData.ipv6.sin6_port);
  }
  if ((int) newFD == -1) {
    return NULL;
  }
  // Add to current connections
  AcquireSRWLockExclusive(&conn->mutex);
  conn->numIncomingFDs += 1;
  if (conn->numIncomingFDs > conn->maxIncomingFDs) {
    conn->maxIncomingFDs *= 2;
    conn->incomingFDs = realloc(conn->incomingFDs, sizeof(int) * conn->maxIncomingFDs);
  }
  conn->incomingFDs[conn->numIncomingFDs - 1] = newFD;
  ReleaseSRWLockExclusive(&conn->mutex);
  ip->handle->fd = newFD;
  
  return ip;
}