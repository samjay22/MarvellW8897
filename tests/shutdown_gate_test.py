"""Stress the driver's actual shutdown/submission gate with mock thread calls."""
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
    'bool isShuttingDown(', 'void beginShutdown(', 'void scheduleCall(',
    'void scheduleDelayedCall('))
prefix = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
struct IOSimpleLock { std::mutex m; };
void IOSimpleLockLock(IOSimpleLock* p) {p->m.lock();}
void IOSimpleLockUnlock(IOSimpleLock* p) {p->m.unlock();}
struct Context {IOSimpleLock* schedulingLock{}; uint32_t shuttingDown{};};
using thread_call_t=Context*;
std::atomic<unsigned> calls{0}; std::atomic<unsigned> afterShutdown{0};
void thread_call_enter(thread_call_t host) {
 if (__atomic_load_n(&host->shuttingDown, __ATOMIC_ACQUIRE)) ++afterShutdown;
 ++calls;
}
void thread_call_enter_delayed(thread_call_t host,uint64_t) {thread_call_enter(host);}
'''
main = r'''
int main() {
 for (int iteration=0; iteration<100; ++iteration) {
  IOSimpleLock lock; Context host; host.schedulingLock=&lock;
  std::atomic<bool> start{false}; std::vector<std::thread> workers;
  for(int i=0;i<8;++i) workers.emplace_back([&,i]{
   while(!start.load(std::memory_order_acquire)) {}
   for(int n=0;n<2000;++n) {
    if(i&1) scheduleCall(&host,&host); else scheduleDelayedCall(&host,&host,1);
   }
  });
  start.store(true,std::memory_order_release);
  for(int spin=0;spin<100;++spin) std::this_thread::yield();
  beginShutdown(&host);
  for(auto& worker:workers)worker.join();
  auto before=calls.load();
  scheduleCall(&host,&host); scheduleDelayedCall(&host,&host,1);
  assert(calls.load()==before && isShuttingDown(&host));
 }
 assert(afterShutdown.load()==0);
 std::printf("100 shutdown races with 8 concurrent producers passed; %u pre-shutdown submissions.\n", calls.load());
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory)
    (path / 'test.cpp').write_text(prefix + impl + main)
    subprocess.run(['clang++', '-std=c++17', '-O1', '-pthread',
                    '-fsanitize=address,undefined', str(path / 'test.cpp'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
