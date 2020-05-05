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

using namespace std;

// ----------------------------------------------------------------------------

Kangaroo::Kangaroo(Secp256K1 *secp,int32_t initDPSize,bool useGpu) {

  this->secp = secp;
  this->initDPSize = initDPSize;
  this->useGpu = useGpu;
  CPU_GRP_SIZE = 1024;

  // Init mutex
#ifdef WIN64
  ghMutex = CreateMutex(NULL,FALSE,NULL);
#else
  ghMutex = PTHREAD_MUTEX_INITIALIZER;
#endif

}

// ----------------------------------------------------------------------------

bool Kangaroo::ParseConfigFile(std::string fileName) {

  // Check file
  FILE *fp = fopen(fileName.c_str(),"rb");
  if(fp == NULL) {
    ::printf("Error: Cannot open %s %s\n",fileName.c_str(),strerror(errno));
    return false;
  }
  fclose(fp);

  // Get lines
  vector<string> lines;
  int nbLine = 0;
  string line;
  ifstream inFile(fileName);
  while(getline(inFile,line)) {

    // Remove ending \r\n
    int l = (int)line.length() - 1;
    while(l >= 0 && isspace(line.at(l))) {
      line.pop_back();
      l--;
    }

    if(line.length() > 0) {
      lines.push_back(line);
      nbLine++;
    }

  }

  if(lines.size()<3) {
    ::printf("Error: %s not enough arguments\n",fileName.c_str());
    return false;
  }

  rangeStart.SetBase16((char *)lines[0].c_str());
  rangeEnd.SetBase16((char *)lines[1].c_str());
  for(int i=2;i<(int)lines.size();i++) {
    
    Point p;
    bool isCompressed;
    if( !secp->ParsePublicKeyHex(lines[i],p,isCompressed) ) {
      ::printf("%s, error line %d: %s\n",fileName.c_str(),i,lines[i].c_str());
      return false;
    }
    keysToSearch.push_back(p);

  }

  ::printf("Start:%s\n",rangeStart.GetBase16().c_str());
  ::printf("Stop :%s\n",rangeEnd.GetBase16().c_str());
  ::printf("Keys :%d\n",(int)keysToSearch.size());

  return true;

}

// ----------------------------------------------------------------------------

bool Kangaroo::IsDP(uint64_t x) {

  return (x & dMask) == 0;

}

void Kangaroo::SetDP(int size) {

  // Mask for distinguised point
  dpSize = size;
  if(dpSize == 0) {
    dMask = 0;
  } else {
    if(dpSize > 64) dpSize = 64;
    dMask = (1ULL << (64 - dpSize)) - 1;
    dMask = ~dMask;
  }

#ifdef WIN64
  ::printf("DP size: %d [0x%016I64X]\n",dpSize,dMask);
#else
  ::printf("DP size: %d [0x%" PRIx64 "]\n",dpSize,dMask);
#endif

}

// ----------------------------------------------------------------------------

bool  Kangaroo::CheckKey(Int d1,Int d2,uint8_t type) {

  // Resolve equivalence collision

  if(type & 0x1)
    d1.ModNegK1order();
  if(type & 0x2)
    d2.ModNegK1order();

  Int pk(&d1);
  pk.ModAddK1order(&d2);

  Point P = secp->ComputePublicKey(&pk);

  if(P.equals(keyToSearch)) {
    // Key solved    
#ifdef USE_SYMMETRY
    pk.ModAddK1order(&rangeWidthDiv2);
#endif
    pk.ModAddK1order(&rangeStart);    
    Point PR = secp->ComputePublicKey(&pk);
    ::printf("\nKey#%2d [%dN]Pub:  0x%s \n",keyIdx,type,secp->GetPublicKeyHex(true,keysToSearch[keyIdx]).c_str());
    if( PR.equals(keysToSearch[keyIdx]) ) {
      ::printf("       Priv: 0x%s \n",pk.GetBase16().c_str());
    } else {
      ::printf("       Failed !\n");
      return false;
    }
    return true;
  }

  if(P.equals(keyToSearchNeg)) {
    // Key solved
    pk.ModNegK1order();
#ifdef USE_SYMMETRY
    pk.ModAddK1order(&rangeWidthDiv2);
#endif
    pk.ModAddK1order(&rangeStart);
    Point PR = secp->ComputePublicKey(&pk);
    ::printf("\nKey#%2d [%dS]Pub:  0x%s \n",keyIdx,type,secp->GetPublicKeyHex(true,keysToSearch[keyIdx]).c_str());
    if(PR.equals(keysToSearch[keyIdx]) ) {
      ::printf("       Priv: 0x%s \n",pk.GetBase16().c_str());
    } else {
      ::printf("       Failed !\n");
      return false;
    }
    return true;
  }

  return false;

}

