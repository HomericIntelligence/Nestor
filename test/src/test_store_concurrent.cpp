// test_store_concurrent.cpp — StoreConcurrencyTest suite for projectnestor::Store
//
// LOCKING INVARIANT (verified against src/store.cpp on main, issue #93):
//   submit_research() inserts into research_items_ AND increments pending_
//   inside the same lock_guard<std::mutex>(mutex_) block (src/store.cpp:101-113).
//   complete_research() does find + erase + --pending_ + ++completed_ all under
//   mutex_ (src/store.cpp:118-138). evict_expired_pending_locked() only
//   decrements pending_ (src/store.cpp:79), never increments. pending_ and
//   completed_ are plain `int`, not atomics — every read (get_stats,
//   src/store.cpp:61-71) and every write is mutex_-guarded.
//
//   Consequence: any get_stats() snapshot is internally consistent with the
//   map state at the moment the lock was held, so a mid-flight invariant of
//   pending + completed <= ops_started_so_far is valid and asserted in
//   ConcurrentReaders_DoNotCrashOrTearJsonStats below.
//
// If any test in this suite reveals a counter value inconsistent with the
// total operation count, that is a real bug per the "flag rather than mask"
// principle (see HomericIntelligence team knowledge base).

#include "projectnestor/store.hpp"

#include <atomic>
#include <barrier>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace projectnestor::test {

namespace {

// run_parallel — release all `n_threads` threads simultaneously using a
// std::barrier, then join() every thread before returning.  The caller's
// `body(thread_index)` lambda is invoked once per thread after the barrier
// opens.  All assertions on shared state belong AFTER this call returns.
void run_parallel(int n_threads, std::function<void(int)> body) {
  std::barrier start_gate(n_threads);
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(n_threads));

  for (int t = 0; t < n_threads; ++t) {
    threads.emplace_back([&start_gate, &body, t]() {
      start_gate.arrive_and_wait();  // synchronise all threads before work
      body(t);
    });
  }

