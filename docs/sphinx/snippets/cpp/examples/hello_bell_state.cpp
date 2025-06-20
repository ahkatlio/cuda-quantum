/*******************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// Compile and run with: `nvq++ hello_bell_state.cpp && ./a.out`

#include <cudaq.h>
#include <stdio.h> // For printf

// [Begin Bell State C++]
struct bell {
  int operator()(int num_iters) __qpu__ {
    cudaq::qarray<2> q;
    int nCorrect = 0;
    for (int i = 0; i < num_iters; i++) {
      h(q[0]);
      x<cudaq::ctrl>(q[0], q[1]);
      auto results = mz(q);
      if (results[0] == results[1])
        nCorrect++;

      reset(q[0]);
      reset(q[1]);
    }
    return nCorrect;
  }
};

int main() {
  printf("N Correct = %d\n", bell{}(100));
  return 0;
}
// [End Bell State C++]