// ----------------------------------------------------------------------------

bool Kangaroo::AddToTable(Int *pos,Int *dist,uint32_t kType) {

  if(hashTable.Add(pos,dist,kType)) {

    int type = hashTable.GetType();

    if(type == kType) {

      // Collision inside the same herd
      return false;

    } else {

      Int Td;
      Int Wd;

      if(kType==TAME) {
        Td.Set(dist);
        Wd.Set(hashTable.GetD());
      } else {
        Td.Set(hashTable.GetD());
        Wd.Set(dist);
      }

      endOfSearch = CheckKey(Td,Wd,0) || CheckKey(Td,Wd,1) || CheckKey(Td,Wd,2) || CheckKey(Td,Wd,3);

      if(!endOfSearch) {

        // Should not happen, reset the kangaroo
        ::printf("\n Unexpected wrong collision, reset kangaroo !\n");
        if((int64_t)(Td.bits64[3])<0) {
          Td.ModNegK1order();
          ::printf("Found: Td-%s\n",Td.GetBase16().c_str());
        } else {
          ::printf("Found: Td %s\n",Td.GetBase16().c_str());
        }
        if((int64_t)(Wd.bits64[3])<0) {
          Wd.ModNegK1order();
          ::printf("Found: Td-%s\n",Wd.GetBase16().c_str());
        } else {
          ::printf("Found: Td %s\n",Wd.GetBase16().c_str());
        }
        return false;

      }

    }

  }

  return true;
}

// ----------------------------------------------------------------------------

void Kangaroo::SolveKeyCPU(TH_PARAM *ph) {

  // Global init
  int thId = ph->threadId;
  counters[thId] = 0;

  // Create Kangaroos
  KANGAROO *herd = new KANGAROO[CPU_GRP_SIZE];

  IntGroup *grp = new IntGroup(CPU_GRP_SIZE);
  Int *dx = new Int[CPU_GRP_SIZE];

  if(keyIdx==0) {
    ::printf("SolveKeyCPU Thread %d: %d kangaroos\n",ph->threadId,CPU_GRP_SIZE);
  }

  ph->hasStarted = true;

  // Using Affine coord
  Int dy;
  Int rx;
  Int ry;
  Int _s;
  Int _p;

  while(!endOfSearch) {

    for(int j = 0; j<CPU_GRP_SIZE; j++)
      Create(&herd[j],j % 2);

    uint64_t cnt = 0;
    while(!endOfSearch && cnt<maxStep) {

      for(int g = 0; g < CPU_GRP_SIZE; g++) {

        uint64_t jmp = herd[g].pos.x.bits64[0] % NB_JUMP;

#ifdef USE_SYMMETRY
        // Limit cycle
        if( jmp == herd[g].lastJump ) jmp = (herd[g].lastJump + 1) % NB_JUMP;
#endif

        Int *p1x = &jumpPointx[jmp];
        Int *p2x = &herd[g].pos.x;
        dx[g].ModSub(p2x,p1x);

      }
      grp->Set(dx);
      grp->ModInv();

      for(int g = 0; g < CPU_GRP_SIZE; g++) {

        uint64_t jmp = herd[g].pos.x.bits64[0] % NB_JUMP;

#ifdef USE_SYMMETRY
        // Limit cycle
        if(jmp == herd[g].lastJump) jmp = (herd[g].lastJump + 1) % NB_JUMP;
#endif

        Int *p1x = &jumpPointx[jmp];
        Int *p1y = &jumpPointy[jmp];
        Int *p2x = &herd[g].pos.x;
        Int *p2y = &herd[g].pos.y;

        dy.ModSub(p2y,p1y);
        _s.ModMulK1(&dy,&dx[g]);
        _p.ModSquareK1(&_s);

        rx.ModSub(&_p,p1x);
        rx.ModSub(p2x);

        ry.ModSub(p2x,&rx);
        ry.ModMulK1(&_s);
        ry.ModSub(p2y);

        herd[g].distance.ModAddK1order(&jumpDistance[jmp]);

#ifdef USE_SYMMETRY
        // Equivalence symmetry class switch
        if(ry.ModPositiveK1())
          herd[g].distance.ModNegK1order();
        herd[g].lastJump = jmp;
#endif

        herd[g].pos.x.Set(&rx);
        herd[g].pos.y.Set(&ry);

      }

      for(int g = 0; g < CPU_GRP_SIZE && !endOfSearch; g++) {

        if(IsDP(herd[g].pos.x.bits64[3])) {
          LOCK(ghMutex);
          if(!endOfSearch) {

            if(!AddToTable(&herd[g].pos.x,&herd[g].distance,herd[g].type)) {
              // Collision inside the same herd
              // We need to reset the kangaroo
              Create(&herd[g],herd[g].type,false);
              collisionInSameHerd++;
            }

          }
          UNLOCK(ghMutex);
        }

        if(!endOfSearch) counters[thId] ++;
        cnt++;

      }

    }
  }

  // Free
  delete grp;
  delete dx;
  delete[] herd;

  ph->isRunning = false;

}

