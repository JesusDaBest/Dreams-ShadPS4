#include <array>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>
#include <set>
#include <mutex>
#include <cassert>
#include "core/libraries/random/random.h"
int main(int argc,char**) {
 using namespace Libraries::Random;
 std::array<u8,64> a{},b{};
 if(argc>1){ assert(sceRandomGetRandomNumber(a.data(),a.size())==0);for(auto x:a)printf("%02x",x);puts("");return 0; }
 assert(sceRandomGetRandomNumber(nullptr,0)==0);
 a.fill(0xA5);assert(sceRandomGetRandomNumber(a.data(),65)!=0);for(auto x:a)assert(x==0xA5);
 assert(sceRandomGetRandomNumber(nullptr,1)!=0);
 for(std::size_t n=1;n<=64;++n){std::array<u8,66> guard{};guard.fill(0xA5);assert(sceRandomGetRandomNumber(guard.data()+1,n)==0);assert(guard[0]==0xA5);for(std::size_t j=n+1;j<66;++j)assert(guard[j]==0xA5);}
 std::srand(1);assert(sceRandomGetRandomNumber(a.data(),64)==0);
 std::srand(1);assert(sceRandomGetRandomNumber(b.data(),64)==0);
 printf("Identical after host srand reset: %s\n",a==b?"YES":"NO");
 std::mutex m;std::set<std::array<u8,64>> samples;
 std::vector<std::thread> threads;
 for(int t=0;t<8;++t)threads.emplace_back([&]{for(int i=0;i<128;++i){std::array<u8,64> x{};assert(sceRandomGetRandomNumber(x.data(),64)==0);std::scoped_lock lock(m);samples.insert(x);}});
 for(auto& t:threads)t.join();printf("Unique concurrent samples: %zu/1024\n",samples.size());
}
