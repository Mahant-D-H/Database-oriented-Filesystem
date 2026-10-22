#include "fs/block_device.hpp"
#include "fs/crash_consistency.hpp"
#include "fs/disk_format.hpp"
#include "fs/layout_engine.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/vfs.h>

using namespace std;

namespace {

using Clock = chrono::steady_clock;
using Page = array<byte, fs::kBlockSize>;
constexpr uint64_t kDefaultPageCount = 4096;
constexpr uint64_t kDefaultOperationCount = 1024;
constexpr uint64_t kDefaultSeed = 0x5eed1234ULL;
constexpr uint64_t kDefaultRepetitions = 5;
constexpr uint64_t kImageOverhead = 32 * 1024 * 1024;
constexpr size_t kWarmupLimit = 64;
constexpr string_view kDefaultImageDirectory = "/tmp";

class TempPath {
public:
    TempPath(const string& directory, string_view image_name) {
        string pattern = directory;
        if (!pattern.empty() && pattern.back() != '/') {
            pattern.push_back('/');
        }
        pattern.append(image_name);
        vector<char> path(pattern.begin(), pattern.end());
        path.push_back('\0');
        const int fd = mkstemp(path.data());
        if (fd < 0) {
            throw system_error(errno, generic_category(), "mkstemp benchmark image");
        }
        path_ = path.data();
        if (::close(fd) != 0) {
            const int error = errno;
            remove(path_.c_str());
            path_.clear();
            throw system_error(error, generic_category(), "close benchmark image handle");
        }
    }

    ~TempPath() {
        if (!path_.empty()) {
            remove(path_.c_str());
        }
    }

    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;

    const string& path() const noexcept {
        return path_;
    }

private:
    string path_;
};

struct FilesystemInfo {
    string type;
    bool is_tmpfs;
};

FilesystemInfo filesystem_info(const string& directory) {
    struct stat directory_status {};
    if (stat(directory.c_str(), &directory_status) != 0) {
        throw system_error(errno, generic_category(), "stat benchmark image directory");
    }
    if (!S_ISDIR(directory_status.st_mode)) {
        throw invalid_argument("benchmark image path is not a directory: " + directory);
    }

    struct statfs status {};
    if (statfs(directory.c_str(), &status) != 0) {
        throw system_error(errno, generic_category(), "statfs benchmark image directory");
    }

    switch (static_cast<unsigned long>(status.f_type)) {
    case EXT4_SUPER_MAGIC:
            return {"ext4", false};
    case TMPFS_MAGIC:
            return {"tmpfs", true};
    case XFS_SUPER_MAGIC:
            return {"xfs", false};
    case BTRFS_SUPER_MAGIC:
            return {"btrfs", false};
    case OVERLAYFS_SUPER_MAGIC:
            return {"overlay", false};
    case NFS_SUPER_MAGIC:
            return {"nfs", false};
    default:
        return {"unknown (magic " + to_string(static_cast<unsigned long>(status.f_type)) + ")", false};
    }
}

struct Operation {
    uint64_t page_id;
    bool write;
    uint64_t version;
};

enum class Workload {
    SequentialWrites,
    RandomReads,
    RandomUpdates,
    SequentialScan,
    Mixed,
};

struct WorkloadPlan {
    Workload workload;
    string_view name;
    vector<Operation> warmup;
    vector<Operation> operations;
};

struct Measurement {
    string_view backend;
    string_view workload;
    uint64_t operations;
    long double operation_nanoseconds;
    vector<int64_t> latency_nanoseconds;
};

uint64_t parse_u64(const char* value, const char* name) {
    const string_view text(value);
    uint64_t result = 0;
    const auto [end, error] = from_chars(text.data(), text.data() + text.size(), result);
    if (error != errc{} || end != text.data() + text.size()) {
        throw invalid_argument(string("invalid ") + name + ": " + value);
    }
    return result;
}

Page make_page(uint64_t page_id, uint64_t version) {
    Page page{};
    for (size_t byte = 0; byte < sizeof(page_id); ++byte) {
        page[byte] = static_cast<std::byte>((page_id >> (byte * 8)) & 0xff);
        page[sizeof(page_id) + byte] = static_cast<std::byte>((version >> (byte * 8)) & 0xff);
    }

    uint64_t state = page_id ^ (version * 0x9e3779b97f4a7c15ULL);
    for (size_t index = 2 * sizeof(uint64_t); index < page.size(); ++index) {
        state += 0x9e3779b97f4a7c15ULL;
        uint64_t mixed = state;
        mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;
        mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
        mixed ^= mixed >> 31;
        page[index] = static_cast<byte>(mixed & 0xff);
    }
    return page;
}

void check_page(const Page& actual, uint64_t page_id, uint64_t version, string_view backend, string_view phase) {
    if (actual != make_page(page_id, version)) {
        throw runtime_error(string(backend) + " " + string(phase) + " mismatch at page " + to_string(page_id));
    }
}

class CustomBackend {
public:
    explicit CustomBackend(fs::FixedPageLayout& layout): layout_(layout) {}

