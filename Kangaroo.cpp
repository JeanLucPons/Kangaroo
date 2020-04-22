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

Int jumpDistance[129];
Point jumpPoint[129];

Kangaroo::Kangaroo(Secp256K1 *secp,int32_t initDPSize,bool useGpu) {

  this->secp = secp;
  this->initDPSize = initDPSize;
  this->useGpu = useGpu;

  // Kangaroo jumps
  jumpPoint[0] = secp->G;
  jumpDistance[0].SetInt32(1);
  for(int i = 1; i < 129; ++i) {
    jumpDistance[i].Add(&jumpDistance[i - 1],&jumpDistance[i - 1]);
    jumpPoint[i] = secp->DoubleDirect(jumpPoint[i - 1]);
  }

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
  }
  else {
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

void Kangaroo::Check(std::vector<int> gpuId,std::vector<int> gridSize) {

  initDPSize = 8;
  SetDP(initDPSize);
  jumpModulo = 128;
  rangePower = 256;

#ifdef WITHGPU

  // Check gpu
  if(useGpu) {

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

    vector<KANGAROO *> K;
    Int *px = new Int[nb];
    Int *py = new Int[nb];
    Int *d = new Int[nb];
    vector<ITEM> gpuFound;

    Int pk;
    pk.Rand(256);
    keyToSearch = secp->ComputePublicKey(&pk);

    for(int i=0;i<nb;i++) {
      K.push_back(Create(i%2));
      px[i].Set(&K[i]->pos.x);
      py[i].Set(&K[i]->pos.y);
      d[i].Set(&K[i]->distance);
    }

    h.SetParams(dMask,jumpModulo);

    h.SetKangaroos(px,py,d,false);

    // Test single
    uint64_t r = rndl() % nb;
    K[r] = Create(r%2);
    h.SetKangaroo(r,&K[r]->pos.x,&K[r]->pos.y,&K[r]->distance);

    h.Launch(gpuFound);
    h.GetKangaroos(px,py,d);
    h.Launch(gpuFound);
    ::printf("DP found: %d\n",(int)gpuFound.size());

    // Do the same on CPU
    for(int r=0;r<NB_RUN;r++) {
      for(int i=0;i<nb;i++) {
        uint64_t jmp = (K[i]->pos.x.bits64[0] % jumpModulo);
        K[i]->pos = secp->AddDirect(K[i]->pos,jumpPoint[jmp]);
        K[i]->distance.ModAddK1order(&jumpDistance[jmp]);

        if( IsDP(K[i]->pos.x.bits64[3])) {

          // Search for DP found
          bool found = false;
          int j = 0;
          while(!found && j<(int)gpuFound.size()) {            
            found = gpuFound[j].x.IsEqual( &K[i]->pos.x ) &&
                    gpuFound[j].d.IsEqual( &K[i]->distance ) &&
                    gpuFound[j].kIdx == (uint64_t)i;
            if(!found) j++;
          }

          if(found) {
            gpuFound.erase(gpuFound.begin()+j);
          } else {
            ::printf("DP Mismatch:\n");
#ifdef WIN64
            ::printf("[%d] %s [0x%016I64X]\n",j,gpuFound[j].x.GetBase16().c_str(),gpuFound[j].kIdx);
#else
            ::printf("[%d] %s [0x%" PRIx64 "]\n",j,gpuFound[j].x.GetBase16().c_str(),gpuFound[j].kIdx);
#endif
            ::printf("[%d] %s \n",i,K[gpuFound[i].kIdx]->pos.x.GetBase16().c_str());
            return;
          }

        }

      }
    }

    // Compare kangaroos
    int nbFault = 0;
    bool firstFaut = true;
    for(int i = 0; i<nb; i++) {
      bool ok = px[i].IsEqual(&K[i]->pos.x) && 
                py[i].IsEqual(&K[i]->pos.y) &&
                d[i].IsEqual(&K[i]->distance);
      if(!ok) {
        nbFault++;
        if(firstFaut) {
          ::printf("CPU Kx=%s\n",K[i]->pos.x.GetBase16().c_str());
          ::printf("CPU Ky=%s\n",K[i]->pos.y.GetBase16().c_str());
          ::printf("CPU Kd=%s\n",K[i]->distance.GetBase16().c_str());
          ::printf("GPU Kx=%s\n",px[i].GetBase16().c_str());
          ::printf("GPU Ky=%s\n",py[i].GetBase16().c_str());
          ::printf("GPU Kd=%s\n",d[i].GetBase16().c_str());
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

// ----------------------------------------------------------------------------

void Kangaroo::SolveKeyCPU(TH_PARAM *ph) {

  // Global init
  int thId = ph->threadId;
  counters[thId] = 0;

  // Create Kangaroos
  KANGAROO *herd[CPU_GRP_SIZE];
  for(int j = 0; j<CPU_GRP_SIZE; j++)
    herd[j] = Create(j%2);

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

    for(int g = 0; g < CPU_GRP_SIZE; g++) {

      uint64_t jmp = herd[g]->pos.x.bits64[0] % jumpModulo;
      Int *p1x = &jumpPoint[jmp].x;
      Int *p2x = &herd[g]->pos.x;
      dx[g].ModSub(p2x,p1x);

    }
    grp->Set(dx);
    grp->ModInv();

    for(int g = 0; g < CPU_GRP_SIZE; g++) {

      uint64_t jmp = herd[g]->pos.x.bits64[0] % jumpModulo;
      Int *p1x = &jumpPoint[jmp].x;
      Int *p1y = &jumpPoint[jmp].y;
      Int *p2x = &herd[g]->pos.x;
      Int *p2y = &herd[g]->pos.y;

      dy.ModSub(p2y,p1y);
      _s.ModMulK1(&dy,&dx[g]);
      _p.ModSquareK1(&_s);

      rx.ModSub(&_p,p1x);
      rx.ModSub(p2x);

      ry.ModSub(p2x,&rx);
      ry.ModMulK1(&_s);
      ry.ModSub(p2y);

      herd[g]->pos.x.Set(&rx);
      herd[g]->pos.y.Set(&ry);
      herd[g]->distance.ModAddK1order(&jumpDistance[jmp]);

    }

    for(int g = 0; g < CPU_GRP_SIZE; g++) {
      if(IsDP(herd[g]->pos.x.bits64[3])) {
        LOCK(ghMutex);
        if(!endOfSearch) {
          if(hashTable.Add(&herd[g]->pos.x,&herd[g]->distance,herd[g]->type)) {

            int type = hashTable.GetType();

            if(type == herd[g]->type) {
              
              // Collision inside the same herd
              // We need to reset the kangaroo
              free(herd[g]);
              herd[g] = Create(type,false);
              collisionInSameHerd++;

            } else {

              // K = startRange + dtame - dwild
              Int pk(&rangeStart);

              if(herd[g]->type==TAME) {
                pk.ModAddK1order(&herd[g]->distance);
                pk.ModSubK1order(hashTable.GetD());
              } else {
                pk.ModAddK1order(hashTable.GetD());
                pk.ModSubK1order(&herd[g]->distance);
              }
              
              Point P = secp->ComputePublicKey(&pk);

              if(P.equals(keyToSearch)) {

                // Key solved
                ::printf("\nKey#%2d Pub:  0x%s \n",keyIdx,secp->GetPublicKeyHex(true,P).c_str());
                ::printf("       Priv: 0x%s \n",pk.GetBase16().c_str());
                endOfSearch = true;

              } else {
                
                // We may have the symetric key
                pk.Neg();
                pk.Add(&secp->order);
                P = secp->ComputePublicKey(&pk);
                if(P.equals(keyToSearch)) {
                  // Key solved
                  ::printf("\nKey#%2d Pub:  0x%s \n",keyIdx,secp->GetPublicKeyHex(true,P).c_str());
                  ::printf("       Priv: 0x%s \n",pk.GetBase16().c_str());
                  endOfSearch = true;
                } else {
                  ::printf("\n Unexpected wrong collision, reset kangaroo !\n");
                  // Should not happen, reset the kangaroo
                  free(herd[g]);
                  herd[g] = Create(type);
                }

              }

            }


          }
        }
        UNLOCK(ghMutex);
      }
    }

    counters[thId] += CPU_GRP_SIZE;

  }

  // Free
  delete grp;
  delete dx;
  for(int j = 0; j<CPU_GRP_SIZE; j++)
    free(herd[j]);

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
  printf("GPU: %s (%.1f MB used)\n",gpu->deviceName.c_str(),gpu->GetMemory() / 1048576.0);

  if(keyIdx == 0)
    ::printf("SolveKeyGPU Thread GPU#%d: creating kangaroos...\n",ph->gpuId);

  // Create Kangaroos
  uint64_t nbKangaroo = gpu->GetNbThread() * GPU_GRP_SIZE;
  px = new Int[nbKangaroo];
  py = new Int[nbKangaroo];
  d = new Int[nbKangaroo];

  for(int j = 0; j<nbKangaroo; j++) {
    KANGAROO *K = Create(j % 2);
    px[j].Set(&K->pos.x);
    py[j].Set(&K->pos.y);
    d[j].Set(&K->distance);
    free(K);
  }

  gpu->SetParams(dMask,jumpModulo);
  gpu->SetKangaroos(px,py,d);
  gpu->callKernel();

  if(keyIdx == 0)
    ::printf("SolveKeyGPU Thread GPU#%d: 2^%.2f kangaroos\n",ph->gpuId,log2((double)nbKangaroo));

  ph->hasStarted = true;

  while(!endOfSearch) {

    gpu->Launch(gpuFound);
    counters[thId] += nbKangaroo * NB_RUN;

    if(gpuFound.size() > 0) {
      
      LOCK(ghMutex);

      for(int g=0;!endOfSearch && g<gpuFound.size();g++) {

        uint32_t kType = (uint32_t)(gpuFound[g].kIdx % 2);

        if(hashTable.Add(&gpuFound[g].x,&gpuFound[g].d,kType)) {

          uint32_t type = hashTable.GetType();

          if(type == kType) {

            // Collision inside the same herd
            // We need to reset the kangaroo
            KANGAROO *K = Create(kType,false);
            gpu->SetKangaroo(gpuFound[g].kIdx,&K->pos.x,&K->pos.y,&K->distance);
            free(K);
            collisionInSameHerd++;

          } else {

            // K = startRange + dtame - dwild
            Int pk(&rangeStart);

            if(kType == TAME) {
              pk.ModAddK1order(&gpuFound[g].d);
              pk.ModSubK1order(hashTable.GetD());
            } else {
              pk.ModAddK1order(hashTable.GetD());
              pk.ModSubK1order(&gpuFound[g].d);
            }

            Point P = secp->ComputePublicKey(&pk);

            if(P.equals(keyToSearch)) {

              // Key solved
              ::printf("\nKey#%2d Pub:  0x%s \n",keyIdx,secp->GetPublicKeyHex(true,P).c_str());
              ::printf("       Priv: 0x%s \n",pk.GetBase16().c_str());
              endOfSearch = true;

            } else {

              // We may have the symetric key
              pk.Neg();
              pk.Add(&secp->order);
              P = secp->ComputePublicKey(&pk);
              if(P.equals(keyToSearch)) {
                // Key solved
                ::printf("\nKey#%2d Pub:  0x%s \n",keyIdx,secp->GetPublicKeyHex(true,P).c_str());
                ::printf("       Priv: 0x%s \n",pk.GetBase16().c_str());
                endOfSearch = true;
              }
              else {
                ::printf("\n Unexpected wrong collision, reset kangaroo !\n");
                // Should not happen, reset the kangaroo
                KANGAROO *K = Create(kType);
                gpu->SetKangaroo(gpuFound[g].kIdx,&K->pos.x,&K->pos.y,&K->distance);
                free(K);
              }

            }

          }


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

KANGAROO *Kangaroo::Create(int type,bool lock) {

  // pos of WILD kangooro is keyToSolve + distance.G
  // pos of TAME kangooro is (startRange + distance).G

  KANGAROO *k = new KANGAROO;
  
  if(lock) LOCK(ghMutex);
  k->distance.Rand(rangePower);
  if(lock) UNLOCK(ghMutex);

  if( type==TAME ) {

    Int pk(&k->distance);
    pk.Add(&rangeStart);
    k->pos = secp->ComputePublicKey(&pk);
    k->type = TAME;

  } else {

    Point o = secp->ComputePublicKey(&k->distance);
    k->pos = secp->AddDirect(keyToSearch,o);
    k->type = WILD;

  }

  return k;

}

// ----------------------------------------------------------------------------

void Kangaroo::Run(int nbThread,std::vector<int> gpuId,std::vector<int> gridSize) {

  double t0 = Timer::get_tick();

  nbCPUThread = nbThread;
  nbGPUThread = (useGpu ? (int)gpuId.size() : 0);
  endOfSearch = false;
  uint64_t totalRW = 0;

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

#ifdef WITHGPU

  // Compute grid size
  for(int i = 0; i < nbGPUThread; i++) {
    int x = gridSize[2 * i];
    int y = gridSize[2 * i + 1];
    if(!GPUEngine::GetGridSize(gpuId[i],&x,&y)) {
      return;
    }
    else {
      params[nbCPUThread + i].gridSizeX = x;
      params[nbCPUThread + i].gridSizeY = y;
    }
    totalRW += GPU_GRP_SIZE * x*y;
  }

#endif

  // Set starting parameters
  Int range(&rangeEnd);
  range.Sub(&rangeStart);
  rangePower = range.GetBitLength();
  ::printf("Range width: 2^%d\n",rangePower);
  jumpModulo = rangePower/2 + 1;
  if(jumpModulo>129) jumpModulo = 129;

  // Compute optimal distinguished bits number.
  // If dp is too large comparing to the total number of parallel random walks
  // an overload appears due to the fact that computed paths become too short
  // and decrease significantly the probability that distiguised points collide 
  // inside the centralized hash table.
  totalRW += nbCPUThread * CPU_GRP_SIZE;
  int optimalDP = (int)((double)rangePower / 2.0 - log2((double)totalRW) - 2);
  if(optimalDP < 0) optimalDP = 0;
  ::printf("Number of random walk: 2^%.2f (Max DP=%d)\n",log2((double)totalRW),optimalDP);

  if(initDPSize > optimalDP) {
    ::printf("Warning, DP is too large, it may cause significant overload.\n");
    ::printf("Hint: decrease number of threads, gridSize, or decrese dp using -d.\n");
  }
  if(initDPSize < 0)
    initDPSize = optimalDP;

  SetDP(initDPSize);

  for(keyIdx =0; keyIdx<keysToSearch.size(); keyIdx++) {

    keyToSearch = keysToSearch[keyIdx];
    endOfSearch = false;
    collisionInSameHerd = 0;

    // Lanch CPU threads
    for(int i=0; i < nbCPUThread; i++) {
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
    Process(params,"MKey/s");
    JoinThreads(thHandles,nbCPUThread + nbGPUThread);
    FreeHandles(thHandles,nbCPUThread + nbGPUThread);
    hashTable.Reset();

  }

  double t1 = Timer::get_tick();

  ::printf("\nDone: Total time %s \n" , GetTimeStr(t1-t0).c_str());

}


