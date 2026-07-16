#include <atomic>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

namespace Nestor {

class SanitizerProof : public ::testing::Test {};

TEST_F(SanitizerProof, HeapUseAfterFree) {
#ifndef NESTOR_SANITIZER_ADDRESS
  GTEST_SKIP() << "AddressSanitizer not enabled";
#else
  // Allocate memory, free it, and then use it — ASAN detects this
  int* ptr = new int(42);
  delete ptr;
  // This access should be caught by ASAN
  [[maybe_unused]] int value = *ptr;
#endif
}

TEST_F(SanitizerProof, DataRace) {
#ifndef NESTOR_SANITIZER_THREAD
  GTEST_SKIP() << "ThreadSanitizer not enabled";
#else
  // Shared non-atomic variable accessed from two threads without synchronization
  int shared_var = 0;

  std::thread t1([&shared_var]() {
    for (int i = 0; i < 100; ++i) {
      shared_var = i;
    }
  });

  std::thread t2([&shared_var]() {
    for (int i = 0; i < 100; ++i) {
      [[maybe_unused]] int x = shared_var;
    }
  });

  t1.join();
  t2.join();
#endif
}

}  // namespace Nestor
