// Storage identity (volume, filesystem, bus type) and a NON-destructive, read-only sequential read
// benchmark. Nothing in this file writes to, locks or modifies any disk or file.
#include <algorithm>
#include <chrono>
#include <vector>

#include "hw.hpp"
#include "win_util.hpp"

#ifdef _WIN32
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif
#endif

namespace hcs::hardware {

#ifdef _WIN32
namespace {

std::string trim_ascii(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Queries bus type / seek penalty of the device behind a volume root like "E:\". Opening with
// access 0 only reads metadata; no data access and no admin rights are required.
void query_device(const std::wstring& root, json& out) {
    out["bus_type"] = "unknown";
    out["removable_media"] = unknown();
    out["rotational"] = unknown();
    out["device_vendor"] = unknown();
    out["device_product"] = unknown();
    if (root.size() < 2 || root[1] != L':')
        return;  // only drive-letter volumes
    const std::wstring dev = std::wstring(L"\\\\.\\") + root.substr(0, 2);
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;

    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    std::vector<char> buf(1024);
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf.data(),
                        static_cast<DWORD>(buf.size()), &got, nullptr) &&
        got >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const auto* d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
        out["bus_type"] = bus_type_name(static_cast<std::uint32_t>(d->BusType));
        out["removable_media"] = d->RemovableMedia != FALSE;
        auto str_at = [&](DWORD off) -> json {
            if (off == 0 || off >= got)
                return unknown();
            const char* p = buf.data() + off;
            const size_t max = got - off;
            size_t n = 0;
            while (n < max && p[n] != '\0') ++n;
            std::string s = trim_ascii(std::string(p, n));
            return s.empty() ? unknown() : json(s);
        };
        out["device_vendor"] = str_at(d->VendorIdOffset);
        out["device_product"] = str_at(d->ProductIdOffset);  // model string only; serial is never read
    }

    STORAGE_PROPERTY_QUERY q2{};
    q2.PropertyId = StorageDeviceSeekPenaltyProperty;
    q2.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR sp{};
    got = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q2, sizeof(q2), &sp, sizeof(sp), &got, nullptr) &&
        got >= sizeof(sp))
        out["rotational"] = sp.IncursSeekPenalty != FALSE;
    CloseHandle(h);
}

json describe_root(const std::wstring& root) {
    json j;
    j["root"] = win::narrow(root);
    const UINT dt = GetDriveTypeW(root.c_str());
    j["drive_type"] = drive_type_name(dt);
    j["filesystem"] = unknown();
    j["total_bytes"] = unknown();
    j["free_bytes"] = unknown();
    if (dt == DRIVE_NO_ROOT_DIR || dt == DRIVE_UNKNOWN) {
        j["bus_type"] = unknown();
        return j;
    }
    if (dt == DRIVE_CDROM || dt == DRIVE_REMOTE) {
        j["bus_type"] = unknown();
    }
    wchar_t fs[MAX_PATH + 1]{};
    DWORD serial = 0, maxc = 0, flags = 0;
    if (GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, &maxc, &flags, fs, MAX_PATH))
        j["filesystem"] = win::narrow(fs);  // volume label and serial are deliberately not reported
    ULARGE_INTEGER avail{}, total{}, freeb{};
    if (GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &freeb)) {
        j["total_bytes"] = static_cast<std::uint64_t>(total.QuadPart);
        j["free_bytes"] = static_cast<std::uint64_t>(freeb.QuadPart);
    }
    if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE || dt == DRIVE_RAMDISK) {
        json dev;
        query_device(root, dev);
        for (auto it = dev.begin(); it != dev.end(); ++it) j[it.key()] = it.value();
    }
    return j;
}

}  // namespace
#endif

#if !defined(_WIN32)
namespace {

#if defined(__linux__)
std::string trim_ascii(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string read_small_file(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::string line;
    if (in && std::getline(in, line))
        return trim_ascii(line);
    return {};
}

struct MountEntry {
    std::string mount_point, fs_type, source;
};

// Mount points in mountinfo escape space, tab, newline and backslash as octal (\040 \011 \012 \134).
std::string unescape_mount(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            o.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 3).c_str(), nullptr, 8)));
            i += 3;
        } else {
            o.push_back(s[i]);
        }
    }
    return o;
}

