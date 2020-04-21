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

#include "Timer.h"
#include "Kangaroo.h"
#include "SECPK1/SECP256k1.h"
#include <fstream>
#include <string>
#include <string.h>
#include <stdexcept>

#define RELEASE "1.0"

using namespace std;

// ------------------------------------------------------------------------------------------

void printUsage() {

  printf("Kangaroo [-v] [-t nbThread] [-d dpBit] inFile\n");
  printf(" -v: Print version\n");
  printf(" -d: Specify number of leading zeros for the DP method (default is auto)\n");
  printf(" -t nbThread: Secify number of thread\n");
  printf(" inFile: intput configuration file\n");
  exit(0);

}

// ------------------------------------------------------------------------------------------

int getInt(string name,char *v) {

  int r;

  try {

    r = std::stoi(string(v));

  } catch(std::invalid_argument&) {

    printf("Invalid %s argument, number expected\n",name.c_str());
    exit(-1);

  }

  return r;

}

// ------------------------------------------------------------------------------------------

int main(int argc, char* argv[]) {

  // Global Init
  Timer::Init();
  rseed((unsigned long)time(NULL));

  // Init SecpK1
  Secp256K1 *secp = new Secp256K1();
  secp->Init();

  int a = 1;
  int dp = -1;
  int nbCPUThread = Timer::getCoreNumber();
  string configFile = "";

  while (a < argc) {

    if(strcmp(argv[a], "-t") == 0) {
      a++;
      nbCPUThread = getInt("nbCPUThread",argv[a]);
      a++;
    } else if(strcmp(argv[a],"-d") == 0) {
      a++;
      dp = getInt("dpSize",argv[a]);
      a++;
    } else if (strcmp(argv[a], "-h") == 0) {
      printUsage();
    } else if (a == argc - 1) {
      configFile = string(argv[a]);
      a++;
    } else {
      printf("Unexpected %s argument\n",argv[a]);
      exit(-1);
    }

  }

  printf("Kangaroo v" RELEASE "\n");

  Kangaroo *v = new Kangaroo(secp,dp);
  if( !v->ParseConfigFile(configFile) )
    exit(-1);
  v->Run(nbCPUThread);

  return 0;

}
