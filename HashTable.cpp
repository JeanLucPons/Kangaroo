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

ENTRY *HashTable::CreateEntry(int256_t *i,KANGAROO *k) {
  ENTRY *e = (ENTRY *)malloc(sizeof(ENTRY));
  e->type = k->type;
  e->x = *i;
  e->d = *(int256_t *)(k->distance.bits64);
  return e;
}

#define ADD_ENTRY(entry) {                 \
  /* Shift the end of the index table */   \
  for (int i = E[h].nbItem; i > st; i--)   \
    E[h].items[i] = E[h].items[i - 1];     \
  E[h].items[st] = entry;                  \
  E[h].nbItem++;}

bool HashTable::Add(Int *x,KANGAROO *k) {

  uint64_t h = (x->bits64[3] & HASH_MASK);

  if(E[h].maxItem == 0) {
    E[h].maxItem = 16;
    E[h].items = (ENTRY **)malloc(sizeof(ENTRY *) * E[h].maxItem);
  }

  if(E[h].nbItem == 0) {
    E[h].items[0] = CreateEntry((int256_t *)(x->bits64),k);
    E[h].nbItem = 1;
    return false;
  }

  if(E[h].nbItem >= E[h].maxItem - 1) {
    // We need to reallocate
    E[h].maxItem += 16;
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
    int comp = compare((int256_t *)(x->bits64),&GET(h,mi)->x);
    if(comp<0) {
      ed = mi - 1;
    } else if (comp==0) {
      // Collision
      d.SetInt32(0);
      memcpy(d.bits64,&(GET(h,mi)->d),sizeof(int256_t));
      type = GET(h,mi)->type;
      return true;
    } else {
      st = mi + 1;
    }
  }

  ENTRY *entry = CreateEntry((int256_t *)(x->bits64),k);
  ADD_ENTRY(entry);
  return false;

}

int HashTable::compare(int256_t *i1,int256_t *i2) {

  uint64_t *a = i1->i64;
  uint64_t *b = i2->i64;
  int i;

  for(i = 0; i<4; i++) {
    if(a[i] != b[i])
      break;
  }

  if(i<4) {
    uint64_t ai = _byteswap_uint64(a[i]);
    uint64_t bi = _byteswap_uint64(b[i]);
    if(ai>bi) return 1;
    else      return -1;
  } else {
    return 0;
  }

}

Int *HashTable::GetD() {
  return &d;
}

uint32_t HashTable::GetType() {
  return type;
}

double HashTable::GetSizeMB() {

  uint64_t byte = sizeof(E);

  for (int h = 0; h < HASH_SIZE; h++) {
    byte += sizeof(ENTRY *) * E[h].maxItem;
    byte += sizeof(ENTRY) * E[h].nbItem;
  }

  return (double)byte / (1024.0*1024.0);

}

std::string HashTable::GetStr(int256_t *i) {

  std::string ret;
  char tmp[256];
  for(int n=7;n>=0;n--) {
    ::sprintf(tmp,"%08X",i->i32[n]); 
    ret += std::string(tmp);
  }
  return ret;

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

  ::printf("Size: %f MB\n",GetSizeMB());
  ::printf("Item: 2^%.2f \n",log2((double)GetNbItem()));
  ::printf("Max : %d [@ %06X]\n",max,maxH);
  ::printf("Min : %d [@ %06X]\n",min,minH);
  ::printf("Avg : %.2f \n",avg);
  ::printf("SDev: %.2f \n",std);

  for(int i=0;i<(int)E[maxH].nbItem;i++) {
    ::printf("[%2d] %s\n",i,GetStr(&E[maxH].items[i]->x).c_str());
  }

}