// "<id> <parent> <maj:min> <root> <mount point> <opts> [optional...] - <fstype> <source> <super opts>"
std::vector<MountEntry> read_mounts() {
    std::vector<MountEntry> v;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        const auto dash = line.find(" - ");
        if (dash == std::string::npos)
            continue;
        std::istringstream pre(line.substr(0, dash)), post(line.substr(dash + 3));
        std::string id, parent, mm, root, mp;
        pre >> id >> parent >> mm >> root >> mp;
        MountEntry e;
        post >> e.fs_type >> e.source;
        e.mount_point = unescape_mount(mp);
        if (!e.mount_point.empty())
            v.push_back(std::move(e));
    }
    return v;
}

// Longest mount point that is a path prefix of `path` (whole components, not substrings).
const MountEntry* mount_for(const std::vector<MountEntry>& mounts, const std::string& path) {
    const MountEntry* best = nullptr;
    for (const auto& m : mounts) {
        const std::string& mp = m.mount_point;
        const bool prefix = path.compare(0, mp.size(), mp) == 0 &&
                            (mp == "/" || path.size() == mp.size() || path[mp.size()] == '/');
        if (prefix && (!best || mp.size() >= best->mount_point.size()))
            best = &m;
    }
    return best;
}

bool is_host_share(const std::string& fs_type) {
    static const char* const kShare[] = {"drvfs", "9p", "nfs", "nfs4", "cifs", "virtiofs"};
    for (const char* t : kShare)
        if (fs_type == t)
            return true;
    return false;
}

bool is_data_filesystem(const MountEntry& m) {
    if (m.source.rfind("/dev/", 0) == 0)
        return m.fs_type != "squashfs";
    return is_host_share(m.fs_type) || m.fs_type == "fuseblk" || m.fs_type == "ntfs3" || m.fs_type == "exfat";
}

// Block-device facts from sysfs for st_dev of a path (read-only; nothing is opened under /dev).
void query_block_device(dev_t dev, json& out) {
    namespace fsys = std::filesystem;
    if (major(dev) == 0)
        return;  // anonymous device (network / virtual filesystem): no block device behind it
    std::error_code ec;
    const fsys::path link =
        fsys::path("/sys/dev/block") / (std::to_string(major(dev)) + ":" + std::to_string(minor(dev)));
    const fsys::path real = fsys::canonical(link, ec);
    if (ec)
        return;
    fsys::path disk = real;
    if (fsys::exists(real / "partition", ec))
        disk = real.parent_path();  // a partition: queue/ and device/ live on the whole disk
    const std::string rs = disk.string();
    if (rs.find("/usb") != std::string::npos)
        out["bus_type"] = "USB";
    else if (rs.find("/nvme") != std::string::npos)
        out["bus_type"] = "NVMe";
    else if (rs.find("/virtio") != std::string::npos)
        out["bus_type"] = "Virtual";
    else if (rs.find("/ata") != std::string::npos)
        out["bus_type"] = "SATA/ATA";
    else if (rs.find("/mmc") != std::string::npos)
        out["bus_type"] = "SD/MMC";
    const std::string rot = read_small_file(disk / "queue" / "rotational");
    if (rot == "0" || rot == "1")
        out["rotational"] = rot == "1";
    const std::string rem = read_small_file(disk / "removable");
    if (rem == "0" || rem == "1")
        out["removable_media"] = rem == "1";
    if (const auto v = read_small_file(disk / "device" / "vendor"); !v.empty())
        out["device_vendor"] = v;
    if (const auto m = read_small_file(disk / "device" / "model"); !m.empty())
        out["device_product"] = m;  // model string only; serial is never read
}
#endif

