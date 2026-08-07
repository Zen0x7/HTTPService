// Minimal <linux/version.h> stub for build environments that lack the Linux
// kernel headers (e.g. the alpine/musl bcompiler images). Boost.Asio reads
// LINUX_VERSION_CODE to decide whether to enable epoll/eventfd/timerfd.
//
// This file is only added to the include path when the real header is not
// available (see CMakeLists.txt), so it never shadows the system one.
#ifndef _LINUX_VERSION_H
#define _LINUX_VERSION_H

#define LINUX_VERSION_CODE 394752 /* 6.6.0 */
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))

#endif /* _LINUX_VERSION_H */
