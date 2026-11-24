#include <algorithm>
#include <atomic>
#include <cassert>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>
int main() {
 std::vector<int> v(1000); std::iota(v.begin(),v.end(),1);
 assert(std::accumulate(v.begin(),v.end(),0)==500500);
 std::atomic<int> sum{0}; std::vector<std::thread> threads;
 for(int i=0;i<4;i++) threads.emplace_back([&]{for(int j=0;j<1000;j++) sum.fetch_add(1);});
 for(auto& t:threads)t.join(); assert(sum==4000);
 bool caught=false; try { throw std::runtime_error("fixture"); } catch(const std::exception& e) { caught=std::string(e.what())=="fixture"; }
 assert(caught); std::cout << "CXX PASS 500500 4000 exception\n";
}
