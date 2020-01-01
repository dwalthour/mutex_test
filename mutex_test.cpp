#include <iostream>
#include <iomanip>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <immintrin.h>

constexpr size_t num_runs = 100;
constexpr size_t num_loops = 16384;

struct terrible_spinlock
{
  void lock()
  {
    while (locked.test_and_set(std::memory_order_acquire))
      {
      }
  }
  void unlock()
  {
    locked.clear(std::memory_order_release);
  }
  
 private:
  std::atomic_flag locked = ATOMIC_FLAG_INIT;
};

struct spinlock_amd
{
  void lock()
  {
    for (;;)
      {
	bool was_locked = locked.load(std::memory_order_relaxed);
	if (!was_locked && locked.compare_exchange_weak(was_locked, true, std::memory_order_acquire))
	  break;
	_mm_pause();
      }
  }
  void unlock()
  {
    locked.store(false, std::memory_order_release);
  }
  
private:
  std::atomic<bool> locked{false};
};

struct spinlock
{
  void lock()
  {
    for (int spin_count = 0; !try_lock(); ++spin_count)
      {
	if (spin_count < 16)
	  _mm_pause();
	else
	  {
	    std::this_thread::yield();
	    spin_count = 0;
	  }
      }
  }
  bool try_lock()
  {
    return !locked.load(std::memory_order_relaxed) && !locked.exchange(true, std::memory_order_acquire);
  }
  void unlock()
  {
    locked.store(false, std::memory_order_release);
  }
  
 private:
  std::atomic<bool> locked{false};
};
  
struct ticket_spinlock
{
  void lock()
  {
    unsigned my = in.fetch_add(1, std::memory_order_relaxed);
    for (int spin_count = 0; out.load(std::memory_order_acquire) != my; ++spin_count)
      {
	if (spin_count < 16)
	  _mm_pause();
	else
	  {
	    std::this_thread::yield();
	    spin_count = 0;
	  }
      }
  }
  void unlock()
  {
    out.store(out.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }
  
private:
  std::atomic<unsigned> in{0};
  std::atomic<unsigned> out{0};
};


template <typename T> struct mutex_type {};
template <> struct mutex_type<std::mutex> {        static constexpr const char * name = "std::mutex       "; };
template <> struct mutex_type<terrible_spinlock> { static constexpr const char * name = "terrible_spinlock"; };
template <> struct mutex_type<spinlock_amd> {      static constexpr const char * name = "spinlock_amd     "; };
template <> struct mutex_type<spinlock> {          static constexpr const char * name = "spinlock         "; };
template <> struct mutex_type<ticket_spinlock> {   static constexpr const char * name = "ticket_spinlock  "; };


std::chrono::duration<long int, std::nano> longest_wait = std::chrono::duration<long int, std::nano>::min();
std::chrono::duration<long int, std::nano> longest_idle = std::chrono::duration<long int, std::nano>::min();
std::chrono::high_resolution_clock::time_point time_before{};
bool first = true;
std::atomic<bool> start_flag = false;


struct stats {
  std::vector<size_t> data_;

  stats(size_t size) { data_.reserve(size); }
  
  void append(size_t value) { data_.push_back(value); }

  double mean() { return (double)std::accumulate(data_.begin(), data_.end(), 0UL) / (double)data_.size(); }

  void sort() { std::sort(data_.begin(), data_.end(), std::greater<size_t>{}); }
};


template <typename MutexT, bool do_SCHED_FIFO, bool measure_idle>
void run(int cores) {

  static MutexT mutex{};

  auto f = []()
	   {	     
	     if constexpr (do_SCHED_FIFO) {
	       sched_param param;
	       param.sched_priority = 99;
	       auto rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
	       if (rc) __builtin_trap();
	     } else {
	       sched_param param;
	       param.sched_priority = 0;
	       auto rc = pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
	       if (rc) __builtin_trap();
	     }
	     while (!start_flag);
	     if constexpr (!measure_idle) {
	       for (size_t i = num_loops; i != 0; --i) {
		 auto time_before = std::chrono::high_resolution_clock::now();
		 mutex.lock();
		 auto wait_time = std::chrono::high_resolution_clock::now() - time_before;
		 longest_wait = std::max(wait_time, longest_wait);
		 mutex.unlock();
	       }
	     } else {
	       for (size_t i = num_loops; i != 0; --i) {
		 mutex.lock();
		 auto wait_time = std::chrono::high_resolution_clock::now() - time_before;
		 if (first)
		   first = false;
		 else if (wait_time > longest_idle)
		   longest_idle = wait_time;
		 time_before = std::chrono::high_resolution_clock::now();
		 mutex.unlock();
	       }
	     }
	   };

  std::thread th[1024];

  stats longest(num_runs), total(num_runs);
  for (size_t j=0; j<num_runs; ++j) {
    longest_wait = std::chrono::duration<long int, std::nano>::min();
    longest_idle = std::chrono::duration<long int, std::nano>::min();
    first = true;
    start_flag = false;

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i=0; i<cores; ++i) {
      th[i] = std::thread{f};
    }
    
    start_flag = true;
    
    for (size_t i=0; i<cores; ++i) {
      th[i].join();
    }
    auto total_time = std::chrono::high_resolution_clock::now() - start;

    if (!measure_idle) {
      longest.append(longest_wait.count());
    } else {
      longest.append(longest_idle.count());
    }
    total.append(total_time.count());
  }
  longest.sort();
  std::cout << (measure_idle ? "idle " : "wait ")
	    << mutex_type<decltype(mutex)>::name
	    << (do_SCHED_FIFO ? " SCHED_FIFO  " : " SCHED_OTHER ")
	    << std::fixed
	    << std::setprecision(2) << total.mean()/1000000.0 << "ms "
	    << std::setprecision(2) << longest.data_[0]/1000000.0 << "ms,"
	    << std::setprecision(2) << longest.data_[1]/1000000.0 << "ms,"
	    << std::setprecision(2) << longest.data_[2]/1000000.0 << "ms,"
	    << std::setprecision(2) << longest.data_[3]/1000000.0 << "ms"
	    << std::endl;
  
}

int main(int argc, const char *argv[]) {
  auto nthreads = std::thread::hardware_concurrency();

  for (size_t i=0; i<4; ++i) {
    run<std::mutex,true,false>(nthreads);
    run<terrible_spinlock,true,false>(nthreads);
    run<spinlock_amd,true,false>(nthreads);
    run<spinlock,true,false>(nthreads);
    run<ticket_spinlock,true,false>(nthreads);
    run<std::mutex,false,false>(nthreads);
    run<terrible_spinlock,false,false>(nthreads);
    run<spinlock_amd,false,false>(nthreads);
    run<spinlock,false,false>(nthreads);
    run<ticket_spinlock,false,false>(nthreads);

    run<std::mutex,true,true>(nthreads);
    run<terrible_spinlock,true,true>(nthreads);
    run<spinlock_amd,true,true>(nthreads);
    run<spinlock,true,true>(nthreads);
    run<ticket_spinlock,true,true>(nthreads);
    run<std::mutex,false,true>(nthreads);
    run<terrible_spinlock,false,true>(nthreads);
    run<spinlock_amd,false,true>(nthreads);
    run<spinlock,false,true>(nthreads);
    run<ticket_spinlock,false,true>(nthreads);
  }
  return EXIT_SUCCESS;
}
