/**
 * @file unique_fd.hpp
 * @brief Move-only RAII owner for a POSIX file descriptor.
 */

#ifndef PROXCHUNK_UNIQUE_FD_HPP
#define PROXCHUNK_UNIQUE_FD_HPP

#include <unistd.h>

namespace proxchunk {

/**
 * @brief Owns a file descriptor and closes it on destruction.
 *
 * Empty state is @c fd == -1. Copy is deleted; move transfers ownership.
 */
class unique_fd
{
public:
    unique_fd() noexcept = default;

    /**
     * @brief Take ownership of @p fd (may be -1).
     * @param[in] fd Raw descriptor; not dup'd.
     */
    explicit unique_fd(int fd) noexcept
        : fd_(fd)
    {
    }

    ~unique_fd()
    {
        reset();
    }

    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;

    unique_fd(unique_fd&& other) noexcept
        : fd_(other.fd_)
    {
        other.fd_ = -1;
    }

    unique_fd& operator=(unique_fd&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    /**
     * @brief Borrow the raw descriptor.
     * @return fd, or -1 if empty.
     */
    [[nodiscard]] int get() const noexcept
    {
        return fd_;
    }

    /**
     * @brief True when a live descriptor is held.
     */
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return fd_ >= 0;
    }

    /**
     * @brief Relinquish ownership without closing.
     * @return The raw fd, or -1 if empty.
     */
    int release() noexcept
    {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    /**
     * @brief Close the current fd (if any) and optionally take @p fd.
     * @param[in] fd New descriptor, or -1 to become empty.
     */
    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0)
        {
            (void)::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

} // namespace proxchunk

#endif /* PROXCHUNK_UNIQUE_FD_HPP */
