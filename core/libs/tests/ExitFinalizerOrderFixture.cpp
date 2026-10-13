#include <cstdlib>

namespace {

void (*finalizer)() = nullptr;

void RunFinalizer() {
    if (finalizer != nullptr) finalizer();
}

struct Registration {
    Registration() {
        if (std::atexit(RunFinalizer) != 0) std::abort();
    }
};

const Registration registration;

}

extern "C" void ExitFinalizerOrderFixtureSet(void (*callback)()) {
    finalizer = callback;
}
