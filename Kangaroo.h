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

#ifndef KANGAROOH
#define KANGAROOH

#include <string>
#include <vector>
#include "SECPK1/SECP256k1.h"
#include "HashTable.h"
#include "SECPK1/IntGroup.h"
#include "GPU/GPUEngine.h"

#ifdef WIN64
#include <Windows.h>
#endif

#ifdef WIN64
typedef HANDLE THREAD_HANDLE;
#define LOCK(mutex) WaitForSingleObject(mutex,INFINITE);
#define UNLOCK(mutex) ReleaseMutex(mutex);
#else
typedef pthread_t THREAD_HANDLE;
#define LOCK(mutex) pthread_mutex_lock(&(mutex));
#define UNLOCK(mutex) pthread_mutex_unlock(&(mutex));
#endif

class Kangaroo;

// Group size
#define CPU_GRP_SIZE 1024
#define MAX_TWIN 1024

#define TAME 0  // Tame kangaroo
#define WILD 1  // Wild kangaroo

typedef struct {

  int   type;      // Kangaroo type (TAME or WILD)
  Point pos;       // Current position
  Int   distance;  // Travelled distance

} KANGAROO;

// Input thread parameters
typedef struct {

  Kangaroo *obj;
  int  threadId;
  bool isRunning;
  bool hasStarted;
  bool isWaiting;

#ifdef WITHGPU
  int  gridSizeX;
  int  gridSizeY;
  int  gpuId;
#endif

} TH_PARAM;


class Kangaroo {

public:

  Kangaroo(Secp256K1 *secp,int32_t initDPSize,bool useGpu);
  void Run(int nbThread,std::vector<int> gpuId,std::vector<int> gridSize);
  bool ParseConfigFile(std::string fileName);
  void Check(std::vector<int> gpuId,std::vector<int> gridSize);

  // Threaded procedures
  void SolveKeyCPU(TH_PARAM *p);
  void SolveKeyGPU(TH_PARAM *p);

private:

  bool IsDP(uint64_t x);
  void SetDP(int size);
  KANGAROO *Create(int type,bool lock=true);

  std::string GetTimeStr(double s);

#ifdef WIN64
  HANDLE ghMutex;
  THREAD_HANDLE LaunchThread(LPTHREAD_START_ROUTINE func,TH_PARAM *p);
#else
  pthread_mutex_t  ghMutex;
  THREAD_HANDLE LaunchThread(void *(*func) (void *), TH_PARAM *p);
#endif

  void JoinThreads(THREAD_HANDLE *handles, int nbThread);
  void FreeHandles(THREAD_HANDLE *handles, int nbThread);
  void Process(TH_PARAM *params,std::string unit);

  uint64_t getCPUCount();
  uint64_t getGPUCount();
  bool isAlive(TH_PARAM *p);
  bool hasStarted(TH_PARAM *p);
  bool isWaiting(TH_PARAM *p);

  Secp256K1 *secp;
  HashTable hashTable;
  uint64_t counters[256];
  int  nbCPUThread;
  int  nbGPUThread;
  double startTime;

  Int rangeStart;
  Int rangeEnd;
  uint64_t jumpModulo;
  uint64_t dMask;
  uint32_t dpSize;
  int32_t initDPSize;
  int collisionInSameHerd;
  int rangePower;
  std::vector<Point> keysToSearch;
  Point keyToSearch;
  int keyIdx;
  bool endOfSearch;
  bool useGpu;

};

#endif // KANGAROOH