    void initialize_page(uint64_t page_id, const Page& page) {
        layout_.write_page(page_id, page);
    }

    void write_page(uint64_t page_id, const Page& page) {
        layout_.write_page(page_id, page);
    }

    void read_page(uint64_t page_id, Page& page) const {
        layout_.read_page(page_id, page);
    }

private:
    fs::FixedPageLayout& layout_;
};

class PosixBackend {
public:
    PosixBackend(const string& path, uint64_t page_count) {
        fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            throw system_error(errno, generic_category(), "open POSIX baseline file");
        }
        const uint64_t bytes = page_count * fs::kBlockSize;
        if (bytes > static_cast<uint64_t>(numeric_limits<off_t>::max()) || ftruncate(fd_, static_cast<off_t>(bytes)) != 0) {
            const int error = errno == 0 ? EOVERFLOW : errno;
            ::close(fd_);
            fd_ = -1;
            throw system_error(error, generic_category(), "resize POSIX baseline file");
        }
    }

    ~PosixBackend() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    PosixBackend(const PosixBackend&) = delete;
    PosixBackend& operator=(const PosixBackend&) = delete;

    void initialize_page(uint64_t page_id, const Page& page) {
        write_all(page_id, page);
    }

    void write_page(uint64_t page_id, const Page& page) {
        write_all(page_id, page);
        if (fdatasync(fd_) != 0) {
            throw system_error(errno, generic_category(), "fdatasync POSIX baseline page");
        }
    }

    void read_page(uint64_t page_id, Page& page) const {
        size_t completed = 0;
        const off_t offset = page_offset(page_id);
        while (completed < page.size()) {
            const ssize_t result = pread(fd_, page.data() + completed, page.size() - completed, offset + static_cast<off_t>(completed));
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw system_error(errno, generic_category(), "pread POSIX baseline page");
            }
            if (result == 0) {
                throw runtime_error("short read from POSIX baseline page " + to_string(page_id));
            }
            completed += static_cast<size_t>(result);
        }
    }

    void finish_initialization() {
        if (fdatasync(fd_) != 0) {
            throw system_error(errno, generic_category(), "fdatasync POSIX baseline initialization");
        }
    }

private:
    static off_t page_offset(uint64_t page_id) {
        return static_cast<off_t>(page_id * fs::kBlockSize);
    }

    void write_all(uint64_t page_id, const Page& page) {
        size_t completed = 0;
        const off_t offset = page_offset(page_id);
        while (completed < page.size()) {
            const ssize_t result = pwrite(fd_, page.data() + completed, page.size() - completed, offset + static_cast<off_t>(completed));
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw system_error(errno, generic_category(), "pwrite POSIX baseline page");
            }
            if (result == 0) {
                throw runtime_error("short write to POSIX baseline page " + to_string(page_id));
            }
            completed += static_cast<size_t>(result);
        }
    }
    int fd_ = -1;
};

string_view workload_name(Workload workload) {
    switch (workload) {
    case Workload::SequentialWrites:
            return "sequential page writes";
    case Workload::RandomReads:
            return "random page reads";
    case Workload::RandomUpdates:
            return "random page updates";
    case Workload::SequentialScan:
            return "sequential scan";
    case Workload::Mixed:
            return "mixed (80/20 read/write probability)";
    }

    throw logic_error("unknown benchmark workload");
}

