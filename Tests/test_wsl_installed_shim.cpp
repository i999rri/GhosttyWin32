#include "pch.h"
#include "../Core/Wsl/InstalledShim.h"

#include <string>

using core::wsl::CheckInstalledShim;
using core::wsl::ShimState;

namespace {

// A directory of this test's own, removed when it goes out of scope.
class TempDir {
public:
    TempDir() {
        m_path = std::filesystem::temp_directory_path() /
                 ("ghostty-shim-" + std::to_string(::GetCurrentProcessId()) +
                  "-" + std::to_string(++s_counter));
        std::filesystem::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    TempDir(TempDir const&) = delete;
    TempDir& operator=(TempDir const&) = delete;

    std::filesystem::path File(std::wstring_view name) const { return m_path / name; }

private:
    std::filesystem::path m_path;
    static int s_counter;
};

int TempDir::s_counter = 0;

void Write(std::filesystem::path const& at, std::string const& bytes) {
    std::ofstream out(at, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Big enough to cross the comparison's chunk boundary, so a difference
// in the last chunk is a difference the loop has to keep looking for.
std::string Bytes(char fill, size_t size = 150 * 1024) {
    return std::string(size, fill);
}

}  // namespace

TEST(WslInstalledShimTest, TakesACopyOfWhatIsShipped) {
    TempDir dir;
    const auto shipped = dir.File(L"shipped.exe");
    const auto installed = dir.File(L"installed.exe");
    Write(shipped, Bytes('s'));
    Write(installed, Bytes('s'));

    EXPECT_EQ(CheckInstalledShim(shipped, installed), ShimState::Shipped);
}

TEST(WslInstalledShimTest, IgnoresWhenTheWriteTimesDiffer) {
    // The reason bytes are compared rather than metadata: a shim
    // downloaded from a release carries the time of the download, and
    // the same bytes must still be recognised.
    TempDir dir;
    const auto shipped = dir.File(L"shipped.exe");
    const auto installed = dir.File(L"installed.exe");
    Write(shipped, Bytes('s'));
    Write(installed, Bytes('s'));
    std::filesystem::last_write_time(
        installed, std::filesystem::last_write_time(installed) + std::chrono::hours(72));

    EXPECT_EQ(CheckInstalledShim(shipped, installed), ShimState::Shipped);
}

TEST(WslInstalledShimTest, SaysDifferentForAnotherBuild) {
    TempDir dir;
    const auto shipped = dir.File(L"shipped.exe");
    const auto installed = dir.File(L"installed.exe");

    // A different length is the cheap half: no read at all.
    Write(shipped, Bytes('s'));
    Write(installed, Bytes('s', 150 * 1024 + 1));
    EXPECT_EQ(CheckInstalledShim(shipped, installed), ShimState::Different);

    // The same length with different contents has to be read to tell,
    // and the difference is placed in the last chunk on purpose.
    auto other = Bytes('s');
    other.back() = 'x';
    Write(installed, other);
    EXPECT_EQ(CheckInstalledShim(shipped, installed), ShimState::Different);
}

TEST(WslInstalledShimTest, SaysMissingWithNothingInstalled) {
    TempDir dir;
    const auto shipped = dir.File(L"shipped.exe");
    Write(shipped, Bytes('s'));

    EXPECT_EQ(CheckInstalledShim(shipped, dir.File(L"absent.exe")), ShimState::Missing);
}

TEST(WslInstalledShimTest, SaysMissingWithNothingToCompareAgainst) {
    // No shipped shim means this build cannot vouch for anything, so
    // the PATH is left alone rather than pointed at an unknown file.
    TempDir dir;
    const auto installed = dir.File(L"installed.exe");
    Write(installed, Bytes('s'));

    EXPECT_EQ(CheckInstalledShim(dir.File(L"absent.exe"), installed), ShimState::Missing);
}
