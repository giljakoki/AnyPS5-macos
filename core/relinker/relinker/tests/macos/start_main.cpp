extern "C" int sceKernelLoadStartModule(const char* path, unsigned long args, const void* argp, unsigned flags, const void* option, int* result);
extern "C" [[noreturn]] void exit(int);

struct StartBlock {
    unsigned long long magic;
    int value;
};

int (*volatile loadStartModule)(const char*, unsigned long, const void*, unsigned, const void*, int*) = sceKernelLoadStartModule;

extern "C" [[noreturn]] void _start(void*) {
    StartBlock block{0x5452415453354150ull, 47};
    int result = -1;
    const int handle = loadStartModule("/app0/Plugins/libstart.prx", sizeof(block), &block, 0, nullptr, &result);
    exit(handle > 0 ? result : 2);
}