vector<Operation> make_plan(Workload workload, uint64_t operation_count, uint64_t page_count, uint64_t seed, vector<uint64_t>& versions) {
    uint64_t count = operation_count;
    if (workload == Workload::SequentialScan) {
        const uint64_t passes = max<uint64_t>(1, 1 + (operation_count - 1) / page_count);
        if (passes > numeric_limits<uint64_t>::max() / page_count) {
            throw overflow_error("sequential scan operation count overflows");
        }
        count = passes * page_count;
    }
    if (count > vector<Operation>().max_size()) {
        throw length_error("requested operation count is too large");
    }

    mt19937_64 random(seed);
    vector<Operation> plan;
    plan.reserve(static_cast<size_t>(count));
    for (uint64_t index = 0; index < count; ++index) {
        uint64_t page_id = 0;
        bool write = false;

        switch (workload) {
        case Workload::SequentialWrites:
            page_id = index % page_count;
            write = true;
            break;
        case Workload::RandomReads:
            page_id = random() % page_count;
            break;
        case Workload::RandomUpdates:
            page_id = random() % page_count;
            write = true;
            break;
        case Workload::SequentialScan:
            page_id = index % page_count;
            break;
        case Workload::Mixed:
            page_id = random() % page_count;
            write = random() % 5 == 0;
            break;
        }

        uint64_t version = versions[page_id];
        if (write) {
            version = ++versions[page_id];
        }
        plan.push_back({page_id, write, version});
    }
    return plan;
}

void initialize_pages(CustomBackend& backend, uint64_t page_count) {
    for (uint64_t page_id = 0; page_id < page_count; ++page_id) {
        backend.initialize_page(page_id, make_page(page_id, 0));
    }
}

void initialize_pages(PosixBackend& backend, uint64_t page_count) {
    for (uint64_t page_id = 0; page_id < page_count; ++page_id) {
        backend.initialize_page(page_id, make_page(page_id, 0));
    }
    backend.finish_initialization();
}

void reset_pages(CustomBackend& backend, const vector<bool>& modified_pages) {
    for (uint64_t page_id = 0; page_id < modified_pages.size(); ++page_id) {
        if (modified_pages[page_id]) {
            backend.initialize_page(page_id, make_page(page_id, 0));
        }
    }
}

void reset_pages(PosixBackend& backend, const vector<bool>& modified_pages) {
    for (uint64_t page_id = 0; page_id < modified_pages.size(); ++page_id) {
        if (modified_pages[page_id]) {
            backend.initialize_page(page_id, make_page(page_id, 0));
        }
    }
    backend.finish_initialization();
}

template<typename Backend> void run_warmup(Backend& backend, const vector<Operation>& operations, string_view backend_name) {
    Page page{};
    for (const Operation& operation : operations) {
        if (operation.write) {
            backend.write_page(operation.page_id, make_page(operation.page_id, operation.version));
        } else {
            backend.read_page(operation.page_id, page);
            check_page(page, operation.page_id, operation.version, backend_name, "warm-up read");
        }
    }
}

template<typename Backend> Measurement run_timed(Backend& backend, string_view backend_name, string_view name, const vector<Operation>& operations) {
    Measurement measurement{backend_name, name, operations.size(), 0, {}};
    measurement.latency_nanoseconds.reserve(operations.size());
    Page page{};
    for (const Operation& operation : operations) {
        const Page payload = operation.write ? make_page(operation.page_id, operation.version) : Page{};
        const auto start = Clock::now();
        if (operation.write) {
            backend.write_page(operation.page_id, payload);
        } else {
            backend.read_page(operation.page_id, page);
        }
        const auto finish = Clock::now();
        const auto elapsed = chrono::duration_cast<chrono::nanoseconds>(finish - start).count();
        measurement.latency_nanoseconds.push_back(elapsed);
        measurement.operation_nanoseconds += chrono::duration<long double, nano>(finish - start).count();

        if (!operation.write) {
            check_page(page, operation.page_id, operation.version, backend_name, "timed read");
        }
    }
    return measurement;
}

