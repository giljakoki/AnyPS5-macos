#include "prx/libc/include/general/VabiMacros.hpp"
#include "SceTypes.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
int APS5_VABI sceNpGetNpId(int user_id, NpId* np_id);
int APS5_VABI sceNpGetState(int user_id, std::uint32_t* state);
int APS5_VABI sceNpHasSignedUp(int user_id, bool* has_signed_up);
int APS5_VABI sceNpGetAccountIdA(int user_id, std::uint64_t* account_id);
int APS5_VABI sceNpGetAccountCountryA(int user_id, void* country_code);
int APS5_VABI sceNpGetOnlineId(int user_id, NpOnlineId* online_id);
}

extern "C" {
int APS5_VABI sceNpCreateRequest(void);
int APS5_VABI sceNpCreateAsyncRequest(const NpCreateAsyncRequestParameter* param);
int APS5_VABI sceNpDeleteRequest(int reqId);
int APS5_VABI sceNpSetTimeout(int reqId, std::int32_t resolveRetry, std::uint32_t resolveTimeout,
    std::uint32_t connTimeout, std::uint32_t sendTimeout, std::uint32_t recvTimeout);
}

namespace {

constexpr int InvalidArgument = static_cast<int>(0x80550003u);
constexpr int SignedOut = static_cast<int>(0x80550006u);
constexpr int RequestNotFound = static_cast<int>(0x80550014u);

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "NpManager: %s\n", message);
        std::abort();
    }
}

void SetSignedIn() {
#ifdef _WIN32
    Require(_putenv_s("APS5_NP_SIGNED_IN", "1") == 0, "cannot set APS5_NP_SIGNED_IN");
#else
    Require(setenv("APS5_NP_SIGNED_IN", "1", 1) == 0, "cannot set APS5_NP_SIGNED_IN");
#endif
}

void CheckRequestTimeouts() {
    Require(sceNpSetTimeout(0x7fffffff, 2, 0, 0, 0, 0) == RequestNotFound, "never created request");
    Require(sceNpSetTimeout(0x7fffffff, 0, 0, 0, 0, 0) == InvalidArgument, "arguments checked before the request");
    const int requestId = sceNpCreateRequest();
    Require(requestId > 0, "sceNpCreateRequest must return a positive request id");
    Require(sceNpSetTimeout(requestId, 2, 1000000, 10000000, 10000000, 10000000) == 0, "minimum timeouts");
    Require(sceNpSetTimeout(requestId, 0, 0, 0, 0, 30000000) == 0, "only the receive timeout");
    Require(sceNpSetTimeout(requestId, 3, 0, 0, 0, 0) == 0, "only the resolve retries");
    Require(sceNpSetTimeout(requestId, 0, 0, 0, 0, 0) == InvalidArgument, "every value left at its default");
    Require(sceNpSetTimeout(0, 2, 0, 0, 0, 0) == InvalidArgument, "request id 0");
    Require(sceNpSetTimeout(-1, 2, 0, 0, 0, 0) == InvalidArgument, "negative request id");
    Require(sceNpSetTimeout(requestId, -1, 0, 0, 0, 30000000) == InvalidArgument, "negative resolve retries");
    Require(sceNpSetTimeout(requestId, 0, 999999, 0, 0, 0) == InvalidArgument, "resolve timeout below 1 s");
    Require(sceNpSetTimeout(requestId, 0, 0, 9999999, 0, 0) == InvalidArgument, "connect timeout below 10 s");
    Require(sceNpSetTimeout(requestId, 0, 0, 0, 9999999, 0) == InvalidArgument, "send timeout below 10 s");
    Require(sceNpSetTimeout(requestId, 0, 0, 0, 0, 1) == InvalidArgument, "receive timeout below 10 s");
    Require(sceNpDeleteRequest(requestId) == 0, "sceNpDeleteRequest failed");
    Require(sceNpSetTimeout(requestId, 2, 0, 0, 0, 0) == RequestNotFound, "deleted request");
    NpCreateAsyncRequestParameter param{};
    const int asyncId = sceNpCreateAsyncRequest(&param);
    Require(asyncId > 0, "sceNpCreateAsyncRequest must return a positive request id");
    Require(sceNpSetTimeout(asyncId, 2, 0, 0, 0, 0) == 0, "async request");
    Require(sceNpDeleteRequest(asyncId) == 0, "sceNpDeleteRequest failed on the async request");
    Require(sceNpSetTimeout(asyncId, 2, 0, 0, 0, 0) == RequestNotFound, "deleted async request");
}

}

int main() {
    CheckRequestTimeouts();
    NpId npId{};
    std::memset(&npId, 0x5a, sizeof(npId));
    NpId untouched{};
    std::memset(&untouched, 0x5a, sizeof(untouched));
    Require(sceNpGetNpId(0x10000, &npId) == SignedOut, "sceNpGetNpId must report the user as signed out");
    Require(std::memcmp(&npId, &untouched, sizeof(npId)) == 0, "sceNpGetNpId must leave the NpId untouched");
    Require(sceNpGetNpId(0x10000, nullptr) == InvalidArgument, "sceNpGetNpId must reject a null NpId");
    std::uint32_t state = 0;
    Require(sceNpGetState(0x10000, &state) == 0 && state == 1, "sceNpGetState must report signed out by default");
    std::uint64_t accountId = 0;
    Require(sceNpGetAccountIdA(0x10000, &accountId) == SignedOut, "sceNpGetAccountIdA must fail while signed out");
    char country[4] = {'x', 'x', 'x', 'x'};
    Require(sceNpGetAccountCountryA(0x10000, country) == SignedOut, "sceNpGetAccountCountryA must fail while signed out");
    NpOnlineId onlineId{};
    Require(sceNpGetOnlineId(0x10000, &onlineId) == SignedOut, "sceNpGetOnlineId must fail while signed out");
    SetSignedIn();
    Require(sceNpGetState(0x10000, &state) == 0 && state == 2, "APS5_NP_SIGNED_IN must report the user as signed in");
    bool signedUp = false;
    Require(sceNpHasSignedUp(0x10000, &signedUp) == 0 && signedUp, "APS5_NP_SIGNED_IN must report a signed-up user");
    Require(sceNpGetAccountIdA(0x10000, &accountId) == 0 && accountId != 0, "APS5_NP_SIGNED_IN must give an account id");
    Require(sceNpGetAccountIdA(0x10000, nullptr) == InvalidArgument, "sceNpGetAccountIdA must reject a null id");
    Require(sceNpGetAccountCountryA(0x10000, country) == 0 && country[0] == 'u' && country[1] == 's' && country[2] == '\0',
        "APS5_NP_SIGNED_IN must give a terminated country code");
    Require(sceNpGetOnlineId(0x10000, &onlineId) == 0 && std::strcmp(onlineId.data, "AnyPS5") == 0,
        "APS5_NP_SIGNED_IN must give an online id");
    Require(sceNpGetNpId(0x10000, &npId) == 0 && std::strcmp(npId.online_id.data, "AnyPS5") == 0,
        "APS5_NP_SIGNED_IN must give an NpId");
    return 0;
}