// ----------------------------------------------------------------------------

void Kangaroo::SolveKeyGPU(TH_PARAM *ph) {

  // Global init
  int thId = ph->threadId;
  counters[thId] = 0;

#ifdef WITHGPU

  vector<ITEM> gpuFound;
  GPUEngine *gpu;
  Int *px;
  Int *py;
  Int *d;

  gpu = new GPUEngine(ph->gridSizeX,ph->gridSizeY,ph->gpuId,65536 * 2);

  if(keyIdx == 0) {
    ::printf("GPU: %s (%.1f MB used)\n",gpu->deviceName.c_str(),gpu->GetMemory() / 1048576.0);
    ::printf("SolveKeyGPU Thread GPU#%d: creating kangaroos...\n",ph->gpuId);
  }

  double t0 = Timer::get_tick();

  // Create Kangaroos
  uint64_t nbThread = gpu->GetNbThread();
  uint64_t nbKangaroo = nbThread * GPU_GRP_SIZE;
  px = new Int[nbKangaroo];
  py = new Int[nbKangaroo];
  d = new Int[nbKangaroo];
  Point Z;
  Z.Clear();

  int k = 0;
  for(uint64_t i = 0; i<nbThread; i++) {

    vector<Int> pk;
    vector<Point> S;
    vector<Point> Sp;
    pk.reserve(GPU_GRP_SIZE);
    S.reserve(GPU_GRP_SIZE);
    Sp.reserve(GPU_GRP_SIZE);

    // Choose random starting distance
    LOCK(ghMutex);
    for(uint64_t j = 0; j<GPU_GRP_SIZE; j++) {

#ifdef USE_SYMMETRY
      d[i*GPU_GRP_SIZE + j].Rand(rangePower-1);
      if(j % 2 == WILD) {
        // Wild in [-N/4..N/4]
        d[i*GPU_GRP_SIZE + j].ModSubK1order(&rangeWidthDiv4);
      }
#else
      d[i*GPU_GRP_SIZE + j].Rand(rangePower);
      if(j % 2 == WILD) {
        // Wild in [-N/2..N/2]
        d[i*GPU_GRP_SIZE + j].ModSubK1order(&rangeWidthDiv2);
      }
#endif

      pk.push_back(d[i*GPU_GRP_SIZE + j]);
    }
    UNLOCK(ghMutex);

    // Compute starting pos
    S = secp->ComputePublicKeys(pk);

    for(uint64_t j = 0; j<GPU_GRP_SIZE; j++) {
      if(j % 2 == TAME) {
        Sp.push_back(Z);
      } else {
        Sp.push_back(keyToSearch);
      }
    }

    S = secp->AddDirect(Sp,S);

    for(uint64_t j = 0; j<GPU_GRP_SIZE; j++) {
      px[i*GPU_GRP_SIZE + j].Set(&S[j].x);
      py[i*GPU_GRP_SIZE + j].Set(&S[j].y);
#ifdef USE_SYMMETRY
      // Equivalence symmetry class switch
      if(py[i*GPU_GRP_SIZE + j].ModPositiveK1())
        d[i*GPU_GRP_SIZE + j].ModNegK1order();
#endif
    }

  }

  gpu->SetParams(dMask,jumpDistance,jumpPointx,jumpPointy);
  gpu->SetKangaroos(px,py,d);
  gpu->callKernel();

  double t1 = Timer::get_tick();

  if(keyIdx == 0)
    ::printf("SolveKeyGPU Thread GPU#%d: 2^%.2f kangaroos in %.1fms\n",ph->gpuId,log2((double)nbKangaroo),(t1-t0)*1000.0);

  ph->hasStarted = true;

  while(!endOfSearch) {

    gpu->Launch(gpuFound);
    counters[thId] += nbKangaroo * NB_RUN;

    if(gpuFound.size() > 0) {
      
      LOCK(ghMutex);

      for(int g=0;!endOfSearch && g<gpuFound.size();g++) {

        uint32_t kType = (uint32_t)(gpuFound[g].kIdx % 2);

        if(!AddToTable(&gpuFound[g].x,&gpuFound[g].d,kType)) {
          // Collision inside the same herd
          // We need to reset the kangaroo
          KANGAROO K;
          Create(&K,kType,false);
          gpu->SetKangaroo(gpuFound[g].kIdx,&K.pos.x,&K.pos.y,&K.distance);
          collisionInSameHerd++;
        }

      }
      UNLOCK(ghMutex);
    }

  }


  delete px;
  delete py;
  delete d;
  delete gpu;

#else

  ph->hasStarted = true;

#endif

  ph->isRunning = false;

}

