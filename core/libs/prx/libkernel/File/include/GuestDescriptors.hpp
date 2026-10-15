#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_GUESTDESCRIPTORS_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_GUESTDESCRIPTORS_HPP

namespace File {

// Guest and host descriptors share one number space, but on a PS5 the guest's
// kernel descriptors are separate. Track the numbers handed to the guest so a
// double close or a stray number can never close or use a host descriptor
// (direct-memory backing, graphics driver files) that happens to hold it.
// Disabled on Windows and with APS5_NO_GUEST_FD_TABLE=1.
bool GuestDescriptorTableEnabled();
int GuestDescriptorFloor();               // first number the rotation hands out
void AdoptGuestDescriptor(int fd);        // fd was just given to the guest
bool ReleaseGuestDescriptor(int fd);      // false if the guest did not own fd
bool GuestDescriptorRejected(int fd);     // true if the guest must get EBADF for fd

}

#endif
