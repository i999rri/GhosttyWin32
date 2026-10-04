#pragma once

// Whether the `wsl` shim a shell would run is the one this build
// shipped.
//
// The shim cannot be run from where it ships. An executable inside an
// installed MSIX cannot be started from its own directory:
// `C:\Program Files\WindowsApps` refuses even a read of its own ACL,
// and CreateProcessW on a file under it answers access denied whatever
// the file's own entry grants. Everything the shim does happens in a
// process a shell starts, so the package is the one place it cannot be.
//
// So it is installed, by the person who wants it, into a directory they
// own -- the same bargain as the bridge's in-distro helper, and the
// same reason: the host cannot put a runnable copy where it is needed.
// This host only reads. It compares what is installed against what it
// ships and either puts that directory on a shell's PATH or leaves the
// PATH alone, which leaves a typed `wsl` running the real wsl.exe.
//
// Bytes, not timestamps. A copy made in Explorer keeps the write time,
// but one downloaded from a release gets the time of the download, so
// the same bytes would read as a different file. Size first, since a
// mismatch there is the common case and costs no read.

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace core::wsl {

// What a shell would find, and whether this host can use it.
enum class ShimState {
    // Nothing installed. The feature was never set up, or the
    // directory was cleaned out.
    Missing,
    // Installed, but not what this build ships -- after an upgrade,
    // most likely. The shim and the host share the wire format and the
    // options they carry, so the two have to come from one build.
    Different,
    // The shim this build ships, where a shell can run it.
    Shipped,
};

// Compares `installed` against `shipped` byte for byte.
inline ShimState CheckInstalledShim(std::filesystem::path const& shipped,
                                    std::filesystem::path const& installed) {
    std::error_code ec;
    const auto shippedSize = std::filesystem::file_size(shipped, ec);
    if (ec) return ShimState::Missing;  // nothing to compare against

    const auto installedSize = std::filesystem::file_size(installed, ec);
    if (ec) return ShimState::Missing;
    if (shippedSize != installedSize) return ShimState::Different;

    std::ifstream a(shipped, std::ios::binary);
    std::ifstream b(installed, std::ios::binary);
    if (!a || !b) return ShimState::Different;

    constexpr std::size_t kChunk = 64 * 1024;
    std::string left(kChunk, '\0');
    std::string right(kChunk, '\0');
    while (a && b) {
        a.read(left.data(), static_cast<std::streamsize>(kChunk));
        b.read(right.data(), static_cast<std::streamsize>(kChunk));
        const auto read = a.gcount();
        if (read != b.gcount()) return ShimState::Different;
        if (read == 0) break;
        if (std::memcmp(left.data(), right.data(), static_cast<std::size_t>(read)) != 0) {
            return ShimState::Different;
        }
    }
    return ShimState::Shipped;
}

}  // namespace core::wsl
