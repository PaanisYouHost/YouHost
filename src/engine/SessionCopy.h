#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
 #include <sys/clonefile.h>
#endif

#if defined(__linux__)
 #include <linux/fs.h>
 #include <sys/ioctl.h>
 #include <sys/sendfile.h>
#endif

namespace youhost
{

struct FileCopyResult
{
    bool ok = false;
    std::uint64_t bytes = 0;
    bool cloned = false;
};

// Fast path used by Save As. macOS clonefile is a copy-on-write clone on the
// same APFS volume. Linux tries FICLONE on the same device, then sendfile,
// then a large read/write. No sleep and no per-file fsync.
inline FileCopyResult copyFileFast(const std::string& source,
                                    const std::string& destination,
                                    const std::function<void(std::uint64_t bytesCopied)>& progress = {})
{
    const int input = ::open(source.c_str(), O_RDONLY);
    if (input < 0)
        return {};

    ::unlink(destination.c_str());

#if defined(__APPLE__)
    if (::clonefile(source.c_str(), destination.c_str(), 0) == 0)
    {
        struct stat clonedStat;
        std::uint64_t bytes = 0;
        if (::fstat(input, &clonedStat) == 0 && clonedStat.st_size > 0)
            bytes = static_cast<std::uint64_t>(clonedStat.st_size);
        ::close(input);
        if (progress)
            progress(bytes);
        return { true, bytes, true };
    }
#endif

    const int output = ::open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output < 0)
    {
        ::close(input);
        return {};
    }

    struct stat sourceStat;
    if (::fstat(input, &sourceStat) != 0 || sourceStat.st_size < 0)
    {
        ::close(input);
        ::close(output);
        return {};
    }
    const auto bytes = static_cast<std::uint64_t>(sourceStat.st_size);

#if defined(__linux__)
    struct stat destStat;
    if (::fstat(output, &destStat) == 0 && sourceStat.st_dev == destStat.st_dev)
    {
        if (::ioctl(output, FICLONE, input) == 0)
        {
            ::close(input);
            ::close(output);
            if (progress)
                progress(bytes);
            return { true, bytes, true };
        }
    }

    std::uint64_t sent = 0;
    bool sendFailed = false;
    while (sent < bytes)
    {
        const auto chunk = bytes - sent > 1024u * 1024u ? 1024u * 1024u : bytes - sent;
        const auto n = ::sendfile(output, input, nullptr, static_cast<size_t>(chunk));
        if (n < 0)
        {
            sendFailed = true;
            break;
        }
        if (n == 0)
            break;
        sent += static_cast<std::uint64_t>(n);
        if (progress)
            progress(sent);
    }
    if (! sendFailed && sent == bytes)
    {
        ::close(input);
        ::close(output);
        return { true, bytes, false };
    }
    if (::lseek(input, 0, SEEK_SET) < 0 || ::lseek(output, 0, SEEK_SET) < 0 || ::ftruncate(output, 0) != 0)
    {
        ::close(input);
        ::close(output);
        return {};
    }
#endif

    std::vector<char> buffer(1024u * 1024u);
    std::uint64_t copied = 0;
    for (;;)
    {
        const auto n = ::read(input, buffer.data(), buffer.size());
        if (n < 0)
        {
            ::close(input);
            ::close(output);
            return {};
        }
        if (n == 0)
            break;
        std::size_t written = 0;
        const auto count = static_cast<std::size_t>(n);
        while (written < count)
        {
            const auto w = ::write(output, buffer.data() + written, count - written);
            if (w < 0)
            {
                ::close(input);
                ::close(output);
                return {};
            }
            written += static_cast<std::size_t>(w);
        }
        copied += static_cast<std::uint64_t>(n);
        if (progress)
            progress(copied);
    }
    ::close(input);
    ::close(output);
    return { copied == bytes, copied, false };
}

// The previous Save As copy: 256 KB chunks, a 1 ms pause every four chunks, and fsync per file.
inline FileCopyResult copyFileSlow(const std::string& source, const std::string& destination)
{
    const int input = ::open(source.c_str(), O_RDONLY);
    if (input < 0)
        return {};
    ::unlink(destination.c_str());
    const int output = ::open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output < 0)
    {
        ::close(input);
        return {};
    }

    std::vector<char> buffer(256u * 1024u);
    std::uint64_t copied = 0;
    int chunks = 0;
    for (;;)
    {
        const auto n = ::read(input, buffer.data(), buffer.size());
        if (n < 0)
        {
            ::close(input);
            ::close(output);
            return {};
        }
        if (n == 0)
            break;
        std::size_t written = 0;
        const auto count = static_cast<std::size_t>(n);
        while (written < count)
        {
            const auto w = ::write(output, buffer.data() + written, count - written);
            if (w < 0)
            {
                ::close(input);
                ::close(output);
                return {};
            }
            written += static_cast<std::size_t>(w);
        }
        copied += static_cast<std::uint64_t>(n);
        if (++chunks >= 4)
        {
            chunks = 0;
            ::usleep(1000);
        }
    }
    ::fsync(output);
    ::close(input);
    ::close(output);
    return { true, copied, false };
}

inline void fsyncDirectory(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return;
    ::fsync(fd);
    ::close(fd);
}

} // namespace youhost
