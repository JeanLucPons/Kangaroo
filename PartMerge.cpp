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
#include <sys/stat.h>
#define _strdup strdup
#endif

using namespace std;

string Kangaroo::GetPartName(std::string& partName,int i,bool tmpPart) {

  char tmp[256];
  if(tmpPart)
    sprintf(tmp,"part%03d.tmp",i);
  else
    sprintf(tmp,"part%03d",i);
  string pName = partName + "/" + string(tmp);

  return pName;

}

FILE * Kangaroo::OpenPart(std::string& partName,char *mode,int i,bool tmpPart) {

  string fName = GetPartName(partName,i,tmpPart);
  FILE* f = fopen(fName.c_str(),mode);
  if(f == NULL) {
    ::printf("OpenPart: Cannot open %s for mode %s\n",fName.c_str(),mode);
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
  if( stat(partName.c_str(),&buffer) == 0 ) {
    ::printf("CreateEmptyPartWork: %s exists\n",partName.c_str());
    return;
  }

  if(mkdir(partName.c_str(),S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0) {
    ::printf("mkdir(%s) Error:\n",partName.c_str());
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

bool Kangaroo::MergePartition(TH_PARAM* p) {

  uint32_t part = p->hStart;
  string p1Name = string(p->part1Name);
  string p2Name = string(p->part2Name);

  FILE *f1 = OpenPart(p1Name,"rb",part,false);
  if(f1 == NULL) return false;
  FILE* f2 = OpenPart(p2Name,"rb",part,false);
  if(f2 == NULL) return false;
  FILE* f = OpenPart(p1Name,"wb",part,true);
  if(f == NULL) return false;

  uint32_t hStart = part * (HASH_SIZE / MERGE_PART);
  uint32_t hStop = (part +1) * (HASH_SIZE / MERGE_PART);

  uint32_t hDP;
  uint32_t hDuplicate;
  Int d1;
  uint32_t type1;
  Int d2;
  uint32_t type2;

  for(uint32_t h = hStart; h < hStop && !endOfSearch; h++) {

    int mStatus = HashTable::MergeH(h,f1,f2,f,&hDP,&hDuplicate,&d1,&type1,&d2,&type2);
    switch(mStatus) {
    case ADD_OK:
      break;
    case ADD_COLLISION:
      CollisionCheck(&d1,type1,&d2,type2);
      break;
    }

    // Counting overflow after (2^32)*MARGE_PART DP
    p->hStop += hDP;
    collisionInSameHerd += hDuplicate;

  }

  ::fclose(f1);
  ::fclose(f2);
  ::fclose(f);

  // Rename
  string oldName = GetPartName(p1Name,part,true);
  string newName = GetPartName(p1Name,part,false);
  remove(newName.c_str());
  rename(oldName.c_str(),newName.c_str());

  return true;

}
// Threaded proc
#ifdef WIN64
extern DWORD WINAPI _mergeThread(LPVOID lpParam);
#else
extern void* _mergeThread(void* lpParam);
#endif

// Threaded proc
#ifdef WIN64
DWORD WINAPI _mergePartThread(LPVOID lpParam) {
#else
void* _mergePartThread(void* lpParam) {
#endif
  TH_PARAM* p = (TH_PARAM*)lpParam;
  p->obj->MergePartition(p);
  p->isRunning = false;
  return 0;
}

bool Kangaroo::MergeWorkPartPart(std::string& part1Name,std::string& part2Name) {

  double t0;
  double t1;
  uint32_t v1;
  uint32_t v2;

  t0 = Timer::get_tick();

  // ---------------------------------------------------
  string file1 = part1Name + "/header";
  bool partIsEmpty = IsEmpty(file1);
  string file2 = part2Name + "/header";
  if( IsEmpty(file2) ) {
    ::printf("MergeWorkPartPart: partition #2 is empty\n");
    return true;
  }

  uint32_t dp1;
  Point k1;
  uint64_t count1;
  double time1;
  Int RS1;
  Int RE1;

  if(!partIsEmpty) {

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
      ::printf("MergeWorkPartPart: key1 does not lie on elliptic curve\n");
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
    ::printf("MergeWorkPartPart: key2 does not lie on elliptic curve\n");
    ::fclose(f2);
    return true;
  }

  if(!partIsEmpty) {

    if(v1 != v2) {
      ::printf("MergeWorkPartPart: cannot merge workfile of different version\n");
      ::fclose(f2);
      return true;
    }

    if(!RS1.IsEqual(&RS2) || !RE1.IsEqual(&RE2)) {

      ::printf("MergeWorkPartPart: File range differs\n");
      ::printf("RS1: %s\n",RS1.GetBase16().c_str());
      ::printf("RE1: %s\n",RE1.GetBase16().c_str());
      ::printf("RS2: %s\n",RS2.GetBase16().c_str());
      ::printf("RE2: %s\n",RE2.GetBase16().c_str());
      ::fclose(f2);
      return true;

    }

    if(!k1.equals(k2)) {

      ::printf("MergeWorkPartPart: key differs, multiple keys not yet supported\n");
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
  fclose(f2);

  ::printf("%s: [DP%d]\n",part1Name.c_str(),dp1);
  ::printf("%s: [DP%d]\n",part2Name.c_str(),dp2);

  // Set starting parameters
  endOfSearch = false;
  keysToSearch.clear();
  keysToSearch.push_back(k1);
  keyIdx = 0;
  collisionInSameHerd = 0;
  rangeStart.Set(&RS1);
  rangeEnd.Set(&RE1);
  InitRange();
  InitSearchKey();

  // Write new header
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


  int nbCore = Timer::getCoreNumber();
  int l2 = (int)log2(nbCore);
  int nbThread = (int)pow(2.0,l2);
  if(nbThread > 16) nbThread = 16;

#ifndef WIN64
  setvbuf(stdout,NULL,_IONBF,0);
#endif

  ::printf("Thread: %d\n",nbThread);
  ::printf("Merging");

  TH_PARAM* params = (TH_PARAM*)malloc(nbThread * sizeof(TH_PARAM));
  THREAD_HANDLE* thHandles = (THREAD_HANDLE*)malloc(nbThread * sizeof(THREAD_HANDLE));
  memset(params,0,nbThread * sizeof(TH_PARAM));
  uint64_t nbDP = 0;

  for(int p = 0; p < MERGE_PART && !endOfSearch; p+=nbThread) {

    printf(".");

    for(int i = 0; i < nbThread; i++) {
      params[i].threadId = i;
      params[i].isRunning = true;
      params[i].hStart = p+i;
      params[i].hStop = 0;
      params[i].part1Name = _strdup(part1Name.c_str());
      params[i].part2Name = _strdup(part2Name.c_str());
      thHandles[i] = LaunchThread(_mergePartThread,params + i);
    }

    JoinThreads(thHandles,nbThread);
    FreeHandles(thHandles,nbThread);

    for(int i = 0; i < nbThread; i++) {
      free(params[i].part1Name);
      free(params[i].part2Name);
      nbDP += params[i].hStop;
    }

  }

  free(params);
  free(thHandles);

  t1 = Timer::get_tick();

  if(!endOfSearch) {

    ::printf("Done [2^%.3f DP][%s]\n",log2((double)nbDP),GetTimeStr(t1 - t0).c_str());

  } else {

#ifdef WIN64
    ::printf("Dead kangaroo: %I64d\n",collisionInSameHerd);
#else
    ::printf("Dead kangaroo: %" PRId64 "\n",collisionInSameHerd);
#endif
    ::printf("Total f1+f2: DP count 2^%.2f\n",log2((double)nbDP));
    return true;

  }

#ifdef WIN64
  ::printf("Dead kangaroo: %I64d\n",collisionInSameHerd);
#else
  ::printf("Dead kangaroo: %" PRId64 "\n",collisionInSameHerd);
#endif
  ::printf("Total f1+f2: DP count 2^%.2f\n",log2((double)nbDP));

  return false;

}

bool Kangaroo::FillEmptyPartFromFile(std::string& partName,std::string& fileName,bool printStat) {

  double t0;
  double t1;
  uint32_t v1;

  uint32_t dp1;
  Point k1;
  uint64_t count1;
  double time1;
  Int RS1;
  Int RE1;

  t0 = Timer::get_tick();

  FILE* f1 = ReadHeader(fileName,&v1,HEADW);
  if(f1 == NULL)
    return true;

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
    ::printf("FillEmptyPartFromFile: key1 does not lie on elliptic curve\n");
    ::fclose(f1);
    return true;
  }

  // Save header
  dpSize = dp1;
  keysToSearch.clear();
  keysToSearch.push_back(k1);
  keyIdx = 0;
  collisionInSameHerd = 0;
  rangeStart.Set(&RS1);
  rangeEnd.Set(&RE1);
  InitRange();
  InitSearchKey();

  string file1 = partName + "/header";
  FILE* f = fopen(file1.c_str(),"wb");
  if(f == NULL) {
    ::printf("FillEmptyPartFromFile: Cannot open %s for writing\n",file1.c_str());
    ::printf("%s\n",::strerror(errno));
    return true;
  }
  if(!SaveHeader(file1,f,HEADW,count1,time1)) {
    return true;
  }
  ::fclose(f);

  ::printf("Part %s: [DP%d]\n",partName.c_str(),dp1);
  ::printf("File %s: [DP%d]\n",fileName.c_str(),dp1);

  uint64_t nbDP = 0;
  ::printf("Filling");

  // Save parts
  for(int p = 0; p < MERGE_PART; p++) {

    if(p % (MERGE_PART / 64) == 0) ::printf(".");

    FILE *f = OpenPart(partName,"wb",p,false);
    uint32_t hStart = p * (HASH_SIZE / MERGE_PART);
    uint32_t hStop = (p + 1) * (HASH_SIZE / MERGE_PART);

    uint32_t nbItem;
    uint32_t maxItem;
    unsigned char buff[32];

    for(uint32_t h= hStart;h<hStop;h++) {
      ::fread(&nbItem,sizeof(uint32_t),1,f1);
      ::fread(&maxItem,sizeof(uint32_t),1,f1);
      ::fwrite(&nbItem,sizeof(uint32_t),1,f);
      ::fwrite(&maxItem,sizeof(uint32_t),1,f);
      for(uint32_t i=0;i<nbItem;i++) {
        ::fread(&buff,32,1,f1);
        ::fwrite(&buff,32,1,f);
      }
      nbDP += nbItem;
    }

    ::fclose(f);

  }

  ::fclose(f1);
  t1 = Timer::get_tick();

  ::printf("Done [2^%.3f DP][%s]\n",log2((double)nbDP),GetTimeStr(t1 - t0).c_str());

  return false;

}

bool Kangaroo::MergeWorkPart(std::string& partName,std::string& file2,bool printStat) {

  double t0;
  double t1;
  uint32_t v1;
  uint32_t v2;

#ifndef WIN64
  setvbuf(stdout,NULL,_IONBF,0);
#endif

  t0 = Timer::get_tick();

  // ---------------------------------------------------
  string file1 = partName + "/header";
  if( IsEmpty(file1) ) 
    return FillEmptyPartFromFile(partName,file2,printStat);

  uint32_t dp1;
  Point k1;
  uint64_t count1;
  double time1;
  Int RS1;
  Int RE1;

  FILE* f1 = ReadHeader(file1,&v1,HEADW);
  if(f1 == NULL)
    return true;

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

  ::printf("Part %s: [DP%d]\n",partName.c_str(),dp1);
  ::printf("File %s: [DP%d]\n",file2.c_str(),dp2);

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

  ::printf("Merging");

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

  uint64_t nbDP = 0;
  uint32_t hDP;
  uint32_t hDuplicate;
  Int d1;
  uint32_t type1;
  Int d2;
  uint32_t type2;

  for(int part = 0; part < MERGE_PART && !endOfSearch; part++) {

    if(part % (MERGE_PART / 64) == 0) ::printf(".");

    uint32_t hStart = part * (HASH_SIZE / MERGE_PART);
    uint32_t hStop = (part + 1) * (HASH_SIZE / MERGE_PART);

    // Load hashtables
    FILE *f1 = OpenPart(partName,"rb",part);
    FILE *f = OpenPart(partName,"wb",part,true);

    for(uint32_t h = hStart; h < hStop && !endOfSearch; h++) {

      int mStatus = HashTable::MergeH(h,f1,f2,f,&hDP,&hDuplicate,&d1,&type1,&d2,&type2);
      switch(mStatus) {
      case ADD_OK:
        break;
      case ADD_COLLISION:
        CollisionCheck(&d1,type1,&d2,type2);
        break;
      }

      nbDP += hDP;
      collisionInSameHerd += hDuplicate;

    }

    fclose(f1);
    fclose(f);

    // Rename
    string oldName = GetPartName(partName,part,true);
    string newName = GetPartName(partName,part,false);
    remove(newName.c_str());
    rename(oldName.c_str(),newName.c_str());

  }

  fclose(f2);

  t1 = Timer::get_tick();

  if(!endOfSearch) {

    ::printf("Done [2^%.3f DP][%s]\n",log2((double)nbDP),GetTimeStr(t1 - t0).c_str());

  } else {

#ifdef WIN64
    ::printf("Dead kangaroo: %I64d\n",collisionInSameHerd);
#else
    ::printf("Dead kangaroo: %" PRId64 "\n",collisionInSameHerd);
#endif
    ::printf("Total f1+f2: DP count 2^%.2f\n",log2((double)nbDP));
    return true;

  }

  if(printStat) {
#ifdef WIN64
    ::printf("Dead kangaroo: %I64d\n",collisionInSameHerd);
#else
    ::printf("Dead kangaroo: %" PRId64 "\n",collisionInSameHerd);
#endif
    ::printf("Total f1+f2: DP count 2^%.2f\n",log2((double)nbDP));
  } else {
    offsetTime = time1 + time2;
    offsetCount = count1 + count2;
  }

  return false;

}