void verify_final_pages(CustomBackend& backend, const vector<uint64_t>& versions) {
    Page page{};
    for (uint64_t page_id = 0; page_id < versions.size(); ++page_id) {
        backend.read_page(page_id, page);
        check_page(page, page_id, versions[page_id], "filesystem", "final verification");
    }
}

void verify_final_pages(PosixBackend& backend, const vector<uint64_t>& versions) {
    Page page{};
    for (uint64_t page_id = 0; page_id < versions.size(); ++page_id) {
        backend.read_page(page_id, page);
        check_page(page, page_id, versions[page_id], "POSIX baseline", "final verification");
    }
}

void verify_reference_state(CustomBackend& custom, PosixBackend& posix, const vector<uint64_t>& reference_versions, string_view phase) {
    Page page{};
    for (uint64_t page_id = 0; page_id < reference_versions.size(); ++page_id) {
        custom.read_page(page_id, page);
        check_page(page, page_id, reference_versions[page_id], "filesystem", phase);
    }
    for (uint64_t page_id = 0; page_id < reference_versions.size(); ++page_id) {
        posix.read_page(page_id, page);
        check_page(page, page_id, reference_versions[page_id], "POSIX baseline", phase);
    }
}

int64_t percentile(vector<int64_t> values, double percentile_value) {
    sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(ceil(percentile_value * values.size())) - 1;
    return values[index];
}

