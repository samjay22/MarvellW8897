"""Exercise the actual DMA-map list helpers with user-space lock stand-ins."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'Contents/MacOS/MarvellW8897MlanHost.cpp').read_text()

def function(signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth = 0
    for pos in range(begin, len(source)):
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
            if not depth:
                return source[start:pos + 1]
    raise ValueError(signature)

impl = '\n'.join(function(s) for s in (
    'unsigned mappingBucket(', 'void addMapping(', 'DmaMapping* takeMapping('))
prefix = r'''
#include <cassert>
#include <cstdint>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstdio>
using t_u8 = uint8_t; using t_u32 = uint32_t; using t_u64 = uint64_t;
struct IOLock { std::mutex m; };
void IOLockLock(IOLock* lock) { lock->m.lock(); }
void IOLockUnlock(IOLock* lock) { lock->m.unlock(); }
struct DmaMapping { DmaMapping* next{}; DmaMapping* previous{};
 DmaMapping* bucketNext{}; t_u8* caller{}; t_u64 address{};
 t_u32 size{}; bool owned{}; };
struct Context { IOLock* mappingLock{}; DmaMapping* mappings{};
 DmaMapping* mappingBuckets[256]{}; };
'''
main = r'''
int main() {
 IOLock lock; Context host; host.mappingLock = &lock;
 constexpr int count = 512; std::vector<DmaMapping> nodes(count);
 for (int i=0; i<count; i++) {
   nodes[i].address = 0x1000000ULL + i*4096ULL;
   nodes[i].caller = reinterpret_cast<t_u8*>(static_cast<uintptr_t>(i+1));
   nodes[i].size = 4096; nodes[i].owned = (i&1);
   addMapping(&host, &nodes[i]);
 }
 // Wrong ownership, caller, length, and missing addresses must not unlink.
 assert(!takeMapping(&host,nodes[0].caller,nodes[0].address,4096,true));
 assert(!takeMapping(&host,nullptr,nodes[0].address,4096,false));
 assert(!takeMapping(&host,nodes[0].caller,nodes[0].address,1,false));
 assert(!takeMapping(&host,nodes[0].caller,0,4096,false));
 unsigned long long total=0, longest=0;
 for (auto& node : nodes) {
   unsigned n=0; auto* p=host.mappingBuckets[mappingBucket(node.address)];
   while (p && p!=&node) {++n; p=p->bucketNext;} assert(p); ++n;
   total+=n; if(n>longest)longest=n;
 }
 std::vector<int> order(count); for(int i=0;i<count;i++)order[i]=i;
 std::mt19937 rng(27); std::shuffle(order.begin(),order.end(),rng);
 for(auto i:order) assert(takeMapping(&host,nodes[i].caller,nodes[i].address,
                                     nodes[i].size,nodes[i].owned)==&nodes[i]);
 assert(!host.mappings); for(auto* p:host.mappingBuckets)assert(!p);
 // A repeated address must still respect caller, size, and ownership, and
 // removing the middle of a bucket must preserve both linked structures.
 DmaMapping same[3];
 for(int i=0;i<3;i++){same[i].address=0x1200000; same[i].size=4096;
   same[i].caller=reinterpret_cast<t_u8*>(static_cast<uintptr_t>(i+1));
   same[i].owned=true; addMapping(&host,&same[i]);}
 assert(takeMapping(&host,same[1].caller,same[1].address,4096,true)==&same[1]);
 assert(takeMapping(&host,same[0].caller,same[0].address,4096,true)==&same[0]);
 assert(takeMapping(&host,same[2].caller,same[2].address,4096,true)==&same[2]);
 assert(!host.mappings); for(auto* p:host.mappingBuckets)assert(!p);
 std::vector<std::thread> threads;
 for(int t=0;t<8;t++)threads.emplace_back([&,t]{
   for(int i=0;i<10000;i++) { DmaMapping v;
     v.address=0x1000000ULL+4096ULL*(t*2+(i&1));
     v.caller=reinterpret_cast<t_u8*>(static_cast<uintptr_t>(t+1));
     v.size=1024; v.owned=true; addMapping(&host,&v);
     assert(takeMapping(&host,v.caller,v.address,v.size,v.owned)==&v);
   }
 });
 for(auto& thread:threads)thread.join();
 assert(!host.mappings); for(auto* p:host.mappingBuckets)assert(!p);
 std::printf("512 page-aligned mappings: average %.2f bucket comparisons, max %llu; old linked list average 256.50.\n", double(total)/count, longest);
 std::puts("Collision, removal, wrong-key, and 80,000 concurrent add/remove checks passed.");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory)
    (path / 'test.cpp').write_text(prefix + impl + main)
    subprocess.run(['clang++', '-std=c++17', '-O1', '-pthread',
                    '-fsanitize=address,undefined', str(path / 'test.cpp'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
