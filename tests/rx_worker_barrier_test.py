"""Exercise the real RX worker wrapper and flush barrier with host-side mocks."""
import os
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
    'bool isShuttingDown(', 'bool waitForRxWorker(', 'void rxCallFired('))
prefix = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
using thread_t=void*; using thread_call_param_t=void*;
thread_local int threadMarker;
thread_t current_thread() {return &threadMarker;}
struct IOLock {std::mutex m; std::condition_variable cv;};
void IOLockLock(IOLock* p){p->m.lock();}
void IOLockUnlock(IOLock* p){p->m.unlock();}
void IOLockSleep(IOLock* p,void*,unsigned){std::unique_lock<std::mutex> l(p->m,std::adopt_lock);p->cv.wait(l);l.release();}
void IOLockWakeup(IOLock* p,void*,bool){p->cv.notify_all();}
constexpr unsigned THREAD_UNINT=0; constexpr int RX_DATA=0;
struct Context {IOLock* rxWorkLock{};thread_t rxWorker{};void* adapter{};uint32_t shuttingDown{};uint64_t rxWorkerRuns{};};
bool waitForRxWorker(Context* host);
std::atomic<bool> entered{false}, allowExit{false}, selfAvoided{false};
void mlan_process_pcie_interrupt_cb(void* value,int) {
 auto* host=static_cast<Context*>(value);
 selfAvoided.store(!waitForRxWorker(host),std::memory_order_release);
 entered.store(true,std::memory_order_release);
 while(!allowExit.load(std::memory_order_acquire))std::this_thread::yield();
}
'''
main = r'''
int main() {
 IOLock lock; Context host; host.rxWorkLock=&lock; host.adapter=&host;
 assert(waitForRxWorker(&host));
 std::thread worker([&]{rxCallFired(&host,nullptr);});
 while(!entered.load(std::memory_order_acquire))std::this_thread::yield();
 assert(selfAvoided.load(std::memory_order_acquire));
 std::atomic<unsigned> ready{0}, finished{0}; std::vector<std::thread> waiters;
 for(int i=0;i<8;++i)waiters.emplace_back([&]{++ready;assert(waitForRxWorker(&host));++finished;});
 while(ready.load()!=8)std::this_thread::yield();
 std::this_thread::sleep_for(std::chrono::milliseconds(20));
 assert(finished.load()==0);
 allowExit.store(true,std::memory_order_release);
 worker.join(); for(auto& waiter:waiters)waiter.join(); assert(finished.load()==8);
 assert(host.rxWorkerRuns == 1);
 assert(waitForRxWorker(&host));
 __atomic_store_n(&host.shuttingDown,1U,__ATOMIC_RELEASE);
 entered.store(false); rxCallFired(&host,nullptr); assert(!entered.load()); assert(host.rxWorkerRuns == 2);
 std::puts("RX barrier: active-worker wait, eight waiters, self-wait guard, and shutdown passed.");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory)
    (path / 'test.cpp').write_text(prefix + impl + main)
    subprocess.run(['clang++', '-std=c++17', '-O1', '-pthread',
                    '-fsanitize=' + os.environ.get('MARVELL_SANITIZER', 'address,undefined'),
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
