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

#define HEAD 0xFA6A8001

// ----------------------------------------------------------------------------

FILE *Kangaroo::ReadHeader(std::string fileName,uint32_t *version) {

  FILE *f = fopen(fileName.c_str(),"rb");
  if(f == NULL) {
    ::printf("ReadHeader: Cannot open %s for reading\n",fileName.c_str());
    ::printf("%s\n",::strerror(errno));
    return NULL;
  }

  uint32_t head;
  uint32_t versionF;

  // Read header
  if(::fread(&head,sizeof(uint32_t),1,f) != 1) {
    ::printf("ReadHeader: Cannot read from %s\n",fileName.c_str());
    if(::feof(fRead)) {
      ::printf("Empty file\n");
    } else {
      ::printf("%s\n",::strerror(errno));
    }
    ::fclose(f);
    return NULL;
  }

  if(head!=HEAD) {
    ::printf("ReadHeader: %s Not a work file\n",fileName.c_str());
    ::fclose(f);
    return NULL;
  }

  ::fread(&versionF,sizeof(uint32_t),1,f);
  if(version) *version = versionF;

  return f;

}

bool Kangaroo::LoadWork(string &fileName) {

  double t0 = Timer::get_tick();

  ::printf("Loading: %s\n",fileName.c_str());

  fRead = ReadHeader(fileName);
  if(fRead == NULL)
    return false;

  keysToSearch.clear();
  Point key;

  // Read global param
  uint32_t dp;
  ::fread(&dp,sizeof(uint32_t),1,fRead);
  if(initDPSize<0) initDPSize = dp;
  ::fread(&rangeStart.bits64,32,1,fRead); rangeStart.bits64[4] = 0;
  ::fread(&rangeEnd.bits64,32,1,fRead); rangeEnd.bits64[4] = 0;
  ::fread(&key.x.bits64,32,1,fRead); key.x.bits64[4] = 0;
  ::fread(&key.y.bits64,32,1,fRead); key.y.bits64[4] = 0;
  ::fread(&offsetCount,sizeof(uint64_t),1,fRead);
  ::fread(&offsetTime,sizeof(double),1,fRead);
  
  key.z.SetInt32(1);
  if(!secp->EC(key)) {
    ::printf("LoadWork: key does not lie on elliptic curve\n");
    return false;
  }
  
  keysToSearch.push_back(key);

  ::printf("Start:%s\n",rangeStart.GetBase16().c_str());
  ::printf("Stop :%s\n",rangeEnd.GetBase16().c_str());
  ::printf("Keys :%d\n",(int)keysToSearch.size());

  // Read hashTable
  hashTable.LoadTable(fRead);

  // Read number of walk
  fread(&nbLoadedWalk,sizeof(uint64_t),1,fRead);

  double t1 = Timer::get_tick();


  ::printf("LoadWork: [HashTalbe %s] [%s]\n",hashTable.GetSizeInfo().c_str(),GetTimeStr(t1 - t0).c_str());

  return true;
}

// ----------------------------------------------------------------------------

void Kangaroo::FetchWalks(uint64_t nbWalk,Int *x,Int *y,Int *d) {

  // Read Kangaroos
  int64_t n = 0;

  ::printf("Fetch kangaroos: %.0f\n",(double)nbWalk);

  for(n = 0; n < (int64_t)nbWalk && nbLoadedWalk>0; n++) {
    ::fread(&x[n].bits64,32,1,fRead); x[n].bits64[4] = 0;
    ::fread(&y[n].bits64,32,1,fRead); y[n].bits64[4] = 0;
    ::fread(&d[n].bits64,32,1,fRead); d[n].bits64[4] = 0;
    nbLoadedWalk--;
  }

  if(n<(int64_t)nbWalk) {
    int64_t empty = nbWalk - n;
    // Fill empty kanagaroo
    CreateHerd((int)empty,&(x[n]),&(y[n]),&(d[n]),TAME);
  }

}

