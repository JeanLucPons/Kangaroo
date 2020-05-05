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

void Kangaroo::Check(std::vector<int> gpuId,std::vector<int> gridSize) {

  initDPSize = 8;
  SetDP(initDPSize);
  rangePower = 64;

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

    vector<KANGAROO> K;
    Int *px = new Int[nb];
    Int *py = new Int[nb];
    Int *d = new Int[nb];
    vector<ITEM> gpuFound;

    Int pk;
    pk.Rand(256);
    keyToSearch = secp->ComputePublicKey(&pk);

    for(int i = 0; i<nb; i++) {
      KANGAROO k;
      Create(&k,i % 2);
      K.push_back(k);
      px[i].Set(&K[i].pos.x);
      py[i].Set(&K[i].pos.y);
      d[i].Set(&K[i].distance);
    }


    CreateJumpTable();

    h.SetParams(dMask,jumpDistance,jumpPointx,jumpPointy);

    h.SetKangaroos(px,py,d,false);

    // Test single
    uint64_t r = rndl() % nb;
    Create(&K[r],r % 2);
    h.SetKangaroo(r,&K[r].pos.x,&K[r].pos.y,&K[r].distance);

    h.Launch(gpuFound);
    h.GetKangaroos(px,py,d);
    h.Launch(gpuFound);
    ::printf("DP found: %d\n",(int)gpuFound.size());

    // Do the same on CPU
    Int _1;
    _1.SetInt32(1);
    for(int r = 0; r<NB_RUN; r++) {
      for(int i = 0; i<nb; i++) {
        uint64_t jmp = (K[i].pos.x.bits64[0] % NB_JUMP);

#ifdef USE_SYMMETRY
        // Limit cycle
        if(jmp == K[i].lastJump) jmp = (K[i].lastJump + 1) % NB_JUMP;
#endif

        Point J(&jumpPointx[jmp],&jumpPointy[jmp],&_1);
        K[i].pos = secp->AddDirect(K[i].pos,J);
        K[i].distance.ModAddK1order(&jumpDistance[jmp]);

#ifdef USE_SYMMETRY
        // Equivalence symmetry class switch
        if(K[i].pos.y.ModPositiveK1())
          K[i].distance.ModNegK1order();
        K[i].lastJump = jmp;
#endif

        if(IsDP(K[i].pos.x.bits64[3])) {

          // Search for DP found
          bool found = false;
          int j = 0;
          while(!found && j<(int)gpuFound.size()) {
            found = gpuFound[j].x.IsEqual(&K[i].pos.x) &&
              gpuFound[j].d.IsEqual(&K[i].distance) &&
              gpuFound[j].kIdx == (uint64_t)i;
            if(!found) j++;
          }

          if(found) {
            gpuFound.erase(gpuFound.begin() + j);
          }
          else {
            ::printf("DP Mismatch:\n");
#ifdef WIN64
            ::printf("[%d] %s [0x%016I64X]\n",j,gpuFound[j].x.GetBase16().c_str(),gpuFound[j].kIdx);
#else
            ::printf("[%d] %s [0x%" PRIx64 "]\n",j,gpuFound[j].x.GetBase16().c_str(),gpuFound[j].kIdx);
#endif
            ::printf("[%d] %s \n",i,K[gpuFound[i].kIdx].pos.x.GetBase16().c_str());
            return;
          }

        }

      }
    }

    // Compare kangaroos
    int nbFault = 0;
    bool firstFaut = true;
    for(int i = 0; i<nb; i++) {
      bool ok = px[i].IsEqual(&K[i].pos.x) &&
        py[i].IsEqual(&K[i].pos.y) &&
        d[i].IsEqual(&K[i].distance);
      if(!ok) {
        nbFault++;
        if(firstFaut) {
          ::printf("CPU Kx=%s\n",K[i].pos.x.GetBase16().c_str());
          ::printf("CPU Ky=%s\n",K[i].pos.y.GetBase16().c_str());
          ::printf("CPU Kd=%s\n",K[i].distance.GetBase16().c_str());
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
