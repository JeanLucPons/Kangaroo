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

#include "HashTable.h"
#include <stdio.h>
#include <math.h>
#ifndef WIN64
#include <string.h>
#endif

#define GET(hash,id) E[hash].items[id]
#define safe_free(x) if(x) {free(x);x=NULL;}


HashTable::HashTable() {

  memset(E,0,sizeof(E));

}

void HashTable::Reset() {

  for(uint32_t h = 0; h < HASH_SIZE; h++) {
    for(uint32_t i = 0; i<E[h].nbItem; i++)
      free(E[h].items[i]);
    safe_free(E[h].items);
    E[h].maxItem = 0;
    E[h].nbItem = 0;
  }

}

uint64_t HashTable::GetNbItem() {

  uint64_t totalItem = 0;
  for(uint64_t h = 0; h < HASH_SIZE; h++) 
    totalItem += (uint64_t)E[h].nbItem;

  return totalItem;

}

ENTRY *HashTable::CreateEntry(int128_t *x,int128_t *d) {

  ENTRY *e = (ENTRY *)malloc(sizeof(ENTRY));
  e->x.i64[0] = x->i64[0];
  e->x.i64[1] = x->i64[1];
  e->d.i64[0] = d->i64[0];
  e->d.i64[1] = d->i64[1];
  return e;

}

#define ADD_ENTRY(entry) {                 \
  /* Shift the end of the index table */   \
  for (int i = E[h].nbItem; i > st; i--)   \
    E[h].items[i] = E[h].items[i - 1];     \
  E[h].items[st] = entry;                  \
  E[h].nbItem++;}

void HashTable::Convert(Int *x,Int *d,uint32_t type,uint64_t *h,int128_t *X,int128_t *D) {

  uint64_t sign = 0;
  uint64_t type64 = (uint64_t)type << 62;

  X->i64[0] = x->bits64[0];
  X->i64[1] = x->bits64[1];

  // Probability of failure (1/2^128)
  if(d->bits64[3] > 0x7FFFFFFFFFFFFFFFULL) {
    Int N(d);
    N.ModNegK1order();
    D->i64[0] = N.bits64[0];
    D->i64[1] = N.bits64[1] & 0x3FFFFFFFFFFFFFFFULL;
    sign = 1ULL << 63;
  } else {
    D->i64[0] = d->bits64[0];
    D->i64[1] = d->bits64[1] & 0x3FFFFFFFFFFFFFFFULL;
  }

  D->i64[1] |= sign;
  D->i64[1] |= type64;

  *h = (x->bits64[2] & HASH_MASK);

}

bool HashTable::Add(Int *x,Int *d,uint32_t type) {

  int128_t X;
  int128_t D;
  uint64_t h;
  Convert(x,d,type,&h,&X,&D);
  return Add(h,&X,&D);

}

bool HashTable::Add(uint64_t h,int128_t *x,int128_t *d) {

  if(E[h].maxItem == 0) {
    E[h].maxItem = 16;
    E[h].items = (ENTRY **)malloc(sizeof(ENTRY *) * E[h].maxItem);
  }

  if(E[h].nbItem == 0) {
    E[h].items[0] = CreateEntry(x,d);
    E[h].nbItem = 1;
    return false;
  }

  if(E[h].nbItem >= E[h].maxItem - 1) {
    // We need to reallocate
    E[h].maxItem += 4;
    ENTRY **nitems = (ENTRY **)malloc(sizeof(ENTRY *) * E[h].maxItem);
    memcpy(nitems,E[h].items,sizeof(ENTRY *) * E[h].nbItem);
    free(E[h].items);
    E[h].items = nitems;
  }

  // Search insertion position
  int st,ed,mi;
  st = 0; ed = E[h].nbItem - 1;
  while(st <= ed) {
    mi = (st + ed) / 2;
    int comp = compare(x,&GET(h,mi)->x);
    if(comp<0) {
      ed = mi - 1;
    } else if (comp==0) {

      // Collision
      int128_t d = GET(h,mi)->d;
      kType = (d.i64[1] & 0x4000000000000000ULL) != 0;
      int sign = (d.i64[1] & 0x8000000000000000ULL) != 0;
      d.i64[1] &= 0x3FFFFFFFFFFFFFFFULL;

      kDist.SetInt32(0);
      kDist.bits64[0] = d.i64[0];
      kDist.bits64[1] = d.i64[1];
      if(sign) kDist.ModNegK1order();

      return true;

    } else {
      st = mi + 1;
    }
  }

  ENTRY *entry = CreateEntry(x,d);
  ADD_ENTRY(entry);
  return false;

}

