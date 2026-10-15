#pragma once

// Debug trap for 'map direct memory backing: Bad file descriptor' (APS5_TRAP_BACKING_FD=1, macOS).
// Each direct memory backing descriptor (a temporary file in trap runs) gets a kernel fd guard: any close() or dup2() onto it, from
// any image (MoltenVK, SDL, libc internals, raw syscalls), raises a fatal EXC_GUARD whose system crash
// report (~/Library/Logs/DiagnosticReports/eboot-*.ips, type EXC_GUARD) holds the closer's backtrace.
// No in-process handler: fatal fd guard kills race it, and a task exception port suppresses the report.
namespace DirectMemoryTrap {

bool Enabled();
// Guards a backing descriptor (no-op unless enabled). Returns false if the guard could not be set.
// macOS refuses fd guards on POSIX shared memory, so trap runs back direct memory with an unlinked
// temporary file instead (CreateBackingFile), whose descriptor can be guarded.
bool GuardBackingFd(int fd);
// Creates a guarded, unlinked, sized temporary file for a backing; -1 with errno on failure.
int CreateBackingFile(unsigned long long bytes);
// Closes a descriptor guarded by GuardBackingFd (plain close when it is not guarded).
void CloseBackingFd(int fd);
// Called before mapping a backing: reports and aborts if the descriptor is no longer open.
void CheckBackingFd(int fd, const void* backing, unsigned long long bytes);

}
