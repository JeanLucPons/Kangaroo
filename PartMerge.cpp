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
#include <dirent.h>
#include <pthread.h>
#endif

using namespace std;

FILE * Kangaroo::OpenPart(std::string& partName,char *mode,int i) {

  char tmp[256];
  sprintf(tmp,"part%03d",i);
  string pName = partName + "/" + string(tmp);
  FILE* f = fopen(pName.c_str(),mode);
  if(f == NULL) {
    ::printf("OpenPart: Cannot open %s for mode %s\n",pName.c_str(),mode);
    ::printf("%s\n",::strerror(errno));
  }

  return f;

}

void Kangaroo::CreateEmptyPartWork(std::string& partName) {

#ifdef WIN64

  WIN32_FIND_DATA ffd;
  HANDLE hFind;

  hFind = FindFirstFile(partName.c_str(),&ffd);
  if(hFind != INVALID_HANDLE_VALUE) {
    ::printf("CreateEmptyPartWork: %s exists\n",partName.c_str());
    return;
  }

  if(CreateDirectory(partName.c_str(),NULL) == 0) {
    ::printf("CreateDirectory Error: %d\n",GetLastError());
    return;
  }

#else

  struct stat buffer;
  return (stat(partName.c_str(),&buffer) == 0);

  if(mkdir(partName.c_str(),S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0) {
    ::printf("mkdir(%s) Error:\n",dirName.c_str());
    perror("");
    return;
  }

#endif

  // Header
  string hName = partName + "/header";
  FILE* f = fopen(hName.c_str(),"wb");
  if(f == NULL) {
    ::printf("CreateEmptyPartWork: Cannot open %s for writing\n",partName.c_str());
    ::printf("%s\n",::strerror(errno));
    return;
  }

  fclose(f);

  // Part
  for(int i = 0; i < MERGE_PART; i++) {

    FILE *f = OpenPart(partName,"wb",i);
    if(f==NULL)
      return;

    for(int j = 0; j < H_PER_PART; j++) {
      uint32_t z = 0;
      fwrite(&z,sizeof(uint32_t),1,f);
      fwrite(&z,sizeof(uint32_t),1,f);
    }

    fclose(f);

  }


  ::printf("CreateEmptyPartWork %s done\n",partName.c_str());

}

// Threaded proc
#ifdef WIN64
extern DWORD WINAPI _mergeThread(LPVOID lpParam);
#else
extern void* _mergeThread(void* lpParam);
#endif

bool Kangaroo::MergeWorkPart(std::string& partName,std::string& file2,bool printStat) {

  double t0;
  double t1;
  uint32_t v1;
  uint32_t v2;

  t0 = Timer::get_tick();

  // ---------------------------------------------------
  string file1 = partName + "/header";
  bool partIsEmpty = IsEmpty(file1);

  uint32_t dp1;
  Point k1;
  uint64_t count1;
  double time1;
  Int RS1;
  Int RE1;

  if( !partIsEmpty) {

    FILE* f1 = ReadHeader(file1,&v1,HEADW);
    if(f1 == NULL)
      return false;

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
      ::printf("MergeWorkPart: key1 does not lie on elliptic curve\n");
      ::fclose(f1);
      return true;
    }
    ::fclose(f1);

  }

  // ---------------------------------------------------

  FILE* f2 = ReadHeader(file2,&v2,HEADW);
  if(f2 == NULL) {
    return true;
  }

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
    ::printf("MergeWorkPart: key2 does not lie on elliptic curve\n");
    ::fclose(f2);
    return true;
  }

  if( !partIsEmpty ) {

    if(v1 != v2) {
      ::printf("MergeWorkPart: cannot merge workfile of different version\n");
      ::fclose(f2);
      return true;
    }

    if(!RS1.IsEqual(&RS2) || !RE1.IsEqual(&RE2)) {

      ::printf("MergeWorkPart: File range differs\n");
      ::printf("RS1: %s\n",RS1.GetBase16().c_str());
      ::printf("RE1: %s\n",RE1.GetBase16().c_str());
      ::printf("RS2: %s\n",RS2.GetBase16().c_str());
      ::printf("RE2: %s\n",RE2.GetBase16().c_str());
      ::fclose(f2);
      return true;

    }

    if(!k1.equals(k2)) {

      ::printf("MergeWorkPart: key differs, multiple keys not yet supported\n");
      ::fclose(f2);
      return true;

    }

  } else {

    dp1 = dp2;
    k1 = k2;
    count1 = 0;
    time1 = 0;
    RS1.Set(&RS2);
    RE1.Set(&RE2);

  }

  // Read hashTable
  HashTable* h2 = new HashTable();  
  for(int i=0;i<MERGE_PART;i++) {
    FILE *f = OpenPart(partName,"rb",i);
    hashTable.SeekNbItem(f,i*H_PER_PART,(i+1)* H_PER_PART);
    fclose(f);
  }
  h2->SeekNbItem(f2,true);
  uint64_t nb1 = hashTable.GetNbItem();
  uint64_t nb2 = h2->GetNbItem();
  uint64_t totalItem = nb1 + nb2;
  ::printf("%s: 2^%.2f DP [DP%d]\n",partName.c_str(),log2((double)nb1),dp1);
  ::printf("%s: 2^%.2f DP [DP%d]\n",file2.c_str(),log2((double)nb2),dp2);

  endOfSearch = false;

  // Set starting parameters
  keysToSearch.clear();
  keysToSearch.push_back(k1);
  keyIdx = 0;
  collisionInSameHerd = 0;
  rangeStart.Set(&RS1);
  rangeEnd.Set(&RE1);
  InitRange();
  InitSearchKey();

  t0 = Timer::get_tick();

  int nbCore = Timer::getCoreNumber();
  int l2 = (int)log2(nbCore);
  int nbThread = (int)pow(2.0,l2);
  if(nbThread>512) nbThread = 512;