json describe_path(const std::string& path) {
    json j;
    j["root"] = path;
    j["drive_type"] = unknown();
    j["filesystem"] = unknown();
    j["total_bytes"] = unknown();
    j["free_bytes"] = unknown();
    j["bus_type"] = unknown();
    j["removable_media"] = unknown();
    j["rotational"] = unknown();
    j["device_vendor"] = unknown();
    j["device_product"] = unknown();
    struct statvfs sv{};
    if (::statvfs(path.c_str(), &sv) == 0) {
        j["total_bytes"] = static_cast<std::uint64_t>(sv.f_blocks) * sv.f_frsize;
        j["free_bytes"] = static_cast<std::uint64_t>(sv.f_bavail) * sv.f_frsize;
    }
#if defined(__linux__)
    std::error_code ec;
    const std::string canon = std::filesystem::canonical(path, ec).string();
    if (!ec) {
        const auto mounts = read_mounts();
        if (const MountEntry* m = mount_for(mounts, canon)) {
            j["filesystem"] = m->fs_type;
            j["mount_point"] = m->mount_point;
            j["drive_type"] = is_host_share(m->fs_type) ? "network/host-share" : "fixed";
        }
    }
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) {
        query_block_device(st.st_dev, j);
        if (j["removable_media"] == true)
            j["drive_type"] = "removable";
    }
#endif
    return j;
}

}  // namespace
#endif

json storage_report(const std::string& storage_root, bool list_volumes) {
    json r;
    r["requested_path"] = storage_root.empty() ? unknown() : json(storage_root);
    r["requested_volume"] = unknown();
    r["volumes"] = json::array();
#ifdef _WIN32
    const UINT old_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
    if (!storage_root.empty()) {
        std::wstring w = win::widen(storage_root);
        wchar_t full[MAX_PATH * 2]{};
        const DWORD n = GetFullPathNameW(w.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
        if (n == 0 || n >= std::size(full) || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
            r["requested_volume"] = {{"error", "path does not exist or cannot be resolved"}};
        } else {
            wchar_t root[MAX_PATH + 1]{};
            if (GetVolumePathNameW(full, root, MAX_PATH))
                r["requested_volume"] = describe_root(root);
            else
                r["requested_volume"] = {{"error", "volume of path could not be determined"}};
        }
    }
    if (list_volumes) {
        const DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i)))
                continue;
            const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
            r["volumes"].push_back(describe_root(root));
        }
    }
    SetErrorMode(old_mode);
#else
    if (!storage_root.empty()) {
        std::error_code ec;
        if (!std::filesystem::exists(storage_root, ec))
            r["requested_volume"] = {{"error", "path does not exist or cannot be resolved"}};
        else
            r["requested_volume"] = describe_path(storage_root);
    }
#if defined(__linux__)
    if (list_volumes) {
        for (const auto& m : read_mounts())
            if (is_data_filesystem(m))
                r["volumes"].push_back(describe_path(m.mount_point));
    }
#else
    (void)list_volumes;
#endif
#endif
    return r;
}

json read_benchmark(const std::string& file, const ReadBenchOptions& opt) {
    json r;
    r["file"] = file;
    r["mode"] = "sequential, read-only, bounded buffer";
    r["buffer_bytes"] = opt.buffer_bytes;
    r["max_bytes_per_sample"] = opt.max_bytes;
    r["cold_or_warm"] = "unknown";  // Windows file cache state is not controlled or measured
    r["samples"] = json::array();
    r["mib_s_median"] = unknown();
    r["mib_s_min"] = unknown();
    r["mib_s_max"] = unknown();
#ifndef _WIN32
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        r["status"] = "error";
        r["error"] = "cannot open file for reading (errno " + std::to_string(errno) + ")";
        return r;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        ::close(fd);
        r["status"] = "error";
        r["error"] = "not a readable, non-empty regular file";
        return r;
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);
    const std::uint64_t target = std::min<std::uint64_t>(file_size, opt.max_bytes);
    r["file_size_bytes"] = file_size;
