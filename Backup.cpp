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
#include <sys/stat.h>
#endif

using namespace std;


// ----------------------------------------------------------------------------

int Kangaroo::FSeek(FILE* stream,uint64_t pos) {

#ifdef WIN64
  return _fseeki64(stream,pos,SEEK_SET);
#else
  return fseeko(stream,pos,SEEK_SET);
#endif

}

uint64_t Kangaroo::FTell(FILE* stream) {

#ifdef WIN64
  return (uint64_t)_ftelli64(stream);
#else
  return (uint64_t)ftello(stream);
#endif

}

bool Kangaroo::IsEmpty(std::string fileName) {

  FILE *pFile = fopen(fileName.c_str(),"r");
  if(pFile==NULL) {
    ::printf("OpenPart: Cannot open %s for reading\n",fileName.c_str());
    ::printf("%s\n",::strerror(errno));
    ::exit(0);
  }
  fseek(pFile,0,SEEK_END);
  uint32_t size = ftell(pFile);
  fclose(pFile);
  return size==0;

}

int Kangaroo::IsDir(string dirName) {

  bool isDir = 0;

#ifdef WIN64

  WIN32_FIND_DATA ffd;
  HANDLE hFind;

  hFind = FindFirstFile(dirName.c_str(),&ffd);
  if(hFind == INVALID_HANDLE_VALUE) {
    ::printf("%s not found\n",dirName.c_str());
    return -1;
  }
  isDir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  FindClose(hFind);

#else

  struct stat buffer;
  if(stat(dirName.c_str(),&buffer) != 0) {
    ::printf("%s not found\n",dirName.c_str());
    return -1;
  }
  isDir = (buffer.st_mode & S_IFDIR) != 0;

#endif

  return isDir;

}

FILE *Kangaroo::ReadHeader(std::string fileName, uint32_t *version, int type) {

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
    if(::feof(f)) {
      ::printf("Empty file\n");
    } else {
      ::printf("%s\n",::strerror(errno));
    }
    ::fclose(f);
    return NULL;
  }

  ::fread(&versionF,sizeof(uint32_t),1,f);
  if(version) *version = versionF;

  if(head!=type) {
    if(head==HEADK) {
      fread(&nbLoadedWalk,sizeof(uint64_t),1,f);
      ::printf("ReadHeader: %s is a kangaroo only file [2^%.2f kangaroos]\n",fileName.c_str(),log2((double)nbLoadedWalk));
    } if(head == HEADKS) {
      fread(&nbLoadedWalk,sizeof(uint64_t),1,f);
      ::printf("ReadHeader: %s is a compressed kangaroo only file [2^%.2f kangaroos]\n",fileName.c_str(),log2((double)nbLoadedWalk));
    } else if(head==HEADW) {
      ::printf("ReadHeader: %s is a work file, kangaroo only file expected\n",fileName.c_str());
    } else {
      ::printf("ReadHeader: %s Not a work file\n",fileName.c_str());
    }
    ::fclose(f);
    return NULL;
  }

  return f;

}

