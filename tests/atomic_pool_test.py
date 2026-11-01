import pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
s=(root/'Contents/MacOS/MarvellW8897MlanHost.cpp').read_text()
def function(signature):
    start=s.index(signature); begin=s.index('{',start); depth=0
    for pos in range(begin,len(s)):
        if s[pos]=='{': depth+=1
        elif s[pos]=='}':
            depth-=1
            if not depth: return s[start:pos+1]
    raise ValueError(signature)
start=s.index('struct Allocation {'); end=s.index('\n};',start)+3
impl=s[start:end]+'\n'+s[s.index('constexpr uint64_t kAllocationMagic'):s.index('bool initializeAtomicPool')]
impl+='\n'+'\n'.join(function(x) for x in ['bool initializeAtomicPool(', 'bool destroyAtomicPool(', 'void* allocateAtomic(', 'void* allocate(', 'void deallocate('])
prefix='''#include <cassert>\n#include <cstdlib>\n#include <cstring>\n#include <cstdint>\n#include <cstdarg>\n#include <mutex>\n#include <thread>\n#include <vector>\nusing t_u32=uint32_t; using t_u64=uint64_t;\nthread_local int spinDepth=0;\nstruct IOSimpleLock {std::mutex mutex;};\nIOSimpleLock* IOSimpleLockAlloc(){assert(!spinDepth); return new IOSimpleLock;}\nvoid IOSimpleLockFree(IOSimpleLock* p){assert(!spinDepth); delete p;}\nvoid IOSimpleLockLock(IOSimpleLock* p){p->mutex.lock(); ++spinDepth;}\nvoid IOSimpleLockUnlock(IOSimpleLock* p){--spinDepth; p->mutex.unlock();}\nvoid* IOMalloc(size_t n){assert(!spinDepth);return malloc(n);}\nvoid IOFree(void* p,size_t){assert(!spinDepth);free(p);}\nvoid IOLog(const char*,...){}\nstruct Allocation;\nstruct Context {IOSimpleLock* atomicPoolLock{}; Allocation* atomicFree{}; t_u32 atomicPoolCount{}; t_u32 atomicPoolAvailable{}; t_u64 atomicPoolFailures{};};\n'''
main='''\nint main(){ Context c; assert(initializeAtomicPool(&c)); assert(c.atomicPoolAvailable==256); std::vector<void*> items; for(unsigned i=0;i<256;i++){ ++spinDepth; void* p=allocateAtomic(&c, 16384); --spinDepth; assert(p); for(int j=0;j<8;j++) assert(((unsigned char*)p)[j]==0); memset(p,0xa5,16384); items.push_back(p); } assert(!allocateAtomic(&c,1)); assert(!allocateAtomic(&c,16385)); assert(c.atomicPoolFailures==2); for(auto p:items){++spinDepth;deallocate(p);--spinDepth;} assert(c.atomicPoolAvailable==256); std::vector<std::thread> threads; for(int i=0;i<8;i++) threads.emplace_back([&]{for(int n=0;n<10000;n++){void* p=allocateAtomic(&c,256);assert(p);memset(p,1,256);deallocate(p);}}); for(auto& t:threads)t.join(); assert(c.atomicPoolAvailable==256); void* p=allocate(123); assert(p); deallocate(p); assert(destroyAtomicPool(&c));}\n'''
with tempfile.TemporaryDirectory() as d:
    src=pathlib.Path(d)/'test.cpp'; exe=pathlib.Path(d)/'test'; src.write_text(prefix+impl+main)
    subprocess.run(['clang++','-std=c++17','-O1','-pthread','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Atomic pool: lock-context, capacity, oversize, reuse, and 80,000 concurrent operations passed under ASan/UBSan.')