  for (auto& th : threads) {
    th.join();
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Test 1: SubmitOnly_ProducesExactPendingCount
//
// 8 threads each call submit_research() 1000 times.
// At quiescence: pending == 8000, completed == 0.
// ---------------------------------------------------------------------------
TEST(StoreConcurrencyTest, SubmitOnly_ProducesExactPendingCount) {
  constexpr int kThreads = 8;
  constexpr int kItersPerThread = 1000;
  constexpr int kTotal = kThreads * kItersPerThread;

  Store store;

  run_parallel(kThreads, [&](int /*t*/) {
    for (int i = 0; i < kItersPerThread; ++i) {
      store.submit_research({{"idea", "concurrent idea"}, {"context", "ctx"}});
    }
  });

  // Quiescent assertions — all ++pending_ atomics have retired by this point.
  const json stats = store.get_stats();
  EXPECT_EQ(stats["pending"].get<int>(), kTotal)
      << "pending counter must equal total submitted at quiescence";
  EXPECT_EQ(stats["completed"].get<int>(), 0);
}

// ---------------------------------------------------------------------------
// Test 2: CompleteOnly_ProducesExactCompletedCount
//
// Pre-submit kTotal items single-threaded to capture IDs.
// 8 threads each complete their assigned slice of IDs.
// At quiescence: completed == kTotal, pending == 0.
// ---------------------------------------------------------------------------
TEST(StoreConcurrencyTest, CompleteOnly_ProducesExactCompletedCount) {
  constexpr int kThreads = 8;
  constexpr int kItersPerThread = 1000;
  constexpr int kTotal = kThreads * kItersPerThread;

  Store store;

  // Single-threaded setup — no race here.
  std::vector<std::string> ids;
  ids.reserve(static_cast<std::size_t>(kTotal));
  for (int i = 0; i < kTotal; ++i) {
    const json r = store.submit_research({{"idea", "pre-submitted"}});
    ids.push_back(r["id"].get<std::string>());
  }

  ASSERT_EQ(store.get_stats()["pending"].get<int>(), kTotal)
      << "setup sanity: all submitted before concurrent completion";

  // Each thread completes its own slice — no ID overlap, no contention on
  // the same item.
  run_parallel(kThreads, [&](int t) {
    const int start = t * kItersPerThread;
    for (int i = start; i < start + kItersPerThread; ++i) {
      const json result = store.complete_research(ids[static_cast<std::size_t>(i)]);
      EXPECT_EQ(result["status"].get<std::string>(), "completed")
          << "each pre-submitted item must complete exactly once";
    }
  });

  const json stats = store.get_stats();
  EXPECT_EQ(stats["completed"].get<int>(), kTotal);
  EXPECT_EQ(stats["pending"].get<int>(), 0);
}

// ---------------------------------------------------------------------------
// Test 3: MixedSubmitComplete_QuiescentInvariant
//
// Producer threads submit; consumer threads complete from a thread-safe queue
// of IDs.  At quiescence: pending + completed == total_submitted.
// Tolerates the submit_research asymmetry because all ++pending_ have retired.
// ---------------------------------------------------------------------------
TEST(StoreConcurrencyTest, MixedSubmitComplete_QuiescentInvariant) {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kSubmitsPerProducer = 500;
  constexpr int kTotal = kProducers * kSubmitsPerProducer;

  Store store;

  // Thread-safe ID queue — producers push, consumers pop.
  std::mutex queue_mutex;
  std::vector<std::string> id_queue;
  id_queue.reserve(static_cast<std::size_t>(kTotal));
  std::atomic<int> total_submitted{0};
  std::atomic<bool> producers_done{false};

  // Producer threads
  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  std::barrier producer_gate(kProducers);
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&]() {
      producer_gate.arrive_and_wait();
      for (int i = 0; i < kSubmitsPerProducer; ++i) {
        const json r = store.submit_research({{"idea", "mixed idea"}});
        const std::string id = r["id"].get<std::string>();
        {
          std::lock_guard<std::mutex> lk(queue_mutex);
          id_queue.push_back(id);
        }
        ++total_submitted;
      }
    });
  }

  // Consumer threads
  std::vector<std::thread> consumers;
  consumers.reserve(kConsumers);
  std::barrier consumer_gate(kConsumers);
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&]() {
      consumer_gate.arrive_and_wait();
      while (true) {
        std::string id;
        {
          std::lock_guard<std::mutex> lk(queue_mutex);
          if (id_queue.empty()) {
            if (producers_done.load()) {
              break;
            }
            continue;
          }
          id = id_queue.back();
          id_queue.pop_back();
        }
        store.complete_research(id);
      }
    });
  }

  for (auto& t : producers) t.join();
  producers_done.store(true);
  for (auto& t : consumers) t.join();

  // Quiescent invariant — pending + completed must equal total submitted.
  // (pending might be > 0 if some IDs weren't popped before producers_done.)
  const json stats = store.get_stats();
  const int final_pending = stats["pending"].get<int>();
  const int final_completed = stats["completed"].get<int>();
  const int expected = total_submitted.load();

  EXPECT_EQ(final_pending + final_completed, expected)
      << "pending=" << final_pending << " completed=" << final_completed
      << " total_submitted=" << expected;
  EXPECT_GE(final_completed, 0);
  EXPECT_GE(final_pending, 0);
}