void Kangaroo::FectchKangaroos(TH_PARAM *threads) {

  // Fetch input kangarou (if any)
  if(nbLoadedWalk>0) {

    double sFetch = Timer::get_tick();
    uint64_t nbSaved = nbLoadedWalk;
    uint64_t created = 0;

    // Fetch loaded walk
    for(int i = 0; i < nbCPUThread; i++) {
      threads[i].px = new Int[CPU_GRP_SIZE];
      threads[i].py = new Int[CPU_GRP_SIZE];
      threads[i].distance = new Int[CPU_GRP_SIZE];
      FetchWalks(CPU_GRP_SIZE,threads[i].px,threads[i].py,threads[i].distance);
    }

#ifdef WITHGPU
    for(int i = 0; i < nbGPUThread; i++) {
      int id = nbCPUThread + i;
      uint64_t n = threads[id].nbKangaroo;
      threads[id].px = new Int[n];
      threads[id].py = new Int[n];
      threads[id].distance = new Int[n];
      FetchWalks(n,threads[id].px,threads[id].py,threads[id].distance);
    }
#endif

    double eFetch = Timer::get_tick();

    if(nbLoadedWalk != 0) {
      ::printf("LoadWork: Warning %.0f unhandled kangaroos !\n",(double)nbLoadedWalk);
    }

    if(nbSaved<totalRW)
      created = totalRW - nbSaved;

    ::printf("LoadWork: [2^%.2f kangaroos loaded] [%.0f created] [%s]\n",log2((double)nbSaved),(double)created,GetTimeStr(eFetch - sFetch).c_str());

  }

  // Close input file
  if(fRead) fclose(fRead);

}


// ----------------------------------------------------------------------------

void Kangaroo::SaveWork(uint64_t totalCount,double totalTime,TH_PARAM *threads,int nbThread) {

  LOCK(saveMutex);

  double t0 = Timer::get_tick();

  // Wait that all threads blocks before saving works
  saveRequest = true;
  int timeout = 1000;
  while(!isWaiting(threads) && timeout>0) {
    Timer::SleepMillis(50);
    timeout -= 50;
  }

  if(timeout<=0) {
    // Thread blocked or ended !
    if(!endOfSearch)
      ::printf("\nSaveWork timweout !\n");
    UNLOCK(saveMutex);
    return;
  }

  ::printf("\nSaveWork: %s",workFile.c_str());

  FILE *f = fopen(workFile.c_str(),"wb");
  if(f == NULL) {
    ::printf("SaveWork: Cannot open %s for writing\n",workFile.c_str());
    ::printf("%s\n",::strerror(errno));
    return;
  }

  // Header
  uint32_t head = HEAD;
  uint32_t version = 0;
  if(::fwrite(&head,sizeof(uint32_t),1,f) != 1 ) {
    ::printf("SaveWork: Cannot write to %s\n",workFile.c_str());
    ::printf("%s\n",::strerror(errno));
    return;
  }
  ::fwrite(&version,sizeof(uint32_t),1,f);

  // Save global param
  ::fwrite(&dpSize,sizeof(uint32_t),1,f);
  ::fwrite(&rangeStart.bits64,32,1,f);
  ::fwrite(&rangeEnd.bits64,32,1,f);
  ::fwrite(&keysToSearch[keyIdx].x.bits64,32,1,f);
  ::fwrite(&keysToSearch[keyIdx].y.bits64,32,1,f);
  ::fwrite(&totalCount,sizeof(uint64_t),1,f);
  ::fwrite(&totalTime,sizeof(double),1,f);

  // Save hash table
  hashTable.SaveTable(f);

  uint64_t totalWalk = 0;

  if(saveKangaroo) {

    // Save kangaroos
    for(int i = 0; i < nbThread; i++)
      totalWalk += threads[i].nbKangaroo;
    ::fwrite(&totalWalk,sizeof(uint64_t),1,f);

    uint64_t point = totalWalk / 16;
    uint64_t pointPrint = 0;

    for(int i = 0; i < nbThread; i++) {
      for(uint64_t n = 0; n < threads[i].nbKangaroo; n++) {
        ::fwrite(&threads[i].px[n].bits64,32,1,f);
        ::fwrite(&threads[i].py[n].bits64,32,1,f);
        ::fwrite(&threads[i].distance[n].bits64,32,1,f);
        pointPrint++;
        if(pointPrint>point) {
          ::printf(".");
          pointPrint = 0;
        }
      }
    }

  } else {

    ::fwrite(&totalWalk,sizeof(uint64_t),1,f);

  }

  uint64_t size = ftell(f);
  fclose(f);

  // Unblock threads
  saveRequest = false;
  UNLOCK(saveMutex);

  double t1 = Timer::get_tick();

  char *ctimeBuff;
  time_t now = time(NULL);
  ctimeBuff = ctime(&now);
  ::printf("done [%.1f MB] [%s] %s",(double)size/(1024.0*1024.0),GetTimeStr(t1 - t0).c_str(),ctimeBuff);

}