double operations_per_second(const Measurement& measurement) {
    const double seconds = static_cast<double>(measurement.operation_nanoseconds / 1'000'000'000.0L);
    return static_cast<double>(measurement.operations) / seconds;
}

double mebibytes_per_second(const Measurement& measurement) {
    const double seconds = static_cast<double>(measurement.operation_nanoseconds / 1'000'000'000.0L);
    const double mebibytes = static_cast<double>(measurement.operations) * fs::kBlockSize / (1024.0 * 1024.0);
    return mebibytes / seconds;
}

void print_measurement(const Measurement& measurement, string_view result_kind, uint64_t repetition = 0) {
    const auto micros = [](int64_t nanoseconds) {
        return static_cast<double>(nanoseconds) / 1000.0;
    };

    cout << result_kind;
    if (result_kind == "run") cout << ' ' << repetition;
    cout << " | " << measurement.backend
         << " | " << measurement.workload
         << " | ops=" << measurement.operations
         << " | " << fixed << setprecision(2) << operations_per_second(measurement) << " ops/s"
         << " | " << mebibytes_per_second(measurement) << " MiB/s"
         << " | p50/p95/p99=" << micros(percentile(measurement.latency_nanoseconds, 0.50)) << "/"
                              << micros(percentile(measurement.latency_nanoseconds, 0.95)) << "/"
                              << micros(percentile(measurement.latency_nanoseconds, 0.99)) << " nano-s\n";
}

double median(vector<double> values) {
    sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
}

void print_aggregate(const vector<Measurement>& measurements) {
    vector<double> ops_rates;
    vector<double> throughput_rates;
    vector<double> p50_latencies;
    vector<double> p95_latencies;
    vector<double> p99_latencies;
    ops_rates.reserve(measurements.size());
    throughput_rates.reserve(measurements.size());
    p50_latencies.reserve(measurements.size());
    p95_latencies.reserve(measurements.size());
    p99_latencies.reserve(measurements.size());
    for (const Measurement& measurement : measurements) {
        ops_rates.push_back(operations_per_second(measurement));
        throughput_rates.push_back(mebibytes_per_second(measurement));
        p50_latencies.push_back(static_cast<double>(percentile(measurement.latency_nanoseconds, 0.50)) / 1000.0);
        p95_latencies.push_back(static_cast<double>(percentile(measurement.latency_nanoseconds, 0.95)) / 1000.0);
        p99_latencies.push_back(static_cast<double>(percentile(measurement.latency_nanoseconds, 0.99)) / 1000.0);
    }
    const Measurement& first = measurements.front();
    cout << "median"
         << " | " << first.backend
         << " | " << first.workload
         << " | ops/run=" << first.operations
         << " | " << fixed << setprecision(2) << median(move(ops_rates)) << " ops/s"
         << " | " << median(move(throughput_rates)) << " MiB/s"
         << " | median(p50/p95/p99)=" << median(move(p50_latencies)) << "/"
                                      << median(move(p95_latencies)) << "/"
                                      << median(move(p99_latencies)) << " us\n";
}

void print_usage(const char* executable) {
    cout << "usage: " << executable << " [page-count (default 4096)] [operation-count (default 1024)] [random-seed (default 1592594996)] [image-directory (default /tmp)] [repetitions (default 5)]\n"
         << "Sequential scan performs enough full working-set passes to cover at least operation-count page reads.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc > 1 && string_view(argv[1]) == "--help") {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (argc > 6) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        const uint64_t page_count = argc > 1 ? parse_u64(argv[1], "page count") : kDefaultPageCount;
        const uint64_t operation_count = argc > 2 ? parse_u64(argv[2], "operation count") : kDefaultOperationCount;
        const uint64_t seed = argc > 3 ? parse_u64(argv[3], "random seed") : kDefaultSeed;
        const string image_directory = argc > 4 ? argv[4] : string(kDefaultImageDirectory);
        const uint64_t repetitions = argc > 5 ? parse_u64(argv[5], "repetition count") : kDefaultRepetitions;
        if (page_count == 0 || operation_count == 0) {
            throw invalid_argument("page count and operation count must be greater than zero");
        }
        if (repetitions == 0 || repetitions > vector<uint64_t>().max_size()) {
            throw invalid_argument("repetition count must be a supported positive value");
        }
        if (image_directory.empty()) {
            throw invalid_argument("benchmark image directory must not be empty");
        }
        if (page_count > vector<uint64_t>().max_size()) {
            throw length_error("requested page count is too large");
        }
        if (page_count > (static_cast<uint64_t>(numeric_limits<off_t>::max()) - kImageOverhead) / fs::kBlockSize) {
            throw overflow_error("requested page count exceeds supported image size");
        }
        const uint64_t image_size = page_count * fs::kBlockSize + kImageOverhead;
        const FilesystemInfo backing_filesystem = filesystem_info(image_directory);

        TempPath filesystem_image(image_directory, "database-benchmark-fs-XXXXXX");
        TempPath posix_image(image_directory, "database-benchmark-posix-XXXXXX");
        auto device = fs::BlockDevice::create(filesystem_image.path(), image_size);
        fs::format_image(device);
        const auto superblock = fs::read_superblock(device);
        auto consistency = fs::CrashConsistency::open_for_update(device);
        auto layout = fs::FixedPageLayout::create(device, superblock, page_count);
        CustomBackend custom(layout);
        PosixBackend posix(posix_image.path(), page_count);

        vector<uint64_t> reference_versions(page_count, 0);
        initialize_pages(custom, page_count);
        initialize_pages(posix, page_count);
        verify_reference_state(custom, posix, reference_versions, "initialization verification");

        array<array<vector<Measurement>, 2>, 5> measurements;
        for (auto& workload_measurements : measurements) {
            for (auto& backend_measurements : workload_measurements) {
                backend_measurements.reserve(static_cast<size_t>(repetitions));
            }
        }
        const array<Workload, 5> workloads{
            Workload::SequentialWrites,
            Workload::RandomReads,
            Workload::RandomUpdates,
            Workload::SequentialScan,
            Workload::Mixed,
        };

        for (uint64_t repetition = 0; repetition < repetitions; ++repetition) {
            for (size_t workload_index = 0; workload_index < workloads.size(); ++workload_index) {
                const Workload workload = workloads[workload_index];
                verify_reference_state(custom, posix, reference_versions, "pre-workload repetition verification");
                vector<uint64_t> warmup_versions = reference_versions;
                const uint64_t warmup_seed = seed ^ (0x9e3779b97f4a7c15ULL + workload_index);
                auto warmup_plan = make_plan(workload, kWarmupLimit, page_count, warmup_seed, warmup_versions);
                if (workload == Workload::SequentialScan) {
                    warmup_plan.resize(min<size_t>(kWarmupLimit, warmup_plan.size()));
                }
                run_warmup(posix, warmup_plan, "POSIX baseline");
                run_warmup(custom, warmup_plan, "filesystem");
                verify_reference_state(custom, posix, warmup_versions, "post-warm-up verification");

                const uint64_t timed_seed = seed ^ (0xd1b54a32d192ed03ULL + workload_index);
                vector<uint64_t> workload_expected_versions = warmup_versions;
                auto operations = make_plan(workload, operation_count, page_count, timed_seed, workload_expected_versions);
                const Measurement posix_measurement = run_timed(posix, "POSIX baseline", workload_name(workload), operations);
                const Measurement custom_measurement = run_timed(custom, "filesystem", workload_name(workload), operations);
                verify_reference_state(custom, posix, workload_expected_versions, "post-workload verification");

                vector<bool> modified_pages(page_count, false);
                for (const Operation& operation : warmup_plan) {
                    if (operation.write) {
                        modified_pages[operation.page_id] = true;
                    }
                }
                for (const Operation& operation : operations) {
                    if (operation.write) {
                        modified_pages[operation.page_id] = true;
                    }
                }
                reset_pages(posix, modified_pages);
                reset_pages(custom, modified_pages);
                verify_reference_state(custom, posix, reference_versions, "post-reset verification");
                measurements[workload_index][0].push_back(move(custom_measurement));
                measurements[workload_index][1].push_back(move(posix_measurement));
            }
        }

        verify_final_pages(custom, reference_versions);
        verify_final_pages(posix, reference_versions);
        consistency.shutdown();
        device.flush();

        cout << "database storage benchmark (correctness verified; not a database-engine benchmark)\n"
             << "pages=" << page_count << " page_bytes=" << page_count * fs::kBlockSize << " operations=" << operation_count << " repetitions=" << repetitions << " seed=" << seed << '\n'
             << "image_directory=" << image_directory << " filesystem_type=" << backing_filesystem.type << '\n';
        if (backing_filesystem.is_tmpfs) {
            cout << "warning: benchmark images are on tmpfs; results do not represent persistent-disk performance.\n";
        }
        cout << "POSIX baseline: buffered pread/pwrite; reads use the OS page cache; each timed write includes fdatasync, while initialization is synced once. It has no filesystem WAL.\n"
             << "Custom filesystem: FixedPageLayout over BlockDevice (O_DIRECT|O_SYNC where supported), each page write uses its WAL/double-write commit path.\n"
             << "The backends have different caching and durability paths; this is not a perfectly equivalent comparison and does not predict SQLite/PostgreSQL performance.\n"
             << "POSIX runs first and the custom filesystem second in every repetition; this order can introduce its own execution-order bias. Repeated runs do not eliminate cache-state differences between O_DIRECT and buffered POSIX I/O.\n"
             << "MiB/s reports logical page bytes per operation, not physical write amplification.\n"
             << "Initialization, reset, and warm-up are excluded. Throughput uses operation count divided by the sum of measured API-call durations; plan generation and read verification are excluded. Percentiles use nearest-rank samples from individual calls.\n"
             << "Correctness verification is completed before this report is printed.\n"
             << "\nVerified benchmark measurements:\n";

        cout << "Individual repetition measurements:\n";
        for (size_t workload_index = 0; workload_index < workloads.size(); ++workload_index) {
            for (uint64_t repetition = 0; repetition < repetitions; ++repetition) {
                print_measurement(measurements[workload_index][0][repetition], "run", repetition + 1);
                print_measurement(measurements[workload_index][1][repetition], "run", repetition + 1);
            }
        }
        cout << "Aggregate median across repetitions:\n";
        for (const auto& workload_measurements : measurements) {
            print_aggregate(workload_measurements[0]);
            print_aggregate(workload_measurements[1]);
        }
        return EXIT_SUCCESS;
    } catch (const exception& error) {
        cerr << "database benchmark failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