// ---------------------------------------------------------------------------
// Test 4: ConcurrentReaders_DoNotCrashOrTearJsonStats
//
// Writers submit/complete; readers call get_stats() in a tight loop and verify
// each snapshot has both keys with integer types and non-negative values.
// No exact-value assertion mid-flight.  Final state asserted at quiescence.
// ---------------------------------------------------------------------------
TEST(StoreConcurrencyTest, ConcurrentReaders_DoNotCrashOrTearJsonStats) {
  constexpr int kWriters = 4;
  constexpr int kReaders = 4;
  constexpr int kOpsPerWriter = 200;

  Store store;

  std::atomic<bool> writers_done{false};
  std::atomic<int> bad_snapshot_count{0};
  // Each submit OR complete bumps ops_started BEFORE the call. Sampling
  // ops_started AFTER the get_stats() snapshot keeps the bound monotone
  // in the reader's favor: a concurrent ++ops_started can only LOOSEN,
  // never tighten, the bound (issue #93 invariant).
  std::atomic<int> ops_started{0};

  // Reader threads — spin reading stats until writers are done.
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  std::barrier reader_gate(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&]() {
      reader_gate.arrive_and_wait();
      while (!writers_done.load()) {
        const json stats = store.get_stats();
        const bool structurally_ok =
            stats.contains("pending") && stats.contains("completed") &&
            stats["pending"].is_number_integer() && stats["completed"].is_number_integer() &&
            stats["pending"].get<int>() >= 0 && stats["completed"].get<int>() >= 0;
        const int started = ops_started.load();
        const bool counters_bounded =
            structurally_ok &&
            (stats["pending"].get<int>() + stats["completed"].get<int>() <= started);
        if (!counters_bounded) {
          ++bad_snapshot_count;
        }
      }
    });
  }

  // Writer threads — submit then complete a batch of items.
  std::barrier writer_gate(kWriters);
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&]() {
      writer_gate.arrive_and_wait();
      for (int i = 0; i < kOpsPerWriter; ++i) {
        ops_started.fetch_add(1, std::memory_order_relaxed);
        const json r = store.submit_research({{"idea", "reader stress"}});
        ops_started.fetch_add(1, std::memory_order_relaxed);
        // Fail loudly if submit_research did not return an id (e.g. a
        // {"error","capacity"} result) rather than throwing in get<string>().
        ASSERT_TRUE(r.contains("id")) << "submit_research must return an 'id' to complete";
        store.complete_research(r["id"].get<std::string>());
      }
    });
  }

  for (auto& t : writers) t.join();
  writers_done.store(true);
  for (auto& t : readers) t.join();

  EXPECT_EQ(bad_snapshot_count.load(), 0)
      << "every get_stats() snapshot must have non-negative integer values "
         "AND pending+completed must not exceed ops_started (issue #93)";

  // Final quiescent assertion.
  const json stats = store.get_stats();
  EXPECT_EQ(stats["pending"].get<int>(), 0);
  EXPECT_EQ(stats["completed"].get<int>(), kWriters * kOpsPerWriter);
}

// ---------------------------------------------------------------------------
// Test 5: DuplicateComplete_AtMostOneSuccess
//
// Submit one item. 8 threads each try to complete it 1000 times.
// At quiescence: completed == 1, and all other calls returned not_found.
// A failure (completed > 1) surfaces a real locking bug.
// ---------------------------------------------------------------------------
TEST(StoreConcurrencyTest, DuplicateComplete_AtMostOneSuccess) {
  constexpr int kThreads = 8;
  constexpr int kAttemptsPerThread = 1000;

  Store store;

  const json submit_result = store.submit_research({{"idea", "duplicate test"}});
  const std::string id = submit_result["id"].get<std::string>();

  std::atomic<int> success_count{0};

  run_parallel(kThreads, [&](int /*t*/) {
    for (int i = 0; i < kAttemptsPerThread; ++i) {
      const json result = store.complete_research(id);
      if (result.contains("status") && result["status"].get<std::string>() == "completed") {
        ++success_count;
      } else {
        // Every non-success must return the canonical not_found error.
        EXPECT_TRUE(result.contains("error")) << "non-success result must contain 'error' key";
        EXPECT_EQ(result["error"].get<std::string>(), "not_found")
            << "non-success result must be 'not_found'";
      }
    }
  });

  // Exactly one completion must have succeeded — the map entry is removed on
  // first complete, so all subsequent calls hit not_found.
  EXPECT_EQ(success_count.load(), 1)
      << "exactly one complete_research() call must succeed for a single item; "
         "success_count="
      << success_count.load() << " indicates a real locking bug";

  const json stats = store.get_stats();
  EXPECT_EQ(stats["completed"].get<int>(), 1);
  EXPECT_EQ(stats["pending"].get<int>(), 0);
}

}  // namespace projectnestor::test
