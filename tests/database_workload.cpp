#include "fs/block_device.hpp"
#include "fs/crash_consistency.hpp"
#include "fs/disk_format.hpp"
#include "fs/layout_engine.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

using namespace std;

namespace {

using Page = array<byte, fs::kBlockSize>;

class TempImage {
public:
    TempImage() {
        array<char, 40> path{};
        constexpr string_view pattern = "/tmp/filesystem-db-workload-XXXXXX";
        copy(pattern.begin(), pattern.end(), path.begin());
        const int fd = mkstemp(path.data());
        if (fd < 0) {
            throw system_error(errno, generic_category(), "mkstemp database workload image");
        }
        path_ = path.data();
        if (close(fd) != 0) {
            const int error = errno;
            remove(path_.c_str());
            throw system_error(error, generic_category(), "close temporary image");
        }
    }

    ~TempImage() {
        if (!path_.empty()) {
            remove(path_.c_str());
        }
    }

    TempImage(const TempImage&) = delete;
    TempImage& operator=(const TempImage&) = delete;

    const string& path() const noexcept {
        return path_;
    }

private:
    string path_;
};

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

string page_context(string_view operation, uint64_t page_id) {
    return string(operation) + " for page " + to_string(page_id);
}

void write_and_verify(fs::FixedPageLayout& layout, uint64_t page_id, const Page& expected, string_view operation, uint64_t& operations_verified) {
    const string context = page_context(operation, page_id);
    try {
        layout.write_page(page_id, expected);
        ++operations_verified;
    } catch (const exception& error) {
        throw runtime_error(context + " write failed: " + error.what());
    }

    Page actual{};
    try {
        layout.read_page(page_id, actual);
        ++operations_verified;
    } catch (const exception& error) {
        throw runtime_error(context + " readback failed: " + error.what());
    }

    if (actual != expected) {
        throw runtime_error(context + " readback differed from all-byte expected contents");
    }
}

void read_and_verify(const fs::FixedPageLayout& layout, uint64_t page_id, const Page& expected, string_view operation, uint64_t& operations_verified) {
    Page actual{};
    try {
        layout.read_page(page_id, actual);
        ++operations_verified;
    } catch (const exception& error) {
        throw runtime_error(page_context(operation, page_id) + " failed: " + error.what());
    }

    if (actual != expected) {
        throw runtime_error(page_context(operation, page_id) + " differed from all-byte expected contents");
    }
}

template<typename ExpectedException, typename Operation> void expect_exception(Operation&& operation, string_view context) {
    try {
        operation();
    } catch (const ExpectedException&) {
        return;
    } catch (const exception& error) {
        throw runtime_error(string(context) + " threw the wrong exception: " + error.what());
    }
    throw runtime_error(string(context) + " was not rejected");
}

} // namespace

int main() {
    try {
        TempImage image;
        constexpr uint64_t page_count = 32;
        constexpr uint64_t image_size = 16 * 1024 * 1024;
        constexpr uint64_t random_seed = 0x5eed1234ULL;

        vector<Page> expected_pages(page_count);
        vector<uint64_t> versions(page_count, 1);
        uint64_t operations_verified = 0;
        uint64_t layout_inode = 0;

        {
            auto device = fs::BlockDevice::create(image.path(), image_size);
            fs::format_image(device);
            const auto superblock = fs::read_superblock(device);
            auto consistency = fs::CrashConsistency::open_for_update(device);
            auto pages = fs::FixedPageLayout::create(device, superblock, page_count);
            layout_inode = pages.inode_number();

            for (uint64_t page_id = 0; page_id < page_count; ++page_id) {
                expected_pages[page_id] = make_page(page_id, versions[page_id]);
                write_and_verify(pages, page_id, expected_pages[page_id], "sequential write", operations_verified);
            }

            for (uint64_t page_id = 0; page_id < page_count / 4; ++page_id) {
                ++versions[page_id];
                expected_pages[page_id] = make_page(page_id, versions[page_id]);
                write_and_verify(pages, page_id, expected_pages[page_id], "overwrite", operations_verified);
            }

            mt19937_64 random(random_seed);
            constexpr uint64_t random_operations = 160;
            for (uint64_t operation = 0; operation < random_operations; ++operation) {
                const uint64_t page_id = random() % page_count;
                if (operation % 3 == 0) {
                    ++versions[page_id];
                    expected_pages[page_id] = make_page(page_id, versions[page_id]);
                    write_and_verify(pages, page_id, expected_pages[page_id], "random update", operations_verified);
                } else {
                    read_and_verify(pages, page_id, expected_pages[page_id], "random read", operations_verified);
                }
            }

            for (uint64_t page_id = 0; page_id < page_count; ++page_id) {
                read_and_verify(pages, page_id, expected_pages[page_id], "sequential scan", operations_verified);
            }

            Page valid_page{};
            array<byte, fs::kBlockSize - 1> short_page{};

            expect_exception<out_of_range>([&] {
                pages.read_page(page_count, valid_page);
            }, page_context("read invalid page ID", page_count));

            expect_exception<out_of_range>( [&] {
                pages.write_page(page_count, valid_page);
            }, page_context("write invalid page ID", page_count));

            expect_exception<invalid_argument>([&] {
                pages.read_page(0, short_page);
            }, page_context("read with invalid buffer size", 0));

            expect_exception<invalid_argument>([&] {
                pages.write_page(0, short_page);
            }, page_context("write with invalid buffer size", 0));

            consistency.shutdown();
            device.flush();
        }

        {
            auto device = fs::BlockDevice::open(image.path(), fs::BlockDevice::Mode::ReadWrite);
            auto consistency = fs::CrashConsistency::open(device);
            const auto superblock = fs::read_superblock(device);
            auto pages = fs::FixedPageLayout::open(device, superblock, layout_inode);
            if (pages.page_count() != page_count || pages.page_size() != fs::kBlockSize) {
                throw runtime_error("reopened layout geometry did not match the created 4 KiB layout");
            }
            for (uint64_t page_id = 0; page_id < page_count; ++page_id) {
                read_and_verify(pages, page_id, expected_pages[page_id], "reopen persistence scan", operations_verified);
            }
            consistency.shutdown();
            device.flush();
        }

        cout << "database workload passed: " << page_count << " pages, " << operations_verified << " page operations verified (seed " << random_seed << ")\n";
        return EXIT_SUCCESS;
    } catch (const exception& error) {
        cerr << "database workload failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
