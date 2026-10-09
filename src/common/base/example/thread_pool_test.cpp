#include <algorithm>
#include <cstdio>
#include <iostream>
#include "common/base/tlss_thread_pool.h"
using namespace std;
void printString(const std::string& str) {
  cout << str << endl;
  usleep(100 * 1000);
}
void test(int max_s) {
  cout << "queue size = " << max_s << endl;
  TLSS::BASE::ThreadPool pool("TestPool");
  pool.set_max_queue_size(max_s);
  pool.start(5);

  cout << "adding" << endl;
  pool.run([]() {
    printf("i am pool task ");
  });

  for (int i = 0; i < 100; ++i) {
    char buf[32];
    snprintf(buf, sizeof buf, "task %d", i);
    pool.run(std::bind(printString, std::string(buf)));
  }
  cout << "Done" << endl;
  pool.stop();
}

int main() {
  test(0);
  test(1);
  test(5);
  test(10);
}
