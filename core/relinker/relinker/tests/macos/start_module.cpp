struct StartBlock {
    unsigned long long magic;
    int value;
};

static const unsigned long long startMagic = 0x5452415453354150ull;
static const unsigned long long* volatile expectedMagic = &startMagic;

extern "C" int module_start(unsigned long args, const void* argp) {
    if (args != sizeof(StartBlock) || argp == nullptr) return 3;
    const auto* block = static_cast<const StartBlock*>(argp);
    return block->magic == *expectedMagic ? block->value : 4;
}