bool Kangaroo::LoadWork(string &fileName) {

  double t0 = Timer::get_tick();

  ::printf("Loading: %s\n",fileName.c_str());

  if(!clientMode) {

    fRead = ReadHeader(fileName,NULL,HEADW);
    if(fRead == NULL)
      return false;

    keysToSearch.clear();
    Point key;

    // Read global param
    uint32_t dp;
    ::fread(&dp,sizeof(uint32_t),1,fRead);
    if(initDPSize < 0) initDPSize = dp;
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

  } else {

    // In client mode, config come from the server, file has only kangaroo
    fRead = ReadHeader(fileName,NULL,HEADK);
    if(fRead == NULL)
      return false;

  }

  // Read number of walk
  fread(&nbLoadedWalk,sizeof(uint64_t),1,fRead);

  double t1 = Timer::get_tick();

  ::printf("LoadWork: [HashTable %s] [%s]\n",hashTable.GetSizeInfo().c_str(),GetTimeStr(t1 - t0).c_str());

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

void Kangaroo::FetchWalks(uint64_t nbWalk,std::vector<int128_t>& kangs,Int* x,Int* y,Int* d) {

  uint64_t n = 0;

  uint64_t avail = (nbWalk<kangs.size())?nbWalk:kangs.size();

  if(avail > 0) {

    vector<Int> dists;
    vector<Point> Sp;
    dists.reserve(avail);
    Sp.reserve(avail);
    Point Z;
    Z.Clear();

    for(n = 0; n < avail; n++) {

      Int dist;
      uint32_t type;
      HashTable::CalcDistAndType(kangs[n],&dist,&type);
      dists.push_back(dist);

    }

    vector<Point> P = secp->ComputePublicKeys(dists);

    for(n = 0; n < avail; n++) {

      if(n % 2 == TAME) {
        Sp.push_back(Z);
      }
      else {
        Sp.push_back(keyToSearch);
      }

    }

    vector<Point> S = secp->AddDirect(Sp,P);

    for(n = 0; n < avail; n++) {
      x[n].Set(&S[n].x);
      y[n].Set(&S[n].y);
      d[n].Set(&dists[n]);
      nbLoadedWalk--;
    }

    kangs.erase(kangs.begin(),kangs.begin() + avail);

  }

  if(avail < nbWalk) {
    int64_t empty = nbWalk - avail;
    // Fill empty kanagaroo
    CreateHerd((int)empty,&(x[n]),&(y[n]),&(d[n]),TAME);
  }

}

void Kangaroo::FectchKangaroos(TH_PARAM *threads) {

  double sFetch = Timer::get_tick();

  // From server
  vector<int128_t> kangs;
  if(saveKangarooByServer) {
    ::printf("FectchKangaroosFromServer");
    if(!GetKangaroosFromServer(workFile,kangs))
      ::exit(0);
    ::printf("Done\n");
    nbLoadedWalk = kangs.size();
  }


  // Fetch input kangaroo from file (if any)
  if(nbLoadedWalk>0) {

    ::printf("Restoring");

    uint64_t nbSaved = nbLoadedWalk;
    uint64_t created = 0;

    // Fetch loaded walk
    for(int i = 0; i < nbCPUThread; i++) {
      threads[i].px = new Int[CPU_GRP_SIZE];
      threads[i].py = new Int[CPU_GRP_SIZE];
      threads[i].distance = new Int[CPU_GRP_SIZE];
      if(!saveKangarooByServer)
        FetchWalks(CPU_GRP_SIZE,threads[i].px,threads[i].py,threads[i].distance);
      else
        FetchWalks(CPU_GRP_SIZE,kangs,threads[i].px,threads[i].py,threads[i].distance);
    }

#ifdef WITHGPU
    for(int i = 0; i < nbGPUThread; i++) {
      ::printf(".");
      int id = nbCPUThread + i;
      uint64_t n = threads[id].nbKangaroo;
      threads[id].px = new Int[n];
      threads[id].py = new Int[n];
      threads[id].distance = new Int[n];
      if(!saveKangarooByServer)
          FetchWalks(n,
            threads[id].px,
            threads[id].py,
            threads[id].distance);
      else
          FetchWalks(n,kangs,
            threads[id].px,
            threads[id].py,
            threads[id].distance);
    }
#endif

    ::printf("Done\n");

    double eFetch = Timer::get_tick();

    if(nbLoadedWalk != 0) {
      ::printf("FectchKangaroos: Warning %.0f unhandled kangaroos !\n",(double)nbLoadedWalk);
    }

    if(nbSaved<totalRW)
      created = totalRW - nbSaved;

    ::printf("FectchKangaroos: [2^%.2f kangaroos loaded] [%.0f created] [%s]\n",log2((double)nbSaved),(double)created,GetTimeStr(eFetch - sFetch).c_str());

  }

  // Close input file
  if(fRead) fclose(fRead);

}


// ----------------------------------------------------------------------------
bool Kangaroo::SaveHeader(string fileName,FILE* f,int type,uint64_t totalCount,double totalTime) {

  // Header
  uint32_t head = type;
  uint32_t version = 0;
  if(::fwrite(&head,sizeof(uint32_t),1,f) != 1) {
    ::printf("SaveHeader: Cannot write to %s\n",fileName.c_str());
    ::printf("%s\n",::strerror(errno));
    return false;
  }
  ::fwrite(&version,sizeof(uint32_t),1,f);

  if(type==HEADW) {

    // Save global param
    ::fwrite(&dpSize,sizeof(uint32_t),1,f);
    ::fwrite(&rangeStart.bits64,32,1,f);
    ::fwrite(&rangeEnd.bits64,32,1,f);
    ::fwrite(&keysToSearch[keyIdx].x.bits64,32,1,f);
    ::fwrite(&keysToSearch[keyIdx].y.bits64,32,1,f);
    ::fwrite(&totalCount,sizeof(uint64_t),1,f);
    ::fwrite(&totalTime,sizeof(double),1,f);

  }

  return true;
}

void  Kangaroo::SaveWork(string fileName,FILE *f,int type,uint64_t totalCount,double totalTime) {

  ::printf("\nSaveWork: %s",fileName.c_str());

  // Header
  if(!SaveHeader(fileName,f,type,totalCount,totalTime))
    return;

  // Save hash table
  hashTable.SaveTable(f);

}

void Kangaroo::SaveServerWork() {

  saveRequest = true;

  double t0 = Timer::get_tick();

  string fileName = workFile;
  if(splitWorkfile)
    fileName = workFile + "_" + Timer::getTS();

  FILE *f = fopen(fileName.c_str(),"wb");
  if(f == NULL) {
    ::printf("\nSaveWork: Cannot open %s for writing\n",fileName.c_str());
    ::printf("%s\n",::strerror(errno));
    saveRequest = false;
    return;
  }

  SaveWork(fileName,f,HEADW,0,0);

  uint64_t totalWalk = 0;
  ::fwrite(&totalWalk,sizeof(uint64_t),1,f);

  uint64_t size = FTell(f);
  fclose(f);

  if(splitWorkfile)
    hashTable.Reset();

  double t1 = Timer::get_tick();

  char *ctimeBuff;
  time_t now = time(NULL);
  ctimeBuff = ctime(&now);
  ::printf("done [%.1f MB] [%s] %s",(double)size / (1024.0*1024.0),GetTimeStr(t1 - t0).c_str(),ctimeBuff);

  saveRequest = false;

}

void Kangaroo::SaveWork(uint64_t totalCount,double totalTime,TH_PARAM *threads,int nbThread) {

  uint64_t totalWalk = 0;
  uint64_t size;

  LOCK(saveMutex);

  double t0 = Timer::get_tick();

  // Wait that all threads blocks before saving works
  saveRequest = true;
  int timeout = wtimeout;
  while(!isWaiting(threads) && timeout>0) {
    Timer::SleepMillis(50);
    timeout -= 50;
  }

  if(timeout<=0) {
    // Thread blocked or ended !
    if(!endOfSearch)
      ::printf("\nSaveWork timeout !\n");
    UNLOCK(saveMutex);
    return;
  }

  string fileName = workFile;
  if(splitWorkfile)
    fileName = workFile + "_" + Timer::getTS();

  // Save
  FILE* f = NULL;
  if(!saveKangarooByServer) {
    f = fopen(fileName.c_str(),"wb");
    if(f == NULL) {
      ::printf("\nSaveWork: Cannot open %s for writing\n",fileName.c_str());
      ::printf("%s\n",::strerror(errno));
      UNLOCK(saveMutex);
      return;
    }
  }

  if (clientMode) {

    if(saveKangarooByServer) {

      ::printf("\nSaveWork (Kangaroo->Server): %s",fileName.c_str());
      vector<int128_t> kangs;
      for(int i = 0; i < nbThread; i++)
        totalWalk += threads[i].nbKangaroo;
      kangs.reserve(totalWalk);

      for(int i = 0; i < nbThread; i++) {
        int128_t X;
        int128_t D;
        uint64_t h;
        for(uint64_t n = 0; n < threads[i].nbKangaroo; n++) {
          HashTable::Convert(&threads[i].px[n],&threads[i].distance[n],n%2,&h,&X,&D);
          kangs.push_back(D);
        }
      }
      SendKangaroosToServer(fileName,kangs);
      size = kangs.size()*16 + 16;
      goto end;

    } else {
      SaveHeader(fileName,f,HEADK,totalCount,totalTime);
      ::printf("\nSaveWork (Kangaroo): %s",fileName.c_str());
    }

  } else {

    SaveWork(fileName,f,HEADW,totalCount,totalTime);

  }


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

  size = FTell(f);
  fclose(f);

  if(splitWorkfile)
    hashTable.Reset();

  // Unblock threads
end:
  saveRequest = false;
  UNLOCK(saveMutex);

  double t1 = Timer::get_tick();

  char *ctimeBuff;
  time_t now = time(NULL);
  ctimeBuff = ctime(&now);
  ::printf("done [%.1f MB] [%s] %s",(double)size/(1024.0*1024.0),GetTimeStr(t1 - t0).c_str(),ctimeBuff);

}

void Kangaroo::WorkInfo(std::string &fName) {

  int isDir = IsDir(fName);
  if(isDir<0)
    return;

  string fileName = fName;
  if(isDir)
    fileName = fName + "/header";

  ::printf("Loading: %s\n",fileName.c_str());

  uint32_t version;
  FILE *f1 = ReadHeader(fileName,&version,HEADW);
  if(f1 == NULL)
    return;

#ifndef WIN64
  int fd = fileno(f1);
  posix_fadvise(fd,0,0,POSIX_FADV_RANDOM|POSIX_FADV_NOREUSE);
#endif

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
    ::printf("WorkInfo: key1 does not lie on elliptic curve\n");
    fclose(f1);
    return;
  }

  // Read hashTable
  if(isDir) {
    for(int i = 0; i < MERGE_PART; i++) {
      FILE* f = OpenPart(fName,"rb",i);
      hashTable.SeekNbItem(f,i * H_PER_PART,(i + 1) * H_PER_PART);
      fclose(f);
    }
  } else {
    hashTable.SeekNbItem(f1);
  }

  ::printf("Version   : %d\n",version);
  ::printf("DP bits   : %d\n",dp1);
  ::printf("Start     : %s\n",RS1.GetBase16().c_str());
  ::printf("Stop      : %s\n",RE1.GetBase16().c_str());
  ::printf("Key       : %s\n",secp->GetPublicKeyHex(true,k1).c_str());
#ifdef WIN64
  ::printf("Count     : %I64d 2^%.3f\n",count1,log2(count1));
#else
  ::printf("Count     : %" PRId64 " 2^%.3f\n",count1,log2(count1));
#endif
  ::printf("Time      : %s\n",GetTimeStr(time1).c_str());
  hashTable.PrintInfo();

  fread(&nbLoadedWalk,sizeof(uint64_t),1,f1);
#ifdef WIN64
  ::printf("Kangaroos : %I64d 2^%.3f\n",nbLoadedWalk,log2(nbLoadedWalk));
#else
  ::printf("Kangaroos : %" PRId64 " 2^%.3f\n",nbLoadedWalk,log2(nbLoadedWalk));
#endif

  fclose(f1);

}