#ifndef WIN64
  setvbuf(stdout,NULL,_IONBF,0);
#endif

  ::printf("Thread: %d\n",nbThread);
  ::printf("Merging");

  TH_PARAM* params = (TH_PARAM*)malloc(nbThread * sizeof(TH_PARAM));
  THREAD_HANDLE* thHandles = (THREAD_HANDLE*)malloc(nbThread * sizeof(THREAD_HANDLE));
  memset(params,0,nbThread * sizeof(TH_PARAM));

  // Save header
  FILE* f = fopen(file1.c_str(),"wb");
  if(f == NULL) {
    ::printf("MergeWorkPart: Cannot open %s for writing\n",file1.c_str());
    ::printf("%s\n",::strerror(errno));
    fclose(f2);
    return true;
  }
  dpSize = (dp1 < dp2) ? dp1 : dp2;
  if(!SaveHeader(file1,f,HEADW,count1 + count2,time1 + time2)) {
    fclose(f2);
    return true;
  }
  fclose(f);


  // Divide by MERGE_PART the amount of needed RAM
  int block = HASH_SIZE / MERGE_PART;
  uint64_t nbDP = 0;
  int pointPrint = 0;

  for(int s = 0; s < HASH_SIZE && !endOfSearch; s += block) {

    int part = s / block;
    if(part%4==0) ::printf(".");

    uint32_t S = s;
    uint32_t E = s + block;

    // Load hashtables
    FILE *f = OpenPart(partName,"rb",part);
    hashTable.LoadTable(f,S,E);
    h2->LoadTable(f2,S,E);

    int stride = block / nbThread;

    for(int i = 0; i < nbThread; i++) {
      params[i].threadId = i;
      params[i].isRunning = true;
      params[i].h2 = h2;
      params[i].hStart = S + i * stride;
      params[i].hStop = S + (i + 1) * stride;
      thHandles[i] = LaunchThread(_mergeThread,params + i);
    }
    JoinThreads(thHandles,nbThread);
    FreeHandles(thHandles,nbThread);

    fclose(f);

    f = OpenPart(partName,"wb",part);
    hashTable.SaveTable(f,S,E,false);
    fclose(f);
    nbDP += hashTable.GetNbItem();
    hashTable.Reset();

  }

  fclose(f2);
  free(params);
  free(thHandles);

  t1 = Timer::get_tick();

  if(!endOfSearch) {

    ::printf("Done [%s]\n",GetTimeStr(t1 - t0).c_str());

  } else {

    return true;

  }

  if(printStat) {
    ::printf("Dead kangaroo: %d\n",collisionInSameHerd);
    ::printf("Total f1+f2: DP count 2^%.2f\n",log2((double)nbDP));
  } else {
    offsetTime = time1 + time2;
    offsetCount = count1 + count2;
  }

  return false;

}
