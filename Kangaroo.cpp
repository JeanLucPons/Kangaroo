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

#define safe_delete_array(x) if(x) {delete[] x;x=NULL;}

// ----------------------------------------------------------------------------

Kangaroo::Kangaroo(Secp256K1 *secp,int32_t initDPSize,bool useGpu,string &workFile,string &iWorkFile,uint32_t savePeriod,bool saveKangaroo,bool saveKangarooByServer,
                   double maxStep,int wtimeout,int port,int ntimeout,string serverIp,string outputFile,bool splitWorkfile) {

  this->secp = secp;
  this->initDPSize = initDPSize;
  this->useGpu = useGpu;
  this->offsetCount = 0;
  this->offsetTime = 0.0;
  this->workFile = workFile;
  this->saveWorkPeriod = savePeriod;
  this->inputFile = iWorkFile;
  this->nbLoadedWalk = 0;
  this->clientMode = serverIp.length() > 0;
  this->saveKangarooByServer = this->clientMode && saveKangarooByServer;
  this->saveKangaroo = saveKangaroo || this->saveKangarooByServer;
  this->fRead = NULL;
  this->maxStep = maxStep;
  this->wtimeout = wtimeout;
  this->port = port;
  this->ntimeout = ntimeout;
  this->serverIp = serverIp;
  this->outputFile = outputFile;
  this->hostInfo = NULL;
  this->endOfSearch = false;
  this->saveRequest = false;
  this->connectedClient = 0;
  this->totalRW = 0;
  this->collisionInSameHerd = 0;
  this->keyIdx = 0;
  this->splitWorkfile = splitWorkfile;
  this->pid = Timer::getPID();
  this->isStride = false;
  this->isChecksum = false;

  CPU_GRP_SIZE = 1024;

  // Init mutex
#ifdef WIN64
  ghMutex = CreateMutex(NULL,FALSE,NULL);
  saveMutex = CreateMutex(NULL,FALSE,NULL);
#else
  pthread_mutex_init(&ghMutex, NULL);
  pthread_mutex_init(&saveMutex, NULL);
  signal(SIGPIPE, SIG_IGN);
#endif

}

// ----------------------------------------------------------------------------

void Kangaroo::SetStride(std::string &stride){
//    ::printf("Stride %s\n", stride.c_str());
    Int _stride;
    _stride.SetBase16((char *)stride.c_str());
    secp->SetStride(&_stride, &rangeStart, &rangeEnd);
    this->isStride = true;
    this->stride = _stride;
}

// ----------------------------------------------------------------------------

void Kangaroo::SetChecksum(std::string &checksum){
//    ::printf("Stride %s\n", stride.c_str());
    Int _checksum;
    _checksum.SetBase16((char *)checksum.c_str());
    secp->SetChecksum(&_checksum);
    this->isChecksum = true;
    this->checksum = _checksum;
}


// ----------------------------------------------------------------------------

