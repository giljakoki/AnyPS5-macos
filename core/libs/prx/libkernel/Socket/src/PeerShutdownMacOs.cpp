#ifdef __APPLE__

#include <sys/event.h>
#include <unistd.h>
#include <cerrno>
#include <ctime>
#include <stdexcept>
#include <string>

// Apart from the guest's kqueue constants (Equeue.hpp): <sys/event.h> defines macros of the same names,
// and EVFILT_USER differs. EV_EOF on a read filter is set once the peer shuts down its sending side, also
// while unread data remains, as Linux's POLLRDHUP and the console's kqueue report it.
extern "C" bool SocketPeerShutDown_nid_no_patch(int native) {
    const int queue = ::kqueue();
    if (queue < 0) throw std::runtime_error("equeue poll: kqueue failed, host errno " + std::to_string(errno));
    struct kevent change;
    EV_SET(&change, native, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, nullptr);
    struct kevent event{};
    const timespec now{0, 0};
    const int count = ::kevent(queue, &change, 1, &event, 1, &now);
    const int error = count < 0 ? errno : (count == 1 && (event.flags & EV_ERROR) != 0 ? static_cast<int>(event.data) : 0);
    ::close(queue);
    if (error != 0) throw std::runtime_error("equeue poll: kevent failed, host errno " + std::to_string(error));
    return count == 1 && (event.flags & EV_EOF) != 0;
}

#endif
