/*
 * This file is part of the BSGS distribution (https://github.com/JeanLucPons/BSGS).
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

Int jumpDistance[256];
Point jumpPoint[256];

Kangaroo::Kangaroo(Secp256K1 *secp,int32_t initDPSize) {

  this->secp = secp;
  this->initDPSize = initDPSize;

  // Kangaroo jumps
  jumpPoint[0] = secp->G;
  jumpDistance[0].SetInt32(1);
  for(int i = 1; i < 256; ++i) {
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

void Kangaroo::SolveKey(TH_PARAM *ph) {

  // Global init
  int thId = ph->threadId;
  ph->hasStarted = true;
  counters[thId] = 0;

  if(keyIdx==0) {
    if(ph->herd[0]->type==TAME)
      ::printf("Solvekey Thread %d: %d TAME kangaroos\n",ph->threadId,CPU_GRP_SIZE);
    else
      ::printf("Solvekey Thread %d: %d WILD kangaroos\n",ph->threadId,CPU_GRP_SIZE);
  }

  IntGroup *grp = new IntGroup(CPU_GRP_SIZE);
  Int *dx = new Int[CPU_GRP_SIZE];

  // Affine coord
  Int dy;
  Int rx;
  Int ry;
  Int _s;
  Int _p;

  while(!endOfSearch) {

    for(int g = 0; g < CPU_GRP_SIZE; g++) {

      uint64_t jmp = ph->herd[g]->pos.x.bits64[0] % jumpModulo;
      Int *p1x = &jumpPoint[jmp].x;
      Int *p2x = &ph->herd[g]->pos.x;
      dx[g].ModSub(p2x,p1x);

    }
    grp->Set(dx);
    grp->ModInv();

    for(int g = 0; g < CPU_GRP_SIZE; g++) {

      uint64_t jmp = ph->herd[g]->pos.x.bits64[0] % jumpModulo;
      Int *p1x = &jumpPoint[jmp].x;
      Int *p1y = &jumpPoint[jmp].y;
      Int *p2x = &ph->herd[g]->pos.x;
      Int *p2y = &ph->herd[g]->pos.y;

      dy.ModSub(p2y,p1y);
      _s.ModMulK1(&dy,&dx[g]);
      _p.ModSquareK1(&_s);

      rx.ModSub(&_p,p1x);
      rx.ModSub(p2x);

      ry.ModSub(p2x,&rx);
      ry.ModMulK1(&_s);
      ry.ModSub(p2y);

      ph->herd[g]->pos.x.Set(&rx);
      ph->herd[g]->pos.y.Set(&ry);
      ph->herd[g]->distance.Add(&jumpDistance[jmp]);

    }

    for(int g = 0; g < CPU_GRP_SIZE; g++) {
      if(IsDP(ph->herd[g]->pos.x.bits64[3])) {
        LOCK(ghMutex);
        if(!endOfSearch) {
          if(hashTable.Add(&ph->herd[g]->pos.x,ph->herd[g])) {

            int type = hashTable.GetType();

            if(type == ph->herd[g]->type) {
              
              // Collision inside the same herd
              // We need to reset the kangaroo
              free(ph->herd[g]);
              ph->herd[g] = Create(type);
              collisionInSameHerd++;

            } else {

              // K = startRange + dtame - dwild
              Int pk(&rangeStart);

              if(ph->herd[g]->type==TAME) {
                pk.ModAddK1order(&ph->herd[g]->distance);
                pk.ModSubK1order(hashTable.GetD());
              } else {
                pk.ModAddK1order(hashTable.GetD());
                pk.ModSubK1order(&ph->herd[g]->distance);
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
                  free(ph->herd[g]);
                  ph->herd[g] = Create(type);
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

  delete grp;
  delete dx;
  ph->isRunning = false;

}

// ----------------------------------------------------------------------------

#ifdef WIN64
DWORD WINAPI _SolveKey(LPVOID lpParam) {
#else
void *_SolveKey(void *lpParam) {
#endif
  TH_PARAM *p = (TH_PARAM *)lpParam;
  p->obj->SolveKey(p);
  return 0;
}

// ----------------------------------------------------------------------------

KANGAROO *Kangaroo::Create(int type) {

  // pos of WILD kangooro is keyToSolve + distance.G
  // pos of TAME kangooro is (startRange + distance).G

  KANGAROO *k = new KANGAROO;
  
  if( type==TAME ) {

    k->distance.Rand(rangePower);
    Int pk(&k->distance);
    pk.Add(&rangeStart);
    k->pos = secp->ComputePublicKey(&pk);
    k->type = TAME;

  } else {

    k->distance.Rand(rangePower);
    Point o = secp->ComputePublicKey(&k->distance);
    k->pos = secp->AddDirect(keyToSearch,o);
    k->type = WILD;

  }

  return k;

}

// ----------------------------------------------------------------------------

void Kangaroo::Run(int nbThread) {

  double t0 = Timer::get_tick();

  nbCPUThread = nbThread;
  endOfSearch = false;

  TH_PARAM *params = (TH_PARAM *)malloc(nbCPUThread * sizeof(TH_PARAM));
  THREAD_HANDLE *thHandles = (THREAD_HANDLE *)malloc(nbCPUThread * sizeof(THREAD_HANDLE));

  memset(params, 0, nbCPUThread * sizeof(TH_PARAM));
  memset(counters, 0, sizeof(counters));
  ::printf("Number of CPU thread: %d\n", nbCPUThread);

  // Set starting parameters
  Int range(&rangeEnd);
  range.Sub(&rangeStart);
  rangePower = range.GetBitLength();
  ::printf("Range width: 2^%d\n",rangePower);
  jumpModulo = rangePower/2 + 1;

  // Compute optimal distinguished bits number.
  // If dp is too large comparing to the total number of parallel random walks
  // an overload appears due to the fact that computed paths become too short
  // and decrease significantly the probability that distiguised points collide 
  // inside the centralized hash table.
  uint64_t totalRW = nbCPUThread * CPU_GRP_SIZE;
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

    // Lanch Tame Kangoro threads

    int i = 0;
    for(; i < nbCPUThread/2; i++) {

      for(int j=0;j<CPU_GRP_SIZE;j++)
        params[i].herd[j] = Create(TAME);      
      params[i].threadId = i;
      params[i].isRunning = true;
      thHandles[i] = LaunchThread(_SolveKey,params + i);

    }

    // Lanch Wild Kangoro threads

    for(; i < nbCPUThread; i++) {

      for(int j = 0; j<CPU_GRP_SIZE; j++)
        params[i].herd[j] = Create(WILD);
      params[i].threadId = i;
      params[i].isRunning = true;
      thHandles[i] = LaunchThread(_SolveKey,params + i);

    }

    // Wait for end
    Process(params,"MKey/s");
    JoinThreads(thHandles,nbCPUThread);
    FreeHandles(thHandles,nbCPUThread);

    // Free
    for(i = 0; i < nbCPUThread; i++) {
      for(int j = 0; j<CPU_GRP_SIZE; j++)
        free(params[i].herd[j]);
    }
    hashTable.Reset();

  }

  double t1 = Timer::get_tick();

  ::printf("\nDone: Total time %s \n" , GetTimeStr(t1-t0).c_str());

}