bool Kangaroo::ParseConfigFile(std::string &fileName) {

  // In client mode, config come from the server
  if(clientMode)
    return true;

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

bool Kangaroo::Output(Int *pk,char sInfo,int sType) {


  FILE* f = stdout;
  bool needToClose = false;

  if(outputFile.length() > 0) {
    f = fopen(outputFile.c_str(),"a");
    if(f == NULL) {
      printf("Cannot open %s for writing\n",outputFile.c_str());
      f = stdout;
    }
    else {
      needToClose = true;
    }
  }

  if(!needToClose)
    ::printf("\n");

::fprintf(f," verify PK %s  \n", pk->GetBase16().c_str());
  Point PR = secp->ComputePublicKey(pk);

  ::fprintf(f,"Key#%2d [%d%c]Pub:  0x%s \n",keyIdx,sType,sInfo,secp->GetPublicKeyHex(true,keysToSearch[keyIdx]).c_str());
  if(PR.equals(keysToSearch[keyIdx])) {
    ::fprintf(f,"       Priv: 0x%s \n",pk->GetBase16().c_str());
    if (this->isStride){
      Int realK = pk;
      realK.Sub(&rangeStart);
      realK.Mult(&stride);
      if (this->isChecksum){
              Int rangeInitWChecksum;
              rangeInitWChecksum.SetBase16((char *)rangeStart.GetBase16().append(checksum.GetBase16()).c_str());
              realK.Add(&rangeInitWChecksum);
              realK.SetBase16((char *)realK.GetBase16().substr(0, realK.GetBase16().length()-8).c_str());
      }else{
        realK.Add(&rangeStart);
      }
      ::fprintf(f,"   RealPriv: 0x%s \n",realK.GetBase16().c_str());
    }
  } else {
    ::fprintf(f,"       Failed !\n");
    if(needToClose)
      fclose(f);
    return false;
  }


  if(needToClose)
    fclose(f);

  return true;

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
    return Output(&pk,'N',type);
  }

  if(P.equals(keyToSearchNeg)) {
    // Key solved
    pk.ModNegK1order();
#ifdef USE_SYMMETRY
    pk.ModAddK1order(&rangeWidthDiv2);
#endif
    pk.ModAddK1order(&rangeStart);
    return Output(&pk,'S',type);
  }

  return false;

}

bool Kangaroo::CollisionCheck(Int* d1,uint32_t type1,Int* d2,uint32_t type2) {


  if(type1 == type2) {

    // Collision inside the same herd
    return false;

  } else {

    Int Td;
    Int Wd;

    if(type1 == TAME) {
      Td.Set(d1);
      Wd.Set(d2);
    }  else {
      Td.Set(d2);
      Wd.Set(d1);
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
        ::printf("Found: Wd-%s\n",Wd.GetBase16().c_str());
      } else {
        ::printf("Found: Wd %s\n",Wd.GetBase16().c_str());
      }
      return false;

    }

  }

  return true;

}

// ----------------------------------------------------------------------------

bool Kangaroo::AddToTable(Int *pos,Int *dist,uint32_t kType) {

  int addStatus = hashTable.Add(pos,dist,kType);
  if(addStatus== ADD_COLLISION)
    return CollisionCheck(&hashTable.kDist,hashTable.kType,dist,kType);

  return addStatus == ADD_OK;

}

bool Kangaroo::AddToTable(uint64_t h,int128_t *x,int128_t *d) {

  int addStatus = hashTable.Add(h,x,d);
  if(addStatus== ADD_COLLISION) {

    Int dist;
    uint32_t kType;
    HashTable::CalcDistAndType(*d,&dist,&kType);
    return CollisionCheck(&hashTable.kDist,hashTable.kType,&dist,kType);

  }

  return addStatus == ADD_OK;

}

// ----------------------------------------------------------------------------