#if defined(__linux__)
    (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#elif defined(__APPLE__)
    (void)::fcntl(fd, F_RDAHEAD, 1);
#endif
    std::vector<char> buf(opt.buffer_bytes);
    std::vector<double> rates;
    for (std::uint32_t s = 0; s < opt.repeats; ++s) {
        std::uint64_t total = 0;
        const auto t0 = std::chrono::steady_clock::now();
        bool failed = false;
        int fail_errno = 0;
        while (total < target) {
            const std::size_t want =
                static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), target - total));
            const ssize_t got = ::pread(fd, buf.data(), want, static_cast<off_t>(total));
            if (got < 0) {
                if (errno == EINTR)
                    continue;
                failed = true;
                fail_errno = errno;
                break;
            }
            if (got == 0)
                break;
            total += static_cast<std::uint64_t>(got);
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        json sample = {{"bytes_read", total}, {"elapsed_ms", secs * 1000.0}};
        if (failed) {
            sample["error"] = "read failed (errno " + std::to_string(fail_errno) + ")";
        } else if (secs > 0 && total > 0) {
            const double mibs = (static_cast<double>(total) / (1024.0 * 1024.0)) / secs;
            sample["mib_s"] = mibs;
            rates.push_back(mibs);
        }
        r["samples"].push_back(std::move(sample));
        if (failed)
            break;
    }
    ::close(fd);
    if (rates.empty()) {
        r["status"] = "error";
        r["error"] = "no successful sample";
        return r;
    }
    std::vector<double> sorted = rates;
    std::sort(sorted.begin(), sorted.end());
    const size_t n = sorted.size();
    r["mib_s_median"] = n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    r["mib_s_min"] = sorted.front();
    r["mib_s_max"] = sorted.back();
    r["status"] = "ok";
    r["note"] =
        "The OS page cache can inflate repeated samples; cold/warm state is not controlled (unknown).";
    return r;
#else
    const std::wstring w = win::widen(file);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        r["status"] = "error";
        r["error"] = "cannot open file for reading (win32 error " + std::to_string(GetLastError()) + ")";
        return r;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        CloseHandle(h);
        r["status"] = "error";
        r["error"] = "not a readable, non-empty regular file";
        return r;
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(size.QuadPart);
    const std::uint64_t target = std::min<std::uint64_t>(file_size, opt.max_bytes);
    r["file_size_bytes"] = file_size;
    std::vector<char> buf(opt.buffer_bytes);
    std::vector<double> rates;
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    for (std::uint32_t s = 0; s < opt.repeats; ++s) {
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN))
            break;
        std::uint64_t total = 0;
        LARGE_INTEGER t0{}, t1{};
        QueryPerformanceCounter(&t0);
        bool failed = false;
        while (total < target) {
            const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(buf.size(), target - total));
            DWORD got = 0;
            if (!ReadFile(h, buf.data(), want, &got, nullptr)) {
                failed = true;
                break;
            }
            if (got == 0)
                break;
            total += got;
        }
        QueryPerformanceCounter(&t1);
        const double secs =
            static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart);
        json sample = {{"bytes_read", total}, {"elapsed_ms", secs * 1000.0}};
        if (failed) {
            sample["error"] = "read failed (win32 error " + std::to_string(GetLastError()) + ")";
        } else if (secs > 0 && total > 0) {
            const double mibs = (static_cast<double>(total) / (1024.0 * 1024.0)) / secs;
            sample["mib_s"] = mibs;
            rates.push_back(mibs);
        }
        r["samples"].push_back(std::move(sample));
        if (failed)
            break;
    }
    CloseHandle(h);
    if (rates.empty()) {
        r["status"] = "error";
        r["error"] = "no successful sample";
        return r;
    }
    std::vector<double> sorted = rates;
    std::sort(sorted.begin(), sorted.end());
    const size_t n = sorted.size();
    r["mib_s_median"] = n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    r["mib_s_min"] = sorted.front();
    r["mib_s_max"] = sorted.back();
    r["status"] = "ok";
    r["note"] =
        "Windows file caching can inflate repeated samples; cold/warm state is not controlled (unknown).";
    return r;
#endif
}

}  // namespace hcs::hardware
