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
#define _strdup strdup
#endif

using namespace std;

uint32_t Kangaroo::CheckHash(uint32_t h,uint32_t nbItem,HashTable* hT,FILE* f) {

  bool ok=true;
  vector<Int> dists;
  vector<uint32_t> types;
  Point Z;
  Z.Clear();
  uint32_t nbWrong = 0;
  ENTRY *items = NULL;
  ENTRY* e;

  if( hT ) {

    for(uint32_t i = 0; i < nbItem; i++) {
      e = hT->E[h].items[i];
      Int dist;
      uint32_t kType;
      HashTable::CalcCollision(e->d,&dist,&kType);
      dists.push_back(dist);
      types.push_back(kType);
    }

  } else {

    items = (ENTRY*)malloc(nbItem * sizeof(ENTRY));

    for(uint32_t i = 0; i < nbItem; i++) {
      ::fread(items+i,32,1,f);
      e = items + i;
      Int dist;
      uint32_t kType;
      HashTable::CalcCollision(e->d,&dist,&kType);
      dists.push_back(dist);
      types.push_back(kType);
    }

  }

  vector<Point> P = secp->ComputePublicKeys(dists);
  vector<Point> Sp;

  for(uint32_t i = 0; i < nbItem; i++) {

    if(types[i] == TAME) {
      Sp.push_back(Z);
    } else {
      Sp.push_back(keyToSearch);
    }

  }

  vector<Point> S = secp->AddDirect(Sp,P);

  for(uint32_t i = 0; i < nbItem; i++) {

    if(hT)    e = hT->E[h].items[i];
    else      e = items + i;

    uint32_t hC = S[i].x.bits64[2] & HASH_MASK;
    ok = (hC == h) && (S[i].x.bits64[0] == e->x.i64[0]) && (S[i].x.bits64[1] == e->x.i64[1]);
    if(!ok) nbWrong++;
    //if(!ok) {
    //  ::printf("\nCheckWorkFile wrong at: %06X [%d]\n",h,i);
    //  ::printf("X=%s\n",S[i].x.GetBase16().c_str());
    //  ::printf("X=%08X%08X%08X%08X\n",e->x.i32[3],e->x.i32[2],e->x.i32[1],e->x.i32[0]);
    //  ::printf("D=%08X%08X%08X%08X\n",e->d.i32[3],e->d.i32[2],e->d.i32[1],e->d.i32[0]);
    //  exit(0);
    //}


  }

  if(items) free(items);
  return nbWrong;

}

bool Kangaroo::CheckPartition(TH_PARAM* p) {

  uint32_t part = p->hStart;
  string pName = string(p->part1Name);

  FILE* f1 = OpenPart(pName,"rb",part,false);
  if(f1 == NULL) return false;

  uint32_t hStart = part * (HASH_SIZE / MERGE_PART);
  uint32_t hStop = (part + 1) * (HASH_SIZE / MERGE_PART);
  p->hStart = 0;

  for(uint32_t h = hStart; h < hStop; h++) {

    uint32_t nbItem;
    uint32_t maxItem;
    ::fread(&nbItem,sizeof(uint32_t),1,f1);
    ::fread(&maxItem,sizeof(uint32_t),1,f1);

    if(nbItem == 0)
      continue;
    p->hStop += CheckHash(h,nbItem,NULL,f1);
    p->hStart += nbItem;

  }

  ::fclose(f1);
  return true;

}

bool Kangaroo::CheckWorkFile(TH_PARAM* p) {

  uint32_t nWrong = 0;

  for(uint32_t h = p->hStart; h < p->hStop; h++) {

    if(hashTable.E[h].nbItem == 0)
      continue;
    nWrong += CheckHash(h,hashTable.E[h].nbItem,&hashTable,NULL);

  }

  p->hStop = nWrong;

  return true;

}

