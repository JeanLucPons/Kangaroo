/*
* This file is part of the BSGS distribution (https://github.com/JeanLucPons/Kangaroo).
* Copyright (c) 2020 Jean Luc PONS.
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, version 3.
*
* This program is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
* General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Kangaroo.h"
#include <fstream>
#include "SECPK1/IntGroup.h"
#include "Timer.h"
#include <string.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <algorithm>
#ifndef WIN64
#include <pthread.h>
#endif
#include <signal.h>

using namespace std;

static SOCKET serverSock = 0;

// ------------------------------------------------------------------------------------------------------
// Common part
// ------------------------------------------------------------------------------------------------------

#define MAX_CLIENT 50
#define WAIT_FOR_READ  1
#define WAIT_FOR_WRITE 2

#define SERVER_VERSION 1

// Commands
#define SERVER_GETCONFIG 0
#define SERVER_STATUS    1
#define SERVER_SENDDP    2

// Status
#define SERVER_OK            0
#define SERVER_END           1
#define SERVER_BACKUP        2


#ifdef WIN64

#define close_socket(s) closesocket(s)

typedef int socklen_t;

string GetNetworkError() {

  char msgbuf[512];    // for a message up to 512 bytes.
  msgbuf[0] = '\0';    // Microsoft doesn't guarantee this on man page.

  FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,   // flags
    NULL,                // lpsource
    WSAGetLastError(),   // message id
    MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT),    // languageid
    msgbuf,              // output buffer
    sizeof(msgbuf),     // size of msgbuf, bytes
    NULL);               // va_list of arguments

  return string(msgbuf);

}

#else

#define close_socket(s) close(s)

string GetNetworkError() {

  return string(strerror(errno));

}

#endif

#define GET(name,s,b,bl,t)  if( (nbRead=Read(s,(char *)(b),bl,t))<0 ) { ::printf("\nReadError(" name "): %s\n",lastError.c_str()); isConnected = false; close_socket(s); return false; }
#define PUT(name,s,b,bl,t)  if( (nbWrite=Write(s,(char *)(b),bl,t))<0 ) { ::printf("\nWriteError(" name "): %s\n",lastError.c_str()); isConnected = false; close_socket(s); return false; }
#define GETFREE(name,s,b,bl,t,x)  if( (nbRead=Read(s,(char *)(b),bl,t))<0 ) { ::printf("\nReadError(" name "): %s\n",lastError.c_str()); isConnected = false; ::free(x); close_socket(s); return false; }
#define PUTFREE(name,s,b,bl,t,x)  if( (nbWrite=Write(s,(char *)(b),bl,t))<0 ) { ::printf("\nWriteError(" name "): %s\n",lastError.c_str()); isConnected = false; ::free(x); close_socket(s); return false; }

void sig_handler(int signo) {
  if(signo == SIGINT) {
    ::printf("Terminated\n");
    if(serverSock>0) close_socket(serverSock);
#ifdef WIN64
    WSACleanup();
#endif
    exit(0);
  }
}

int Kangaroo::WaitFor(SOCKET sock,int timeout,int mode) {

  fd_set fdset;
  fd_set *rd = NULL,*wr = NULL;
  struct timeval tmout;
  int result;

  FD_ZERO(&fdset);
  FD_SET(sock,&fdset);
  if(mode == WAIT_FOR_READ)
    rd = &fdset;
  if(mode == WAIT_FOR_WRITE)
    wr = &fdset;

  tmout.tv_sec = (int)(timeout / 1000);
  tmout.tv_usec = (int)(timeout % 1000) * 1000;

  do
    result = select((int)sock + 1,rd,wr,NULL,&tmout);
  while(result < 0 && errno == EINTR);

  if(result == 0) {
    lastError = "The operation timed out";
  } else if(result < 0) {
    lastError = GetNetworkError();
    return 0;
  }

  return result;

}

int Kangaroo::Write(SOCKET sock,char *buf,int bufsize,int timeout) {

  int total_written = 0;
  int written = 0;

  while(bufsize > 0)
  {
    // Wait
    if(!WaitFor(sock,timeout,WAIT_FOR_WRITE))
      return -1;

    // Write
    do
      written = send(sock,buf,bufsize,0);
    while(written == -1 && errno == EINTR);

    if(written <= 0)
      break;

    buf += written;
    total_written += written;
    bufsize -= written;
  }

  if(written < 0) {
    lastError = GetNetworkError();
    return -1;
  }

  if(bufsize != 0) {
    lastError = "Failed to send entire buffer";
    return -1;
  }

  return total_written;

}

int Kangaroo::Read(SOCKET sock,char *buf,int bufsize,int timeout) { // Timeout in millisec

  int rd = 0;
  int total_read = 0;

  while( bufsize>0 ) {

    // Wait
    if(!WaitFor(sock,timeout,WAIT_FOR_READ)) {
      return -1;
    }

    // Read
    do
      rd = recv(sock,buf,bufsize,0);
    while(rd == -1 && errno == EINTR);

    if( rd <= 0 )
      break;

    buf += rd;
    total_read += rd;
    bufsize -= rd;

  }

  if(rd < 0) {
    lastError = GetNetworkError();
    return -1;
  }

  return total_read;

}

void Kangaroo::InitSocket() {

#ifdef WIN64
  // connect to Winscok DLL
  WSADATA WSAData;
  int err = WSAStartup(MAKEWORD(2,2),&WSAData);
  if(err != 0) {
    ::printf("WSAStartup failed error : %d\n",err);
    exit(-1);
  }
#endif

}

// ------------------------------------------------------------------------------------------------------
// Server part
// ------------------------------------------------------------------------------------------------------

// Server status
int32_t Kangaroo::GetServerStatus() {

  if(endOfSearch) {
    return SERVER_END;
  }

  if(saveRequest) {
    return SERVER_BACKUP;
  }

  return SERVER_OK;

}

// Server request handler
bool Kangaroo::HandleRequest(TH_PARAM *p) {

  char cmdBuff;
  uint32_t version = SERVER_VERSION;
  int nbRead;
  int nbWrite;
  int32_t state;

  while( p->isRunning ) {

    // Wait for command (5min timeout)
    nbRead = Read(p->clientSock,(char *)(&cmdBuff),1,(int)(CLIENT_TIMEOUT*1000.0));
    if(nbRead<=0) {
      ::printf("\nClosing connection with %s\n",p->clientInfo.c_str());
      close_socket(p->clientSock);
      return false;
    }

    switch(cmdBuff) {

    case SERVER_GETCONFIG: {
      ::printf("\nNew connection from %s\n",p->clientInfo.c_str());

      // Send config to the client
      PUT("Version",p->clientSock,&version,sizeof(uint32_t),ntimeout);
      PUT("RangeStart",p->clientSock,rangeStart.bits64,32,ntimeout);
      PUT("RangeEnd",p->clientSock,rangeEnd.bits64,32,ntimeout);
      PUT("KeyX",p->clientSock,keysToSearch[keyIdx].x.bits64,32,ntimeout);
      PUT("KeyY",p->clientSock,keysToSearch[keyIdx].y.bits64,32,ntimeout);
      PUT("DP",p->clientSock,&initDPSize,sizeof(int32_t),ntimeout);
    } break;

    case SERVER_STATUS: {

      state = GetServerStatus();
      PUT("Status",p->clientSock,&state,sizeof(int32_t),ntimeout);

    } break;

    case SERVER_SENDDP: {

      uint32_t nbDP=0;

      GET("nbDP",p->clientSock,&nbDP,sizeof(uint32_t),ntimeout);

      if(nbDP == 0) {

        ::printf("\nUnexpected number of DP [%d] from %s\n",nbDP,p->clientInfo.c_str());
        ::printf("\nClosing connection with %s\n",p->clientInfo.c_str());
        close_socket(p->clientSock);
        return false;

      } else {

        //::printf("%d DP from %s\n",nbDP,p->clientInfo.c_str());

        DP *dp = (DP *)malloc(sizeof(DP)*nbDP);
        GETFREE("DP",p->clientSock,dp,sizeof(DP)*nbDP,ntimeout,dp);
        state = GetServerStatus();
        PUTFREE("Status",p->clientSock,&state,sizeof(int32_t),ntimeout,dp);

        if(nbRead != sizeof(DP)*nbDP) {

          ::printf("\nUnexpected DP size from %s [nbDP=%d,Got %d,Expected %d]\n",p->clientInfo.c_str(),nbDP,nbRead,(int)(sizeof(DP)*nbDP));
          ::printf("\nClosing connection with %s\n",p->clientInfo.c_str());
          close_socket(p->clientSock);
          return false;

        } else {

          LOCK(ghMutex);
          DP_CACHE dc;
          dc.nbDP = nbDP;
          dc.dp = dp;
          recvDP.push_back(dc);
          UNLOCK(ghMutex);

        }

      }

    } break;

    }

  }

  close_socket(p->clientSock);
  return true;

}

// Threaded proc
#ifdef WIN64
DWORD WINAPI _acceptThread(LPVOID lpParam) {
#else
void *_acceptThread(void *lpParam) {
#endif
  TH_PARAM *p = (TH_PARAM *)lpParam;
  p->obj->HandleRequest(p);
  p->isRunning = false;
  return 0;
}

#ifdef WIN64
DWORD WINAPI _processServer(LPVOID lpParam) {
#else
void *_processServer(void *lpParam) {
#endif
  Kangaroo *obj = (Kangaroo *)lpParam;
  obj->ProcessServer();
  return 0;
}

// Main server loop
void Kangaroo::AcceptConnections(SOCKET server_soc) {

  SOCKET clientSock;

  ::printf("Kangaroo server is ready and listening to TCP port %d ...\n",port);

  while(true) {

    struct sockaddr_in client_add;
    socklen_t len = sizeof(sockaddr_in);

    if((clientSock = accept(server_soc,(struct sockaddr*)&client_add,&len)) < 0) {

      ::printf("Error: Invalid Socket returned by accept(): %s\n",GetNetworkError().c_str());

    } else {
      
      TH_PARAM p;
      char info[256];
      ::sprintf(info,"%s:%d",inet_ntoa(client_add.sin_addr),ntohs(client_add.sin_port));
      p.clientInfo = string(info);
      p.obj = this;
      p.isRunning = true;
      p.clientSock = clientSock;
      clients.push_back(p);
      LaunchThread(_acceptThread,&clients[clients.size()-1]);

    }

  }

}

// Starts the server
void Kangaroo::RunServer() {

  if(signal(SIGINT,sig_handler) == SIG_ERR)
    ::printf("\nWarning:can't install singal handler\n");

  // Set starting parameters
  collisionInSameHerd = 0;
  InitRange();
  InitSearchKey();

  ComputeExpected((double)initDPSize,&expectedNbOp,&expectedMem);
  ::printf("Expected operations: 2^%.2f\n",log2(expectedNbOp));
  ::printf("Expected RAM: %.1fMB\n",expectedMem);

  if(initDPSize<0) {
    ::printf("Error: Server must be launched with a specified number of distinguished bits (-d)\n");
    exit(-1);
  }
  SetDP(initDPSize);
  keyIdx = 0;

  if(sizeof(DP)!=40) {
    ::printf("Error: Invalid DP size struct\n");
    exit(-1);
  }

  if(saveKangaroo) {
    ::printf("Waring: Server does not support -ws, ignoring\n");
    saveKangaroo = false;
  }

  // Main thread of server (handle backup and collision check)
  LaunchThread(_processServer,(TH_PARAM *)this);

  // Server stuff

  InitSocket();

  /* Create socket */
  serverSock = socket(AF_INET,SOCK_STREAM,0);

  if(serverSock<0) {
    ::printf("Error: Invalid socket : %s\n",GetNetworkError().c_str());
    exit(-1);
  }

  struct sockaddr_in soc_addr;

  /* Reuse Address */
  int32_t yes = 1;
  if(setsockopt(serverSock,SOL_SOCKET,SO_REUSEADDR,(char *)&yes,sizeof(yes)) < 0) {
    ::printf("Warning: Couldn't Reuse Address: %s\n",GetNetworkError().c_str());
  }

  memset(&soc_addr,0,sizeof(soc_addr));
  soc_addr.sin_family = AF_INET;
  soc_addr.sin_port = htons(port);
  soc_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if(bind(serverSock,(struct sockaddr*)&soc_addr,sizeof(soc_addr))) {
    ::printf("Error: Can not bind socket. Another server running?\n%s\n",GetNetworkError().c_str());
    exit(-1);
  }

  if(listen(serverSock,MAX_CLIENT)<0) {
    ::printf("Error: Can not listen to socket\n%s\n",GetNetworkError().c_str());
    exit(-1);
  }

  AcceptConnections(serverSock);

