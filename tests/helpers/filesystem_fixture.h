#pragma once

#include "temp_dir.h"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

namespace backup::testing {

inline bool create_unix_socket(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) return false;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string name = path.string();
    if (name.size() >= sizeof(address.sun_path)) {
        ::close(descriptor);
        errno = ENAMETOOLONG;
        return false;
    }
    std::strncpy(address.sun_path, name.c_str(), sizeof(address.sun_path) - 1);
    ::unlink(address.sun_path);
    const bool bound = ::bind(
        descriptor,
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address)) == 0;
    ::close(descriptor);
    return bound;
}

inline bool create_device(const std::filesystem::path& path,
                          bool block_device,
                          unsigned int major_number,
                          unsigned int minor_number) {
    std::filesystem::create_directories(path.parent_path());
    const mode_t type = block_device ? S_IFBLK : S_IFCHR;
    return ::mknod(path.c_str(), type | 0600,
                   makedev(major_number, minor_number)) == 0;
}

inline bool set_file_times(const std::filesystem::path& path,
                           const timespec& access_time,
                           const timespec& modified_time) {
    const timespec times[] = {access_time, modified_time};
    return ::utimensat(AT_FDCWD, path.c_str(), times, 0) == 0;
}

}  // namespace backup::testing