void Kangaroo::MergeWork(std::string &file1,std::string &file2,std::string &dest) {

  double t0;
  double t1;

  // ---------------------------------------------------

  ::printf("Loading: %s\n",file1.c_str());

  t0 = Timer::get_tick();

  FILE *f1 = ReadHeader(file1);
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
    ::printf("MergeWork: key1 does not lie on elliptic curve\n");
    fclose(f1);
    return;
  }

  t1 = Timer::get_tick();

  // Read hashTable
  hashTable.LoadTable(f1);
  ::printf("MergeWork: [HashTalbe1 %s] [%s]\n",hashTable.GetSizeInfo().c_str(),GetTimeStr(t1 - t0).c_str());

  fclose(f1);

  // ---------------------------------------------------

  ::printf("Loading: %s\n",file2.c_str());

  t0 = Timer::get_tick();

  FILE *f2 = ReadHeader(file2);
  if(f2 == NULL)
    return;

  uint32_t dp2;
  Point k2;
  uint64_t count2;
  double time2;
  Int RS2;
  Int RE2;

  // Read global param
  ::fread(&dp2,sizeof(uint32_t),1,f2);
  ::fread(&RS2.bits64,32,1,f2); RS2.bits64[4] = 0;
  ::fread(&RE2.bits64,32,1,f2); RE2.bits64[4] = 0;
  ::fread(&k2.x.bits64,32,1,f2); k2.x.bits64[4] = 0;
  ::fread(&k2.y.bits64,32,1,f2); k2.y.bits64[4] = 0;
  ::fread(&count2,sizeof(uint64_t),1,f2);
  ::fread(&time2,sizeof(double),1,f2);

  k2.z.SetInt32(1);
  if(!secp->EC(k2)) {
    ::printf("MergeWork: key2 does not lie on elliptic curve\n");
    fclose(f2);
    return;
  }

  if(!RS1.IsEqual(&RS2) || !RE1.IsEqual(&RE2)) {

    ::printf("MergeWork: File range differs\n");
    ::printf("RS1: %s\n",RS1.GetBase16().c_str());
    ::printf("RE1: %s\n",RE1.GetBase16().c_str());
    ::printf("RS2: %s\n",RS2.GetBase16().c_str());
    ::printf("RE2: %s\n",RE2.GetBase16().c_str());
    fclose(f2);
    return;

  }

  if( !k1.equals(k2) ) {

    ::printf("MergeWork: key differs, multiple keys not yet supported\n");
    fclose(f2);
    return;

  }

  t1 = Timer::get_tick();

  // Read hashTable
  HashTable *h2 = new HashTable();
  h2->LoadTable(f2);
  ::printf("MergeWork: [HashTalbe2 %s] [%s]\n",hashTable.GetSizeInfo().c_str(),GetTimeStr(t1 - t0).c_str());

  fclose(f2);

  ::printf("Merging...\n");

  endOfSearch = false;

  // Set starting parameters
  keysToSearch.clear();
  keysToSearch.push_back(k1);
  keyIdx = 0;
  rangeStart.Set(&RS1);
  rangeEnd.Set(&RE1);
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
  collisionInSameHerd = 0;

  for(uint64_t h = 0; h < HASH_SIZE && !endOfSearch; h++) {
    for(uint32_t i = 0; i<h2->E[h].nbItem && !endOfSearch; i++) {

      // Reconstruct point
      Int x;
      x.SetInt32(0);
      x.bits64[0] = h2->E[h].items[i]->x.i64[0];
      x.bits64[1] = h2->E[h].items[i]->x.i64[1];
      x.bits64[2] = h;

      Int d;
      d.SetInt32(0);
      int128_t dd = h2->E[h].items[i]->d;
      d.bits64[0] = dd.i64[0];
      d.bits64[1] = dd.i64[1] & 0x3FFFFFFFFFFFFFFFULL;
      int sign = (dd.i64[1] & 0x8000000000000000ULL) != 0;
      if(sign) d.ModNegK1order();
      uint32_t kType = (dd.i64[1] & 0x4000000000000000ULL) != 0;

      // Add
      if( !AddToTable(&x,&d,kType) )
        collisionInSameHerd++;

    }
  }

  if(!endOfSearch) {
  
    // Write the new work file
    dpSize = (dp1<dp2)?dp1:dp2;
    workFile = dest;
    SaveWork(count1+count2,time1+time2,NULL,0);

  }

  ::printf("Dead kangaroo: %d\n",collisionInSameHerd);  
  ::printf("Total f1+f2: count 2^%.2f [%s]\n",log2((double)count1 + (double)count2),GetTimeStr(time1 + time2).c_str());

}

