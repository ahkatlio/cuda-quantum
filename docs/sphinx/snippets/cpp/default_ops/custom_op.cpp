/*******************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// Compile and run with: `nvq++ custom_op.cpp && ./a.out`

#include <cudaq.h>

CUDAQ_REGISTER_OPERATION(custom_h, 1, 0,
                         {M_SQRT1_2, M_SQRT1_2, M_SQRT1_2, -M_SQRT1_2})

CUDAQ_REGISTER_OPERATION(custom_x, 1, 0, {0, 1, 1, 0})

__qpu__ void bell_pair() {
  cudaq::qubit q, r;
  custom_h(q);
  custom_x<cudaq::ctrl>(q, r);
}

int main() {
  // [Begin Custom Op]
  auto counts = cudaq::sample(bell_pair);
  for (auto &[bits, count] : counts) {
    printf("%s\n", bits.data());
  }

  return 0;
}
// [End Custom Op]