// Threaded proc
#ifdef WIN64
DWORD WINAPI _checkPartThread(LPVOID lpParam) {
#else
void* _checkPartThread(void* lpParam) {
#endif
  TH_PARAM* p = (TH_PARAM*)lpParam;
  p->obj->CheckPartition(p);
  p->isRunning = false;
  return 0;
}

#ifdef WIN64
DWORD WINAPI _checkWorkThread(LPVOID lpParam) {
#else
void* _checkWorkThread(void* lpParam) {
#endif
  TH_PARAM* p = (TH_PARAM*)lpParam;
  p->obj->CheckWorkFile(p);
  p->isRunning = false;
  return 0;
}

void Kangaroo::CheckPartition(int nbCore,std::string& partName) {

  double t0;
  double t1;
  uint32_t v1;

  t0 = Timer::get_tick();

  // ---------------------------------------------------
  FILE* f1 = ReadHeader(partName+"/header",&v1,HEADW);
  if(f1 == NULL)
    return;

  uint32_t dp1;
  Point k1;
  uint64_t count1;
  double time1;
  Int RS1;
  Int RE1;

  // Read global param
  ::fread(&dp1,sizeof(uint32_t),1,f1);
  ::fread(&RS1.bits64,32,1,f1); RS1.bits64[4] = 0;
  ::fread(&RE1.bits64,32,1,f1); RE1.bits64[4] = 0;
  ::fread(&k1.x.bits64,32,1,f1); k1.x.bits64[4] = 0;
  ::fread(&k1.y.bits64,32,1,f1); k1.y.bits64[4] = 0;
  ::fread(&count1,sizeof(uint64_t),1,f1);
  ::fread(&time1,sizeof(double),1,f1);

  k1.z.SetInt32(1);
  if(!secp->EC(k1)) {
    ::printf("CheckPartition: key1 does not lie on elliptic curve\n");
    ::fclose(f1);
    return;
  }

  ::fclose(f1);

  // Set starting parameters
  keysToSearch.clear();
  keysToSearch.push_back(k1);
  keyIdx = 0;
  collisionInSameHerd = 0;
  rangeStart.Set(&RS1);
  rangeEnd.Set(&RE1);
  InitRange();
  InitSearchKey();

  int l2 = (int)log2(nbCore);
  int nbThread = (int)pow(2.0,l2);
  if(nbThread > MERGE_PART) nbThread = MERGE_PART;

  ::printf("Thread: %d\n",nbThread);
  ::printf("CheckingPart");

  TH_PARAM* params = (TH_PARAM*)malloc(nbThread * sizeof(TH_PARAM));
  THREAD_HANDLE* thHandles = (THREAD_HANDLE*)malloc(nbThread * sizeof(THREAD_HANDLE));
  memset(params,0,nbThread * sizeof(TH_PARAM));
  uint64_t nbDP = 0;
  uint64_t nbWrong = 0;

  for(int p = 0; p < MERGE_PART; p += nbThread) {

    printf(".");

    for(int i = 0; i < nbThread; i++) {
      params[i].threadId = i;
      params[i].isRunning = true;
      params[i].hStart = p + i;
      params[i].hStop = 0;
      params[i].part1Name = _strdup(partName.c_str());
      thHandles[i] = LaunchThread(_checkPartThread,params + i);
    }

    JoinThreads(thHandles,nbThread);
    FreeHandles(thHandles,nbThread);

    for(int i = 0; i < nbThread; i++) {
      free(params[i].part1Name);
      nbDP += params[i].hStart;
      nbWrong += params[i].hStop;
    }

  }

  free(params);
  free(thHandles);

  t1 = Timer::get_tick();

  double O = (double)nbWrong / (double)nbDP;
  O = (1.0-O) * 100.0;

  ::printf("[%.3f%% OK][%s]\n",O,GetTimeStr(t1 - t0).c_str());
  if(nbWrong>0) {

#ifdef WIN64
    ::printf("DP: %I64d\n",nbDP);
    ::printf("Wrong DP: %I64d\n",nbWrong);
#else
    ::printf("DP: %" PRId64 "\n",nbDP);
    ::printf("DP Wrong: %" PRId64 "\n",nbWrong);
#endif

  }

}

void Kangaroo::CheckWorkFile(int nbCore,std::string& fileName) {

  double t0;
  double t1;
  uint32_t v1;

#ifndef WIN64
  setvbuf(stdout,NULL,_IONBF,0);
#endif

  if(IsDir(fileName)) {
    CheckPartition(nbCore,fileName);
    return;
  }
    
  t0 = Timer::get_tick();

  // ---------------------------------------------------
  FILE* f1 = ReadHeader(fileName,&v1,HEADW);
  if(f1 == NULL)
    return;

  uint32_t dp1;
  Point k1;
  uint64_t count1;
  double time1;
  Int RS1;
  Int RE1;

  // Read global param
  ::fread(&dp1,sizeof(uint32_t),1,f1);
  ::fread(&RS1.bits64,32,1,f1); RS1.bits64[4] = 0;
  ::fread(&RE1.bits64,32,1,f1); RE1.bits64[4] = 0;
  ::fread(&k1.x.bits64,32,1,f1); k1.x.bits64[4] = 0;
  ::fread(&k1.y.bits64,32,1,f1); k1.y.bits64[4] = 0;
  ::fread(&count1,sizeof(uint64_t),1,f1);
  ::fread(&time1,sizeof(double),1,f1);

  k1.z.SetInt32(1);
  if(!secp->EC(k1)) {
    ::printf("CheckWorkFile: key1 does not lie on elliptic curve\n");
    ::fclose(f1);
    return;
  }

  // Set starting parameters
  keysToSearch.clear();
  keysToSearch.push_back(k1);
  keyIdx = 0;
  collisionInSameHerd = 0;
  rangeStart.Set(&RS1);
  rangeEnd.Set(&RE1);
  InitRange();
  InitSearchKey();

  int l2 = (int)log2(nbCore);
  int nbThread = (int)pow(2.0,l2);
  uint64_t nbDP = 0;
  uint64_t nbWrong = 0;

  ::printf("Thread: %d\n",nbThread);
  ::printf("Checking");

  TH_PARAM* params = (TH_PARAM*)malloc(nbThread * sizeof(TH_PARAM));
  THREAD_HANDLE* thHandles = (THREAD_HANDLE*)malloc(nbThread * sizeof(THREAD_HANDLE));
  memset(params,0,nbThread * sizeof(TH_PARAM));

  int block = HASH_SIZE / 64;

  for(int s = 0; s < HASH_SIZE; s += block) {

    ::printf(".");

    uint32_t S = s;
    uint32_t E = s + block;

    // Load hashtables
    hashTable.LoadTable(f1,S,E);

    int stride = block / nbThread;

    for(int i = 0; i < nbThread; i++) {
      params[i].threadId = i;
      params[i].isRunning = true;
      params[i].hStart = S + i * stride;
      params[i].hStop = S + (i + 1) * stride;
      thHandles[i] = LaunchThread(_checkWorkThread,params + i);
    }
    JoinThreads(thHandles,nbThread);
    FreeHandles(thHandles,nbThread);

    for(int i = 0; i < nbThread; i++)
      nbWrong += params[i].hStop;
    nbDP += hashTable.GetNbItem();

    hashTable.Reset();

  }

  ::fclose(f1);
  free(params);
  free(thHandles);

  t1 = Timer::get_tick();

  double O = (double)nbWrong / (double)nbDP;
  O = (1.0 - O) * 100.0;

  ::printf("[%.3f%% OK][%s]\n",O,GetTimeStr(t1 - t0).c_str());
  if(nbWrong > 0) {

#ifdef WIN64
    ::printf("DP: %I64d\n",nbDP);
    ::printf("Wrong DP: %I64d\n",nbWrong);
#else
    ::printf("DP: %" PRId64 "\n",nbDP);
    ::printf("DP Wrong: %" PRId64 "\n",nbWrong);
#endif

  }

}


void Kangaroo::Check(std::vector<int> gpuId,std::vector<int> gridSize) {

  initDPSize = 8;
  SetDP(initDPSize);

  double t0;
  double t1;
  int nbKey = 16384;
  vector<Point> pts1;
  vector<Point> pts2;
  vector<Int> priv;

  // Check on ComputePublicKeys
  for(int i = 0; i<nbKey; i++) {
    Int rnd;
    rnd.Rand(256);
    priv.push_back(rnd);
  }

  t0 = Timer::get_tick();
  for(int i = 0; i<nbKey; i++)
    pts1.push_back(secp->ComputePublicKey(&priv[i]));
  t1 = Timer::get_tick();
  ::printf("ComputePublicKey %d : %.3f KKey/s\n",nbKey,(double)nbKey / ((t1 - t0)*1000.0));

  t0 = Timer::get_tick();
  pts2 = secp->ComputePublicKeys(priv);
  t1 = Timer::get_tick();
  ::printf("ComputePublicKeys %d : %.3f KKey/s\n",nbKey,(double)nbKey / ((t1 - t0)*1000.0));

  bool ok = true;
  int i = 0;
  for(; ok && i<nbKey;) {
    ok = pts1[i].equals(pts2[i]);
    if(ok) i++;
  }

  if(!ok) {
    ::printf("ComputePublicKeys wrong at %d\n",i);
    ::printf("%s\n",pts1[i].toString().c_str());
    ::printf("%s\n",pts2[i].toString().c_str());
  }

  // Check jump table
  for(int i=0;i<128;i++) {
    rangePower = i;
    CreateJumpTable();  
  }

#ifdef WITHGPU

  // Check gpu
  if(useGpu) {

    rangePower = 64;
    CreateJumpTable();

    ::printf("GPU allocate memory:");
    int x = gridSize[0];
    int y = gridSize[1];
    if(!GPUEngine::GetGridSize(gpuId[0],&x,&y)) {
      return;
    }

    GPUEngine h(x,y,gpuId[0],65536);
    ::printf(" done\n");
    ::printf("GPU: %s\n",h.deviceName.c_str());
    ::printf("GPU: %.1f MB\n",h.GetMemory() / 1048576.0);

    int nb = h.GetNbThread() * GPU_GRP_SIZE;

    Int *gpuPx = new Int[nb];
    Int *gpuPy = new Int[nb];
    Int *gpuD = new Int[nb];
    Int *cpuPx = new Int[nb];
    Int *cpuPy = new Int[nb];
    Int *cpuD = new Int[nb];
    uint64_t *lastJump = new uint64_t[nb];
    vector<ITEM> gpuFound;

    Int pk;
    pk.Rand(256);
    keyToSearch = secp->ComputePublicKey(&pk);

    CreateHerd(nb,cpuPx,cpuPy,cpuD,TAME);
    for(int i=0;i<nb;i++) lastJump[i]=NB_JUMP;

    CreateJumpTable();

    h.SetParams(dMask,jumpDistance,jumpPointx,jumpPointy);

    h.SetKangaroos(cpuPx,cpuPy,cpuD);

    // Test single
    uint64_t r = rndl() % nb;
    CreateHerd(1,&cpuPx[r],&cpuPy[r],&cpuD[r],r % 2);
    h.SetKangaroo(r,&cpuPx[r],&cpuPy[r],&cpuD[r]);

    h.Launch(gpuFound);
    h.GetKangaroos(gpuPx,gpuPy,gpuD);
    h.Launch(gpuFound);
    ::printf("DP found: %d\n",(int)gpuFound.size());

    // Do the same on CPU
    Int _1;
    _1.SetInt32(1);
    for(int r = 0; r<NB_RUN; r++) {
      for(int i = 0; i<nb; i++) {
        uint64_t jmp = (cpuPx[i].bits64[0] % NB_JUMP);

#ifdef USE_SYMMETRY
        // Limit cycle
        if(jmp == lastJump[i]) jmp = (lastJump[i] + 1) % NB_JUMP;
#endif

        Point J(&jumpPointx[jmp],&jumpPointy[jmp],&_1);
        Point P(&cpuPx[i],&cpuPy[i],&_1);
        P = secp->AddDirect(P,J);
        cpuPx[i].Set(&P.x);
        cpuPy[i].Set(&P.y);

        cpuD[i].ModAddK1order(&jumpDistance[jmp]);

#ifdef USE_SYMMETRY
        // Equivalence symmetry class switch
        if(cpuPy[i].ModPositiveK1())
          cpuD[i].ModNegK1order();
        lastJump[i] = jmp;
#endif

        if(IsDP(cpuPx[i].bits64[3])) {

          // Search for DP found
          bool found = false;
          int j = 0;
          while(!found && j<(int)gpuFound.size()) {
            found = gpuFound[j].x.IsEqual(&cpuPx[i]) &&
              gpuFound[j].d.IsEqual(&cpuD[i]) &&
              gpuFound[j].kIdx == (uint64_t)i;
            if(!found) j++;
          }

          if(found) {
            gpuFound.erase(gpuFound.begin() + j);
          } else {
            ::printf("DP Mismatch:\n");
#ifdef WIN64
            ::printf("[%d] %s [0x%016I64X]\n",j,gpuFound[j].x.GetBase16().c_str(),gpuFound[j].kIdx);
#else
            ::printf("[%d] %s [0x%" PRIx64 "]\n",j,gpuFound[j].x.GetBase16().c_str(),gpuFound[j].kIdx);
#endif
            ::printf("[%d] %s \n",i,cpuPx[gpuFound[i].kIdx].GetBase16().c_str());
            return;
          }

        }

      }
    }

    // Compare kangaroos
    int nbFault = 0;
    bool firstFaut = true;
    for(int i = 0; i<nb; i++) {
      bool ok = gpuPx[i].IsEqual(&cpuPx[i]) && gpuPy[i].IsEqual(&cpuPy[i]) &&
                gpuD[i].IsEqual(&cpuD[i]);
      if(!ok) {
        nbFault++;
        if(firstFaut) {
          ::printf("CPU Kx=%s\n",cpuPx[i].GetBase16().c_str());
          ::printf("CPU Ky=%s\n",cpuPy[i].GetBase16().c_str());
          ::printf("CPU Kd=%s\n",cpuD[i].GetBase16().c_str());
          ::printf("GPU Kx=%s\n",gpuPx[i].GetBase16().c_str());
          ::printf("GPU Ky=%s\n",gpuPy[i].GetBase16().c_str());
          ::printf("GPU Kd=%s\n",gpuD[i].GetBase16().c_str());
          firstFaut = false;
        }
      }

    }

    if(nbFault) {
      ::printf("CPU/GPU not ok: %d/%d faults\n",nbFault,nb);
      return;
    }

    // Comapre DP


    ::printf("CPU/GPU ok\n");

  }

#endif

}
