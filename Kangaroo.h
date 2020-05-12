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

// Input thread parameters
typedef struct {

  Kangaroo *obj;
  int  threadId;
  bool isRunning;
  bool hasStarted;
  bool isWaiting;
  uint64_t nbKangaroo;

#ifdef WITHGPU
  int  gridSizeX;
  int  gridSizeY;
  int  gpuId;
#endif

  Int *px; // Kangaroo position
  Int *py; // Kangaroo position
  Int *distance; // Travelled distance

#ifdef USE_SYMMETRY
  uint64_t *lastJump; // Last jump
#endif

} TH_PARAM;


class Kangaroo {

public:

  Kangaroo(Secp256K1 *secp,int32_t initDPSize,bool useGpu,std::string &workFile,std::string &iWorkFile,
           uint32_t savePeriod,bool saveKangaroo,double maxStep,int wtimeout);
  void Run(int nbThread,std::vector<int> gpuId,std::vector<int> gridSize);
  bool ParseConfigFile(std::string &fileName);
  bool LoadWork(std::string &fileName);
  void Check(std::vector<int> gpuId,std::vector<int> gridSize);
  void MergeWork(std::string &file1,std::string &file2,std::string &dest);
  void WorkInfo(std::string &fileName);

  // Threaded procedures
  void SolveKeyCPU(TH_PARAM *p);
  void SolveKeyGPU(TH_PARAM *p);

private:

  bool IsDP(uint64_t x);
  void SetDP(int size);
  void CreateHerd(int nbKangaroo,Int *px, Int *py, Int *d, int firstType,bool lock=true);
  void CreateJumpTable();
  bool AddToTable(Int *pos,Int *dist,uint32_t kType);
  bool CheckKey(Int d1,Int d2,uint8_t type);
  void ComputeExpected(double dp,double *op,double *ram);

  void SaveWork(std::string fileName);
  void SaveWork(uint64_t totalCount,double totalTime,TH_PARAM *threads,int nbThread);
  void FetchWalks(uint64_t nbWalk,Int *x,Int *y,Int *d);
  void FectchKangaroos(TH_PARAM *threads);
  FILE *ReadHeader(std::string fileName,uint32_t *version = NULL);

  std::string GetTimeStr(double s);

#ifdef WIN64
  HANDLE ghMutex;
  HANDLE saveMutex;
  THREAD_HANDLE LaunchThread(LPTHREAD_START_ROUTINE func,TH_PARAM *p);
#else
  pthread_mutex_t  ghMutex;
  pthread_mutex_t  saveMutex;
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

  // Range
  int rangePower;
  Int rangeStart;
  Int rangeEnd;
  Int rangeWidth;
  Int rangeWidthDiv2;
  Int rangeWidthDiv4;
  Int rangeWidthDiv8;

  uint64_t dMask;
  uint32_t dpSize;
  int32_t initDPSize;
  int collisionInSameHerd;
  std::vector<Point> keysToSearch;
  Point keyToSearch;
  Point keyToSearchNeg;
  uint32_t keyIdx;
  bool endOfSearch;
  bool useGpu;
  double expectedNbOp;
  double expectedMem;
  double maxStep;
  uint64_t totalRW;

  Int jumpDistance[NB_JUMP];
  Int jumpPointx[NB_JUMP];
  Int jumpPointy[NB_JUMP];

  int CPU_GRP_SIZE;

  // Backup stuff
  FILE *fRead;
  uint64_t offsetCount;
  double offsetTime;
  int64_t nbLoadedWalk;
  std::string workFile;
  std::string inputFile;
  int  saveWorkPeriod;
  bool saveRequest;
  bool saveKangaroo;
  int wtimeout;

};

#endif // KANGAROOH
