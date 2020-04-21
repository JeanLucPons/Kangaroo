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

#ifndef HASHTABLEH
#define HASHTABLEH

#include <string>
#include <vector>
#include "SECPK1/Point.h"
#ifdef WIN64
#include <Windows.h>
#endif

#define HASH_SIZE_BIT 16
#define HASH_SIZE (1<<HASH_SIZE_BIT)
#define HASH_MASK (HASH_SIZE-1)

#define TAME 0  // Tame kangaroo
#define WILD 1  // Wild kangaroo

typedef struct {

  int   type;      // Kangaroo type (TAME or WILD)
  Point pos;       // Current position
  Int   distance;  // Travelled distance

} KANGAROO;


union int256_s {

  uint8_t  i8[32];
  uint16_t i16[16];
  uint32_t i32[8];
  uint64_t i64[4];

};

typedef union int256_s int256_t;

typedef struct {

  uint32_t  type; // Kangoroo type
  int256_s  x;    // Poisition of kangaroo
  int256_s  d;    // Travelled distance

} ENTRY;

typedef struct {

  uint32_t   nbItem;
  uint32_t   maxItem;
  ENTRY    **items;

} HASH_ENTRY;

class HashTable {

public:

  HashTable();
  bool Add(Int *x,KANGAROO *k);
  ENTRY *CreateEntry(int256_t *i,KANGAROO *k);
  int HashTable::compare(int256_t *i1,int256_t *i2);
  uint64_t GetNbItem();
  void Reset();
  double GetSizeMB();
  void PrintInfo();

  Int *GetD();
  uint32_t GetType();

private:

  std::string GetStr(int256_t *i);

  HASH_ENTRY    E[HASH_SIZE];

  // Collision
  Int d;
  uint32_t type;

};

#endif // HASHTABLEH
