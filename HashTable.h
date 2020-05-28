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

#ifndef HASHTABLEH
#define HASHTABLEH

#include <string>
#include <vector>
#include "SECPK1/Point.h"
#ifdef WIN64
#include <Windows.h>
#endif

#define HASH_SIZE_BIT 18
#define HASH_SIZE (1<<HASH_SIZE_BIT)
#define HASH_MASK (HASH_SIZE-1)

#define ADD_OK        0
#define ADD_DUPLICATE 1
#define ADD_COLLISION 2

union int128_s {

  uint8_t  i8[16];
  uint16_t i16[8];
  uint32_t i32[4];
  uint64_t i64[2];

};

typedef union int128_s int128_t;

// We store only 128 (+18) bit a the x value which give a probabilty a wrong collision after 2^73 entries

typedef struct {

  int128_t  x;    // Poisition of kangaroo (128bit LSB)
  int128_t  d;    // Travelled distance (b127=sign b126=kangaroo type, b125..b0 distance

} ENTRY;

typedef struct {

  uint32_t   nbItem;
  uint32_t   maxItem;
  ENTRY    **items;

} HASH_ENTRY;

class HashTable {

public:

  HashTable();
  int Add(Int *x,Int *d,uint32_t type);
  int Add(uint64_t h,int128_t *x,int128_t *d);
  int Add(uint64_t h,ENTRY *e);
  uint64_t GetNbItem();
  void Reset();
  std::string GetSizeInfo();
  void PrintInfo();
  void SaveTable(FILE *f);
  void LoadTable(FILE *f);
  Int *GetD();
  uint32_t GetType();
  void ReAllocate(uint64_t h,uint32_t add);

  HASH_ENTRY    E[HASH_SIZE];

  static void Convert(Int *x,Int *d,uint32_t type,uint64_t *h,int128_t *X,int128_t *D);

private:

  ENTRY *CreateEntry(int128_t *x,int128_t *d);
  int compare(int128_t *i1,int128_t *i2);
  std::string GetStr(int128_t *i);


  // Collision
  Int      kDist;
  uint32_t kType;


};

#endif // HASHTABLEH