void Kangaroo::SolveKeyCPU(TH_PARAM *ph) {

  vector<ITEM> dps;
  double lastSent = 0;

  // Global init
  int thId = ph->threadId;

  // Create Kangaroos
  ph->nbKangaroo = CPU_GRP_SIZE;

#ifdef USE_SYMMETRY
  ph->symClass = new uint64_t[CPU_GRP_SIZE];
  for(int i = 0; i<CPU_GRP_SIZE; i++) ph->symClass[i] = 0;
#endif

  IntGroup *grp = new IntGroup(CPU_GRP_SIZE);
  Int *dx = new Int[CPU_GRP_SIZE];

  if(ph->px==NULL) {

    // Create Kangaroos, if not already loaded
    ph->px = new Int[CPU_GRP_SIZE];
    ph->py = new Int[CPU_GRP_SIZE];
    ph->distance = new Int[CPU_GRP_SIZE];
    CreateHerd(CPU_GRP_SIZE,ph->px,ph->py,ph->distance,TAME);

  }

  if(keyIdx==0)
    ::printf("SolveKeyCPU Thread %d: %d kangaroos\n",ph->threadId,CPU_GRP_SIZE);

  ph->hasStarted = true;

  // Using Affine coord
  Int dy;
  Int rx;
  Int ry;
  Int _s;
  Int _p;

  while(!endOfSearch) {

    // Random walk

    for(int g = 0; g < CPU_GRP_SIZE; g++) {

#ifdef USE_SYMMETRY
      uint64_t jmp = ph->px[g].bits64[0] % (NB_JUMP/2) + (NB_JUMP / 2) * ph->symClass[g];
#else
      uint64_t jmp = ph->px[g].bits64[0] % NB_JUMP;
#endif

      Int *p1x = &jumpPointx[jmp];
      Int *p2x = &ph->px[g];
      dx[g].ModSub(p2x,p1x);

    }

    grp->Set(dx);
    grp->ModInv();

    for(int g = 0; g < CPU_GRP_SIZE; g++) {

#ifdef USE_SYMMETRY
      uint64_t jmp = ph->px[g].bits64[0] % (NB_JUMP / 2) + (NB_JUMP / 2) * ph->symClass[g];
#else
      uint64_t jmp = ph->px[g].bits64[0] % NB_JUMP;
#endif

      Int *p1x = &jumpPointx[jmp];
      Int *p1y = &jumpPointy[jmp];
      Int *p2x = &ph->px[g];
      Int *p2y = &ph->py[g];

      dy.ModSub(p2y,p1y);
      _s.ModMulK1(&dy,&dx[g]);
      _p.ModSquareK1(&_s);

      rx.ModSub(&_p,p1x);
      rx.ModSub(p2x);

      ry.ModSub(p2x,&rx);
      ry.ModMulK1(&_s);
      ry.ModSub(p2y);

      ph->distance[g].ModAddK1order(&jumpDistance[jmp]);

#ifdef USE_SYMMETRY
      // Equivalence symmetry class switch
      if( ry.ModPositiveK1() ) {
        ph->distance[g].ModNegK1order();
        ph->symClass[g] = !ph->symClass[g];
      }
#endif

      ph->px[g].Set(&rx);
      ph->py[g].Set(&ry);

    }

    if( clientMode ) {

      // Send DP to server
      for(int g = 0; g < CPU_GRP_SIZE; g++) {
        if(IsDP(ph->px[g].bits64[3])) {
          ITEM it;
          it.x.Set(&ph->px[g]);
          it.d.Set(&ph->distance[g]);
          it.kIdx = g;
          dps.push_back(it);
        }
      }

      double now = Timer::get_tick();
      if( now-lastSent > SEND_PERIOD ) {
        LOCK(ghMutex);
        SendToServer(dps,ph->threadId,0xFFFF);
        UNLOCK(ghMutex);
        lastSent = now;
      }

      if(!endOfSearch) counters[thId] += CPU_GRP_SIZE;

    } else {

      // Add to table and collision check
      for(int g = 0; g < CPU_GRP_SIZE && !endOfSearch; g++) {

        if(IsDP(ph->px[g].bits64[3])) {
          LOCK(ghMutex);
          if(!endOfSearch) {

            if(!AddToTable(&ph->px[g],&ph->distance[g],g % 2)) {
              // Collision inside the same herd
              // We need to reset the kangaroo
              CreateHerd(1,&ph->px[g],&ph->py[g],&ph->distance[g],g % 2,false);
              collisionInSameHerd++;
            }

          }
          UNLOCK(ghMutex);
        }

        if(!endOfSearch) counters[thId] ++;

      }

    }

    // Save request
    if(saveRequest && !endOfSearch) {
      ph->isWaiting = true;
      LOCK(saveMutex);
      ph->isWaiting = false;
      UNLOCK(saveMutex);
    }

  }

  // Free
  delete grp;
  delete[] dx;
  safe_delete_array(ph->px);
  safe_delete_array(ph->py);
  safe_delete_array(ph->distance);
#ifdef USE_SYMMETRY
  safe_delete_array(ph->symClass);
#endif

  ph->isRunning = false;

}