int HashTable::compare(int128_t *i1,int128_t *i2) {

  uint64_t *a = i1->i64;
  uint64_t *b = i2->i64;

  if(a[1] == b[1]) {
    if(a[0] == b[0]) {
      return 0;
    } else {
      return (a[0] > b[0]) ? 1 : -1;
    }
  } else {
    return (a[1] > b[1]) ? 1 : -1;
  }

}

Int *HashTable::GetD() {
  return &kDist;
}

uint32_t HashTable::GetType() {
  return kType;
}

std::string HashTable::GetSizeInfo() {

  uint64_t totalByte = sizeof(E);
  uint64_t usedByte = HASH_SIZE*2*sizeof(uint32_t);

  for (int h = 0; h < HASH_SIZE; h++) {
    totalByte += sizeof(ENTRY *) * E[h].maxItem;
    totalByte += sizeof(ENTRY) * E[h].nbItem;
    usedByte += sizeof(ENTRY) * E[h].nbItem;
  }

  double totalMB = (double)totalByte / (1024.0*1024.0);
  double usedMB = (double)usedByte / (1024.0*1024.0);

  char ret[256];
  sprintf(ret,"%.1f/%.1fMB",usedMB,totalMB);

  return std::string(ret);

}

std::string HashTable::GetStr(int128_t *i) {

  std::string ret;
  char tmp[256];
  for(int n=3;n>=0;n--) {
    ::sprintf(tmp,"%08X",i->i32[n]); 
    ret += std::string(tmp);
  }
  return ret;

}

void HashTable::SaveTable(FILE *f) {

  uint64_t point = GetNbItem() / 16;
  uint64_t pointPrint = 0;

  for(uint32_t h = 0; h < HASH_SIZE; h++) {
    fwrite(&E[h].nbItem,sizeof(uint32_t),1,f);
    fwrite(&E[h].maxItem,sizeof(uint32_t),1,f);
    for(uint32_t i = 0; i < E[h].nbItem; i++) {
      fwrite(&(E[h].items[i]->x),16,1,f);
      fwrite(&(E[h].items[i]->d),16,1,f);
      pointPrint++;
      if(pointPrint>point) {
        ::printf(".");
        pointPrint = 0;
      }
    }
  }

}

void HashTable::LoadTable(FILE *f) {

  uint32_t totalItem = 0;

  Reset();
  for(uint32_t h = 0; h < HASH_SIZE; h++) {
    
    fread(&E[h].nbItem,sizeof(uint32_t),1,f);
    fread(&E[h].maxItem,sizeof(uint32_t),1,f);

    if(E[h].maxItem > 0)
      // Allocate indexes
      E[h].items = (ENTRY **)malloc(sizeof(ENTRY *) * E[h].maxItem);

    for(uint32_t i = 0; i < E[h].nbItem; i++) {
      ENTRY *e = (ENTRY *)malloc(sizeof(ENTRY));
      fread(&(e->x),16,1,f);
      fread(&(e->d),16,1,f);
      E[h].items[i] = e;
    }
    totalItem += E[h].nbItem;

  }

  //printf("HashTable::LoadTable(): %d items loaded\n",totalItem);

}

void HashTable::PrintInfo() {

  uint16_t max = 0;
  uint32_t maxH = 0;
  uint16_t min = 65535;
  uint32_t minH = 0;
  double std = 0;
  double avg = (double)GetNbItem() / (double)HASH_SIZE;

  for(uint32_t h=0;h<HASH_SIZE;h++) {
    if(E[h].nbItem>max) {
      max= E[h].nbItem;
      maxH = h;
    }
    if(E[h].nbItem<min) {
      min= E[h].nbItem;
      minH = h;
    }
    std += (avg - (double)E[h].nbItem)*(avg - (double)E[h].nbItem);
  }
  std /= (double)HASH_SIZE;
  std = sqrt(std);

  uint64_t count = GetNbItem();

  ::printf("DP Size   : %s\n",GetSizeInfo().c_str());
#ifdef WIN64
  ::printf("DP Count  : %I64d 2^%.3f\n",count,log2(count));
#else
  ::printf("DP Count  : %" PRId64 " 2^%.3f\n",count,log2(count));
#endif
  ::printf("HT Max    : %d [@ %06X]\n",max,maxH);
  ::printf("HT Min    : %d [@ %06X]\n",min,minH);
  ::printf("HT Avg    : %.2f \n",avg);
  ::printf("HT SDev   : %.2f \n",std);

  //for(int i=0;i<(int)E[maxH].nbItem;i++) {
  //  ::printf("[%2d] %s\n",i,GetStr(&E[maxH].items[i]->x).c_str());
  //  ::printf("[%2d] %s\n",i,GetStr(&E[maxH].items[i]->d).c_str());
  //}

}
