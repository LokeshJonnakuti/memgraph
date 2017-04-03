#pragma once

#include "utils/likely.hpp"
#include "utils/random/xorshift128plus.hpp"

template <class R = Xorshift128plus>
class FastBinomial {
  // fast binomial draws coin tosses from a single generated random number
  // let's draw a random 4 bit number and count trailing ones
  //
  // 1  0000 -> 1 =
  // 2  0001 -> 2 ==      8 x =     p = 8/16 = 1/2
  // 3  0010 -> 1 =       4 x ==    p = 4/16 = 1/4     p_total = 15/16
  // 4  0011 -> 3 ===     2 x ===   p = 2/16 = 1/8
  // 5  0100 -> 1 =       1 x ====  p = 1/16 = 1/16
  // 6  0101 -> 2 ==     --------------------------
  // 7  0110 -> 1 =       1 x ===== p = 1/16 invalid value, retry!
  // 8  0111 -> 4 ====
  // 9  1000 -> 1 =
  // 10 1001 -> 2 ==
  // 11 1010 -> 1 =
  // 12 1011 -> 3 ===
  // 13 1100 -> 1 =
  // 14 1101 -> 2 ==
  // 15 1110 -> 1 =
  // ------------------
  // 16 1111 -> 5 =====

 public:
  /**
   * Return random number X between 1 and tparam N with probability 2^-X.
   */
  unsigned operator()(const int n) {
    while (true) {
      // couting trailing ones is equal to counting trailing zeros
      // since the probability for both is 1/2 and we're going to
      // count zeros because they are easier to work with

      // generate a random number
      auto x = random() & mask(n);

      // if we have all zeros, then we have an invalid case and we
      // need to generate again, we have this every (1/2)^N times
      // so therefore we could say it's very unlikely to happen for
      // large N. e.g. N = 32; p = 2.328 * 10^-10
      if (UNLIKELY(!x)) continue;

      // ctzl = count trailing zeros from long
      //        ^     ^        ^          ^
      return __builtin_ctzl(x) + 1;
    }
  }

 private:
  uint64_t mask(const int n) { return (1ULL << n) - 1; }
  R random;
};