#ifdef WIN64
  WSACleanup();
#endif

  ::printf("Abnormal termination...\n");
  return;

}

// ------------------------------------------------------------------------------------------------------
// Client part
// ------------------------------------------------------------------------------------------------------

// Connection to server
bool Kangaroo::ConnectToServer(SOCKET *retSock) {

  lastError = "";

  // Resolve IP
  if(!hostInfo) {

    if(signal(SIGINT,sig_handler) == SIG_ERR)
      ::printf("\nWarning:can't install singal handler\n");

    InitSocket();

    struct hostent *host_info;
    host_info = gethostbyname(serverIp.c_str());
    if(host_info == NULL) {
      lastError = "Unknown host:" + serverIp;
      hostInfo = NULL;
      hostInfoLength = 0;
      return false;
    } else {
      hostInfoLength = host_info->h_length;
      hostInfo = (char *)malloc(hostInfoLength);
      ::memcpy(hostInfo,host_info->h_addr,hostInfoLength);
      hostAddrType = host_info->h_addrtype;
    }

  }

  struct sockaddr_in server;

  // Build TCP connection
  SOCKET sock = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
  if(sock < 0) {
    lastError = "Socket error: " + GetNetworkError();
    return false;
  }

  // Use non blocking socket
#ifdef WIN64
  unsigned long iMode = 0;
  if(ioctlsocket(sock,FIONBIO,&iMode) != 0) {
#else
  if(fcntl(sock,F_SETFL,O_NONBLOCK) == -1) {
#endif
    lastError = "Cannot use non blocking socket, " + GetNetworkError();
    close_socket(sock);
    return false;
  }

  // Connect
  ::memset(&server,0,sizeof(sockaddr_in));
  server.sin_family = hostAddrType;
  ::memcpy((char*)&server.sin_addr,hostInfo,hostInfoLength);
  server.sin_port = htons(port);

  int connectStatus = connect(sock,(struct sockaddr *)&server,sizeof(server));

  if((connectStatus < 0) && (errno != EINPROGRESS)) {
    lastError = "Cannot connect to host: " + GetNetworkError();
    close_socket(sock);
    return false;
  }

  if(connectStatus<0) {

    // Wait for connection
    if(!WaitFor(sock,ntimeout,WAIT_FOR_WRITE)) {
      lastError = "Cannot connect, unreachable host " + serverIp;
      close_socket(sock);
      return false;
    }

    // Check connection completion
    int socket_err;
#ifdef WIN64
    int serrlen = sizeof socket_err;
    if(getsockopt(sock,SOL_SOCKET,SO_ERROR,(char *)&socket_err,&serrlen) != 0) {
#else
    socklen_t serrlen = sizeof(socket_err);
    if(getsockopt(sock,SOL_SOCKET,SO_ERROR,&socket_err,&serrlen) == -1) {
#endif
      lastError = "Cannot connect to host: " + GetNetworkError();
      close_socket(sock);
      return false;
    }

    if(socket_err != 0) {
      lastError = "Cannot connect to host: " + string(strerror(socket_err));
      close_socket(sock);
      return false;
    }

  }

  int on = 1;
  if(setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,
    (const char*)&on,sizeof(on)) == -1) {
    lastError = "Socket error: setsockopt error SO_REUSEADDR";
    close_socket(sock);
    return false;
  }

  int flag = 1;
  struct protoent *p;
  p = getprotobyname("tcp");
  if(setsockopt(sock,p->p_proto,TCP_NODELAY,(char *)&flag,sizeof(flag)) == -1) {
    lastError = "Socket error: setsockopt error TCP_NODELAY";
    close_socket(sock);
    return false;
  }

  *retSock = sock;
  return true;

}

// Wait while server is not running
void Kangaroo::WaitForServer() {

  int nbRead;
  int32_t status;
  bool ok = false;

  while(!ok) {
    
    // Wait for connection
    while(!isConnected) {
      serverStatus = "Fault";
      Timer::SleepMillis(1000);
      // Try to reconnect
      isConnected = ConnectToServer(&serverConn);
    }

    // Wait for ready
    while(isConnected && !ok) {

      char cmd = SERVER_STATUS;

      if( Write(serverConn,&cmd,1,ntimeout)<0 ) {

        ::printf("\nSendToServer(Status): %s\n",lastError.c_str()); 
        serverStatus = "Not OK";
        close_socket(serverConn);
        isConnected = false;

      } else {

        nbRead = Read(serverConn,(char *)(&status),sizeof(int32_t),ntimeout);
        if( nbRead<0 ) {
          ::printf("\nRecvFromServer(Status): %s\n",lastError.c_str()); 
          serverStatus = "Fault";
          close_socket(serverConn);
          isConnected = false;
        } else {

          switch(status) {
          case SERVER_OK:
            serverStatus = "OK";
            ok = true;
            break;

          case SERVER_END:
            serverStatus = "END";
            endOfSearch = true;
            ok = true;
            break;

          case SERVER_BACKUP:
            serverStatus = "Backup";
            Timer::SleepMillis(1000);
            break;
          }

        }

      }

    }

  }

}

// Send DP to 
bool Kangaroo::SendToServer(std::vector<ITEM> &dps) {

  int nbRead;
  int nbWrite;
  uint32_t nbDP = (uint32_t)dps.size();
  if(dps.size()==0)
    return false;

  WaitForServer();

  if(!endOfSearch) {

    int32_t status;

    // Send DP
    DP *dp = (DP *)malloc(sizeof(DP)*nbDP);
    for(uint32_t i = 0; i<nbDP; i++) {

      int128_t X;
      int128_t D;
      uint64_t h;
      HashTable::Convert(&dps[i].x,&dps[i].d,dps[i].kIdx % 2,&h,&X,&D);

      dp[i].kIdx = (uint32_t)dps[i].kIdx;
      dp[i].h = (uint32_t)h;
      dp[i].x.i64[0] = X.i64[0];
      dp[i].x.i64[1] = X.i64[1];
      dp[i].d.i64[0] = D.i64[0];
      dp[i].d.i64[1] = D.i64[1];

    }

    char cmd = SERVER_SENDDP;

    PUTFREE("CMD",serverConn,&cmd,1,ntimeout,dp);
    PUTFREE("nbDP",serverConn,&nbDP,sizeof(uint32_t),ntimeout,dp);
    PUTFREE("DP",serverConn,dp,sizeof(DP)*nbDP,ntimeout,dp);
    GETFREE("Status",serverConn,&status,sizeof(uint32_t),ntimeout,dp)

    dps.clear();
    free(dp);

  }

  return true;

}

// Get configuration from server
bool Kangaroo::GetConfigFromServer() {

  int nbRead;
  int nbWrite;

  if(!ConnectToServer(&serverConn)) {
    ::printf("Cannot connect to server: %s\n%s\n",serverIp.c_str(),lastError.c_str());
    return false;
  }

  isConnected = true;
  serverStatus = "OK";

  Point key;
  key.Clear();
  key.z.SetInt32(1);
  rangeStart.SetInt32(0);
  rangeEnd.SetInt32(0);
  initDPSize = -1;

  char cmd = SERVER_GETCONFIG;
  PUT("CMD",serverConn,&cmd,1,ntimeout);
  uint32_t version;

  GET("Version",serverConn,&version,sizeof(uint32_t),ntimeout);
  GET("RangeStart",serverConn,rangeStart.bits64,32,ntimeout);
  GET("RangeEnd",serverConn,rangeEnd.bits64,32,ntimeout);
  GET("KeyX",serverConn,key.x.bits64,32,ntimeout);
  GET("KeyY",serverConn,key.y.bits64,32,ntimeout);
  GET("DP",serverConn,&initDPSize,sizeof(int32_t),ntimeout);

  ::printf("Succesfully connected to server: %s (Version %d)\n",serverIp.c_str(),version);

  keysToSearch.clear();
  keysToSearch.push_back(key);
  return true;

}

