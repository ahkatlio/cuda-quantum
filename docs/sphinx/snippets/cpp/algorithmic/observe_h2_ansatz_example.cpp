#include <cudaq.h>
#include <stdio.h>

// [Begin H2 Ansatz C++]
struct ansatz_h2 {
  auto operator()(double theta) __qpu__ {
    cudaq::qarray<2> q;
    x(q[0]);
    ry(theta, q[1]);
    x<cudaq::ctrl>(q[1], q[0]);
  }
};

int main() {
  cudaq::spin_op h = 5.907 - 2.1433 * cudaq::spin_op::x(0) * cudaq::spin_op::x(1) -
                   2.1433 * cudaq::spin_op::y(0) * cudaq::spin_op::y(1) +
                   .21829 * cudaq::spin_op::z(0) - 6.125 * cudaq::spin_op::z(1);

  double energy = cudaq::observe(ansatz_h2{}, h, .59);
  printf("Energy is %lf\n", energy);
  return 0;
}