// ----------------------------------------------------------------------------

#ifdef WIN64
DWORD WINAPI _SolveKeyCPU(LPVOID lpParam) {
#else
void *_SolveKeyCPU(void *lpParam) {
#endif
  TH_PARAM *p = (TH_PARAM *)lpParam;
  p->obj->SolveKeyCPU(p);
  return 0;
}

#ifdef WIN64
DWORD WINAPI _SolveKeyGPU(LPVOID lpParam) {
#else
void *_SolveKeyGPU(void *lpParam) {
#endif
  TH_PARAM *p = (TH_PARAM *)lpParam;
  p->obj->SolveKeyGPU(p);
  return 0;
}

// ----------------------------------------------------------------------------

void Kangaroo::Create(KANGAROO *k,int type,bool lock) {

  k->type = type;

#ifdef USE_SYMMETRY
  if(lock) LOCK(ghMutex);
  k->distance.Rand(rangePower - 1);
  if(lock) UNLOCK(ghMutex);
#else
  if(lock) LOCK(ghMutex);
  k->distance.Rand(rangePower);
  if(lock) UNLOCK(ghMutex);
#endif

  if( type == TAME ) {

    // Tame in [0..N/2] using symmetry, [0..N] not using symmetry
    k->pos = secp->ComputePublicKey(&k->distance);

  } else {

    // Wild in [-N/4..N/4] using symmetry, [-N/2..N/2] not using symmetry
#ifdef USE_SYMMETRY
    k->distance.ModSubK1order(&rangeWidthDiv4);
#else
    k->distance.ModSubK1order(&rangeWidthDiv2);
#endif
    Point O = secp->ComputePublicKey(&k->distance);
    k->pos = secp->AddDirect(keyToSearch,O);

  }

#ifdef USE_SYMMETRY
  // Equivalence symmetry class switch
  if(k->pos.y.ModPositiveK1())
    k->distance.ModNegK1order();
#endif

  k->lastJump = NB_JUMP;

}

// ----------------------------------------------------------------------------

void Kangaroo::CreateJumpTable() {

#ifdef USE_SYMMETRY
  int jumpBit = rangePower / 2 + 1;
#else
  int jumpBit = rangePower / 2;
#endif
  if(jumpBit > 128) jumpBit = 128;
  int maxRetry = 100;
  bool ok = false;
  double maxAvg = pow(2.0,(double)jumpBit - 0.95);
  double minAvg = pow(2.0,(double)jumpBit - 1.05);
  double distAvg;
  //::printf("Jump Avg distance min: 2^%.2f\n",log2(minAvg));
  //::printf("Jump Avg distance max: 2^%.2f\n",log2(maxAvg));

  // Kangaroo jumps

  // Positive only
  // When using symmetry, the sign is switched by the symmetry class switch
  while(!ok && maxRetry>0 ) {
    Int totalDist;
    totalDist.SetInt32(0);
    for(int i = 0; i < NB_JUMP; ++i) {
      jumpDistance[i].Rand(jumpBit);
      if(jumpDistance[i].IsZero())
        jumpDistance[i].SetInt32(1);
      totalDist.Add(&jumpDistance[i]);
    }
    distAvg = totalDist.ToDouble() / (double)(NB_JUMP);
    ok = distAvg>minAvg && distAvg<maxAvg;
    maxRetry--;
  }

  for(int i = 0; i < NB_JUMP; ++i) {
    Point J = secp->ComputePublicKey(&jumpDistance[i]);
    jumpPointx[i].Set(&J.x);
    jumpPointy[i].Set(&J.y);
  }

  if(!ok) {
    ::printf("Warning, jump Avg distance out of bounds: 2^%.2f (restart the program)\n",log2(distAvg));
  } else {
    ::printf("Jump Avg distance: 2^%.2f\n",log2(distAvg));
  }

}

// ----------------------------------------------------------------------------

void Kangaroo::ComputeExpected(double dp,double *op,double *ram) {

  // Compute expected number of operation and memory

#ifdef USE_SYMMETRY
  double gainS = 1.0 / sqrt(2.0);
#else
  double gainS = 1.0;
#endif

  // Kangaroo number
  double k = (double)totalRW;

  // Range size
  double N = pow(2.0,(double)rangePower);

  // theta
  double theta = pow(2.0,dp);

  // Average for DP = 0
  double avgDP0 = (2.0 * (2.0 - sqrt(2.0)) * gainS) * sqrt(M_PI) * sqrt(N);

  // DP Overhead (for small number of kangaroo)
  double overSmallK = k * theta;

  // Average estimation (when k >> theta)
  double overLargeK = pow(16.0 * N * theta * k,1.0 / 3.0);

  double avg =  avgDP0 + overSmallK;
  if(overLargeK>avg)
    *op = overLargeK;
  else
    *op = avg;

  *ram = (double)sizeof(HASH_ENTRY) * HASH_SIZE +
         (double)(sizeof(ENTRY) + sizeof(ENTRY *)) * (*op / theta);

  *ram /= (1024.0*1024.0);

}

// ----------------------------------------------------------------------------

void Kangaroo::Run(int nbThread,std::vector<int> gpuId,std::vector<int> gridSize) {

  double t0 = Timer::get_tick();

  nbCPUThread = nbThread;
  nbGPUThread = (useGpu ? (int)gpuId.size() : 0);
  endOfSearch = false;
  totalRW = 0;

#ifndef WITHGPU

  if(nbGPUThread>0) {
    ::printf("GPU code not compiled, use -DWITHGPU when compiling.\n");
    nbGPUThread = 0;
  }

#endif

  TH_PARAM *params = (TH_PARAM *)malloc((nbCPUThread + nbGPUThread) * sizeof(TH_PARAM));
  THREAD_HANDLE *thHandles = (THREAD_HANDLE *)malloc((nbCPUThread + nbGPUThread) * sizeof(THREAD_HANDLE));

  memset(params, 0,(nbCPUThread + nbGPUThread) * sizeof(TH_PARAM));
  memset(counters, 0, sizeof(counters));
  ::printf("Number of CPU thread: %d\n", nbCPUThread);

  // Set starting parameters
  rangeWidth.Set(&rangeEnd);
  rangeWidth.Sub(&rangeStart);
  rangePower = rangeWidth.GetBitLength();
  ::printf("Range width: 2^%d\n",rangePower);
  rangeWidthDiv2.Set(&rangeWidth);
  rangeWidthDiv2.ShiftR(1);
  rangeWidthDiv4.Set(&rangeWidthDiv2);
  rangeWidthDiv4.ShiftR(1);
  rangeWidthDiv8.Set(&rangeWidthDiv4);
  rangeWidthDiv8.ShiftR(1);

  CreateJumpTable();

#define STATS
#ifdef STATS

  CPU_GRP_SIZE = 1024;
  for(; CPU_GRP_SIZE <= 1024; CPU_GRP_SIZE *=4) {

    uint64_t totalCount = 0;
    uint64_t totalDead = 0;

#endif

#ifdef WITHGPU

    // Compute grid size
    for(int i = 0; i < nbGPUThread; i++) {
      int x = gridSize[2 * i];
      int y = gridSize[2 * i + 1];
      if(!GPUEngine::GetGridSize(gpuId[i],&x,&y)) {
        return;
      } else {
        params[nbCPUThread + i].gridSizeX = x;
        params[nbCPUThread + i].gridSizeY = y;
      }
      totalRW += GPU_GRP_SIZE * x*y;
    }

#endif

    // Compute optimal distinguished bits number (see README)
    totalRW += nbCPUThread * CPU_GRP_SIZE;
    int suggestedDP = (int)((double)rangePower / 2.0 - log2((double)totalRW));
    if(suggestedDP < 0) suggestedDP = 0;

    //if(initDPSize > suggestedDP) {
    //  ::printf("Warning, DP is too large, it may cause significant overload.\n");
    //  ::printf("Hint: decrease number of threads, gridSize, or decrease dp using -d.\n");
    //}

    if(initDPSize < 0)
      initDPSize = suggestedDP;

    ComputeExpected((double)initDPSize,&expectedNbOp,&expectedMem);

    ::printf("Number of kangaroos: 2^%.2f\n",log2((double)totalRW));
    ::printf("Suggested DP: %d\n",suggestedDP);
    ::printf("Expected operations: 2^%.2f\n",log2(expectedNbOp));
    ::printf("Expected RAM: %.1fMB\n",expectedMem);

    SetDP(initDPSize);

    for(keyIdx = 0; keyIdx < keysToSearch.size(); keyIdx++) {

      Int SP;
      SP.Set(&rangeStart);
#ifdef USE_SYMMETRY
      SP.ModAddK1order(&rangeWidthDiv2);
#endif
      Point RS = secp->ComputePublicKey(&SP);
      RS.y.ModNeg();
      keyToSearch = secp->AddDirect(keysToSearch[keyIdx] ,RS);
      keyToSearchNeg = keyToSearch;
      keyToSearchNeg.y.ModNeg();

      endOfSearch = false;
      collisionInSameHerd = 0;
      maxStep = 1ULL<<63;

      // Lanch CPU threads
      for(int i = 0; i < nbCPUThread; i++) {
        params[i].threadId = i;
        params[i].isRunning = true;
        thHandles[i] = LaunchThread(_SolveKeyCPU,params + i);
      }

#ifdef WITHGPU

      // Launch GPU threads
      for(int i = 0; i < nbGPUThread; i++) {
        int id = nbCPUThread + i;
        params[id].threadId = 0x80L + i;
        params[id].isRunning = true;
        params[id].gpuId = gpuId[i];
        thHandles[id] = LaunchThread(_SolveKeyGPU,params + id);
      }

#endif

      // Wait for end
      Process(params,"MK/s");
      JoinThreads(thHandles,nbCPUThread + nbGPUThread);
      FreeHandles(thHandles,nbCPUThread + nbGPUThread);
      hashTable.Reset();

#ifdef STATS

      uint64_t count = getCPUCount() + getGPUCount();
      totalCount += count;
      totalDead += collisionInSameHerd;
      ::printf("[%3d] 2^%.3f Dead:%d Avg:2^%.3f DeadAvg:%.1f (2^%.3f)\n",
                              keyIdx, log2((double)count), collisionInSameHerd, 
                              log2((double)totalCount / (double)(keyIdx + 1)),
                              (double)totalDead / (double)(keyIdx + 1),
                              log2(expectedNbOp));
    }
    string fName = "DP" + ::to_string(dpSize) + ".txt";
    FILE *f = fopen(fName.c_str(),"a");
    fprintf(f,"%d %f\n",CPU_GRP_SIZE*nbCPUThread,(double)totalCount);
    fclose(f);

#endif

  }

  double t1 = Timer::get_tick();

  ::printf("\nDone: Total time %s \n" , GetTimeStr(t1-t0).c_str());

}