// ----------------------------------------------------------------------------

void Kangaroo::SolveKeyGPU(TH_PARAM *ph) {

  double lastSent = 0;

  // Global init
  int thId = ph->threadId;

#ifdef WITHGPU

  vector<ITEM> dps;
  vector<ITEM> gpuFound;
  GPUEngine *gpu;

  gpu = new GPUEngine(ph->gridSizeX,ph->gridSizeY,ph->gpuId,65536 * 2);

  if(keyIdx == 0)
    ::printf("GPU: %s (%.1f MB used)\n",gpu->deviceName.c_str(),gpu->GetMemory() / 1048576.0);

  double t0 = Timer::get_tick();


  if( ph->px==NULL ) {
    if(keyIdx == 0)
      ::printf("SolveKeyGPU Thread GPU#%d: creating kangaroos...\n",ph->gpuId);
    // Create Kangaroos, if not already loaded
    uint64_t nbThread = gpu->GetNbThread();
    ph->px = new Int[ph->nbKangaroo];
    ph->py = new Int[ph->nbKangaroo];
    ph->distance = new Int[ph->nbKangaroo];

    for(uint64_t i = 0; i<nbThread; i++) {
      CreateHerd(GPU_GRP_SIZE,&(ph->px[i*GPU_GRP_SIZE]),
                              &(ph->py[i*GPU_GRP_SIZE]),
                              &(ph->distance[i*GPU_GRP_SIZE]),
                              TAME);
    }
  }

#ifdef USE_SYMMETRY
  gpu->SetWildOffset(&rangeWidthDiv4);
#else
  gpu->SetWildOffset(&rangeWidthDiv2);
#endif
  gpu->SetParams(dMask,jumpDistance,jumpPointx,jumpPointy);
  gpu->SetKangaroos(ph->px,ph->py,ph->distance);

  if(workFile.length()==0 || !saveKangaroo) {
    // No need to get back kangaroo, free memory
    safe_delete_array(ph->px);
    safe_delete_array(ph->py);
    safe_delete_array(ph->distance);
  }

  gpu->callKernel();

  double t1 = Timer::get_tick();

  if(keyIdx == 0)
    ::printf("SolveKeyGPU Thread GPU#%d: 2^%.2f kangaroos [%.1fs]\n",ph->gpuId,log2((double)ph->nbKangaroo),(t1-t0));

  ph->hasStarted = true;

  while(!endOfSearch) {

    gpu->Launch(gpuFound);
    counters[thId] += ph->nbKangaroo * NB_RUN;

    if( clientMode ) {

      for(int i=0;i<(int)gpuFound.size();i++)
        dps.push_back(gpuFound[i]);

      double now = Timer::get_tick();
      if(now - lastSent > SEND_PERIOD) {
        LOCK(ghMutex);
        SendToServer(dps,ph->threadId,ph->gpuId);
        UNLOCK(ghMutex);
        lastSent = now;
      }

    } else {

      if(gpuFound.size() > 0) {

        LOCK(ghMutex);

        for(int g = 0; !endOfSearch && g < gpuFound.size(); g++) {

          uint32_t kType = (uint32_t)(gpuFound[g].kIdx % 2);

          if(!AddToTable(&gpuFound[g].x,&gpuFound[g].d,kType)) {
            // Collision inside the same herd
            // We need to reset the kangaroo
            Int px;
            Int py;
            Int d;
            CreateHerd(1,&px,&py,&d,kType,false);
            gpu->SetKangaroo(gpuFound[g].kIdx,&px,&py,&d);
            collisionInSameHerd++;
          }

        }
        UNLOCK(ghMutex);
      }

    }

    // Save request
    if(saveRequest && !endOfSearch) {
      // Get kangaroos
      if(saveKangaroo)
        gpu->GetKangaroos(ph->px,ph->py,ph->distance);
      ph->isWaiting = true;
      LOCK(saveMutex);
      ph->isWaiting = false;
      UNLOCK(saveMutex);
    }

  }


  safe_delete_array(ph->px);
  safe_delete_array(ph->py);
  safe_delete_array(ph->distance);
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

void Kangaroo::CreateHerd(int nbKangaroo,Int *px,Int *py,Int *d,int firstType,bool lock) {

  vector<Int> pk;
  vector<Point> S;
  vector<Point> Sp;
  pk.reserve(nbKangaroo);
  S.reserve(nbKangaroo);
  Sp.reserve(nbKangaroo);
  Point Z;
  Z.Clear();

  // Choose random starting distance
  if(lock) LOCK(ghMutex);

  for(uint64_t j = 0; j<nbKangaroo; j++) {

#ifdef USE_SYMMETRY

    // Tame in [0..N/2]
    d[j].Rand(rangePower - 1);
    if((j+ firstType) % 2 == WILD) {
      // Wild in [-N/4..N/4]
      d[j].ModSubK1order(&rangeWidthDiv4);
    }

#else

    // Tame in [0..N]
    d[j].Rand(rangePower);
    if((j + firstType) % 2 == WILD) {
      // Wild in [-N/2..N/2]
      d[j].ModSubK1order(&rangeWidthDiv2);
    }

#endif

    pk.push_back(d[j]);

  }

  if(lock) UNLOCK(ghMutex);

  // Compute starting pos
  S = secp->ComputePublicKeys(pk);

  for(uint64_t j = 0; j<nbKangaroo; j++) {
    if((j + firstType) % 2 == TAME) {
      Sp.push_back(Z);
    } else {
      Sp.push_back(keyToSearch);
    }
  }

  S = secp->AddDirect(Sp,S);

  for(uint64_t j = 0; j<nbKangaroo; j++) {

    px[j].Set(&S[j].x);
    py[j].Set(&S[j].y);

#ifdef USE_SYMMETRY
    // Equivalence symmetry class switch
    if( py[j].ModPositiveK1() )
      d[j].ModNegK1order();
#endif

  }

}

// ----------------------------------------------------------------------------

void Kangaroo::CreateJumpTable() {

#ifdef USE_SYMMETRY
  int jumpBit = rangePower / 2;
#else
  int jumpBit = rangePower / 2 + 1;
#endif

  if(jumpBit > 128) jumpBit = 128;
  int maxRetry = 100;
  bool ok = false;
  double distAvg;
  double maxAvg = pow(2.0,(double)jumpBit - 0.95);
  double minAvg = pow(2.0,(double)jumpBit - 1.05);
  //::printf("Jump Avg distance min: 2^%.2f\n",log2(minAvg));
  //::printf("Jump Avg distance max: 2^%.2f\n",log2(maxAvg));
  
  // Kangaroo jumps
  // Constant seed for compatibilty of workfiles
  rseed(0x600DCAFE);

#ifdef USE_SYMMETRY
  Int old;
  old.Set(Int::GetFieldCharacteristic());
  Int u;
  Int v;
  u.SetInt32(1);
  u.ShiftL(jumpBit/2);
  u.AddOne();
  while(!u.IsProbablePrime()) {
    u.AddOne();
    u.AddOne();
  }
  v.Set(&u);
  v.AddOne();
  v.AddOne();
  while(!v.IsProbablePrime()) {
    v.AddOne();
    v.AddOne();
  }
  Int::SetupField(&old);

  ::printf("U= %s\n",u.GetBase16().c_str());
  ::printf("V= %s\n",v.GetBase16().c_str());
#endif

  // Positive only
  // When using symmetry, the sign is switched by the symmetry class switch
  while(!ok && maxRetry>0 ) {
    Int totalDist;
    totalDist.SetInt32(0);
#ifdef USE_SYMMETRY
    for(int i = 0; i < NB_JUMP/2; ++i) {
      jumpDistance[i].Rand(jumpBit/2);
      jumpDistance[i].Mult(&u);
      if(jumpDistance[i].IsZero())
        jumpDistance[i].SetInt32(1);
      totalDist.Add(&jumpDistance[i]);
    }
    for(int i = NB_JUMP / 2; i < NB_JUMP; ++i) {
      jumpDistance[i].Rand(jumpBit/2);
      jumpDistance[i].Mult(&v);
      if(jumpDistance[i].IsZero())
        jumpDistance[i].SetInt32(1);
      totalDist.Add(&jumpDistance[i]);
    }
#else
    for(int i = 0; i < NB_JUMP; ++i) {
      jumpDistance[i].Rand(jumpBit);
      if(jumpDistance[i].IsZero())
        jumpDistance[i].SetInt32(1);
      totalDist.Add(&jumpDistance[i]);
  }
#endif
    distAvg = totalDist.ToDouble() / (double)(NB_JUMP);
    ok = distAvg>minAvg && distAvg<maxAvg;
    maxRetry--;
  }

  for(int i = 0; i < NB_JUMP; ++i) {
    Point J = secp->ComputePublicKey(&jumpDistance[i]);
    jumpPointx[i].Set(&J.x);
    jumpPointy[i].Set(&J.y);
  }

  ::printf("Jump Avg distance: 2^%.2f\n",log2(distAvg));

  unsigned long seed = Timer::getSeed32();
  rseed(seed);

}

// ----------------------------------------------------------------------------

void Kangaroo::ComputeExpected(double dp,double *op,double *ram,double *overHead) {

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

  // Z0
  double Z0 = (2.0 * (2.0 - sqrt(2.0)) * gainS) * sqrt(M_PI);

  // Average for DP = 0
  double avgDP0 = Z0 * sqrt(N);

  // DP Overhead
  *op = Z0 * pow(N * (k * theta + sqrt(N)),1.0 / 3.0);

  *ram = (double)sizeof(HASH_ENTRY) * (double)HASH_SIZE + // Table
         (double)sizeof(ENTRY *) * (double)(HASH_SIZE * 4) + // Allocation overhead
         (double)(sizeof(ENTRY) + sizeof(ENTRY *)) * (*op / theta); // Entries

  *ram /= (1024.0*1024.0);

  if(overHead)
    *overHead = *op/avgDP0;

}

// ----------------------------------------------------------------------------

void Kangaroo::InitRange() {

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

}

void Kangaroo::InitSearchKey() {

  Int SP;
  SP.Set(&rangeStart);
#ifdef USE_SYMMETRY
  SP.ModAddK1order(&rangeWidthDiv2);
#endif
  if(!SP.IsZero()) {
    Point RS = secp->ComputePublicKey(&SP);
    RS.y.ModNeg();
    keyToSearch = secp->AddDirect(keysToSearch[keyIdx],RS);
  } else {
    keyToSearch = keysToSearch[keyIdx];
  }
  keyToSearchNeg = keyToSearch;
  keyToSearchNeg.y.ModNeg();

}

// ----------------------------------------------------------------------------

void Kangaroo::Run(int nbThread,std::vector<int> gpuId,std::vector<int> gridSize) {

  double t0 = Timer::get_tick();

  nbCPUThread = nbThread;
  nbGPUThread = (useGpu ? (int)gpuId.size() : 0);
  totalRW = 0;

#ifndef WITHGPU

  if(nbGPUThread>0) {
    ::printf("GPU code not compiled, use -DWITHGPU when compiling.\n");
    nbGPUThread = 0;
  }

#endif

  uint64_t totalThread = (uint64_t)nbCPUThread + (uint64_t)nbGPUThread;
  if(totalThread == 0) {
    ::printf("No CPU or GPU thread, exiting.\n");
    ::exit(0);
  }

  TH_PARAM *params = (TH_PARAM *)malloc(totalThread * sizeof(TH_PARAM));
  THREAD_HANDLE *thHandles = (THREAD_HANDLE *)malloc(totalThread * sizeof(THREAD_HANDLE));

  memset(params, 0,totalThread * sizeof(TH_PARAM));
  memset(counters, 0, sizeof(counters));
  ::printf("Number of CPU thread: %d\n", nbCPUThread);

#ifdef WITHGPU

  // Compute grid size
  for(int i = 0; i < nbGPUThread; i++) {
    int x = gridSize[2ULL * i];
    int y = gridSize[2ULL * i + 1ULL];
    if(!GPUEngine::GetGridSize(gpuId[i],&x,&y)) {
      return;
    } else {
      params[nbCPUThread + i].gridSizeX = x;
      params[nbCPUThread + i].gridSizeY = y;
    }
    params[nbCPUThread + i].nbKangaroo = (uint64_t)GPU_GRP_SIZE * x * y;
    totalRW += params[nbCPUThread + i].nbKangaroo;
  }

#endif

  totalRW += nbCPUThread * (uint64_t)CPU_GRP_SIZE;

  // Set starting parameters
  if( clientMode ) {
    // Retrieve config from server
    if( !GetConfigFromServer() )
      ::exit(0);
    // Client save only kangaroos, force -ws
    if(workFile.length()>0)
      saveKangaroo = true;
  }

  InitRange();
  CreateJumpTable();

  ::printf("Number of kangaroos: 2^%.2f\n",log2((double)totalRW));

  if( !clientMode ) {

    // Compute suggested distinguished bits number for less than 5% overhead (see README)
    double dpOverHead;
    int suggestedDP = (int)((double)rangePower / 2.0 - log2((double)totalRW));
    if(suggestedDP<0) suggestedDP=0;
    ComputeExpected((double)suggestedDP,&expectedNbOp,&expectedMem,&dpOverHead);
    while(dpOverHead>1.05 && suggestedDP>0) {
      suggestedDP--;
      ComputeExpected((double)suggestedDP,&expectedNbOp,&expectedMem,&dpOverHead);
    }

    if(initDPSize < 0)
      initDPSize = suggestedDP;

    ComputeExpected((double)initDPSize,&expectedNbOp,&expectedMem);
    if(nbLoadedWalk == 0) ::printf("Suggested DP: %d\n",suggestedDP);
    ::printf("Expected operations: 2^%.2f\n",log2(expectedNbOp));
    ::printf("Expected RAM: %.1fMB\n",expectedMem);

  } else {

    keyIdx = 0;
    InitSearchKey();

  }

  SetDP(initDPSize);

  // Fetch kangaroos (if any)
  FectchKangaroos(params);

//#define STATS
#ifdef STATS

    CPU_GRP_SIZE = 1024;
    for(; CPU_GRP_SIZE <= 1024; CPU_GRP_SIZE *= 4) {

      uint64_t totalCount = 0;
      uint64_t totalDead = 0;

#endif

    for(keyIdx = 0; keyIdx < keysToSearch.size(); keyIdx++) {

      InitSearchKey();

      endOfSearch = false;
      collisionInSameHerd = 0;

      // Reset conters
      memset(counters,0,sizeof(counters));

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
      double SN = pow(2.0,rangePower / 2.0);
      double avg = (double)totalCount / (double)(keyIdx + 1);
      ::printf("\n[%3d] 2^%.3f Dead:%d Avg:2^%.3f DeadAvg:%.1f (%.3f %.3f sqrt(N))\n",
                              keyIdx, log2((double)count), (int)collisionInSameHerd, 
                              log2(avg), (double)totalDead / (double)(keyIdx + 1),
                              avg/SN,expectedNbOp/SN);
    }
    string fName = "DP" + ::to_string(dpSize) + ".txt";
    FILE *f = fopen(fName.c_str(),"a");
    fprintf(f,"%d %f\n",CPU_GRP_SIZE*nbCPUThread,(double)totalCount);
    fclose(f);

#endif

  }

  double t1 = Timer::get_tick();

  ::printf("\nDone: Total time %s \n" , GetTimeStr(t1-t0+offsetTime).c_str());

}


