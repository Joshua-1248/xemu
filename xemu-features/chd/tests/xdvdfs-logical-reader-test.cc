// SPDX-License-Identifier: GPL-2.0-or-later
#include "xemu-features/disc-modding/xdvdfs.hh"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace xemu_disc_modding;

namespace {
constexpr size_t kSector = 2048;

static void le16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
}
static void le32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}

class MemoryReader final : public LogicalDiscReader {
public:
    explicit MemoryReader(const std::vector<uint8_t> &bytes) : bytes_(bytes) {}
    uint64_t Size() const override { return bytes_.size(); }
    bool ReadAt(uint64_t offset, void *dst, size_t len,
                std::string *error) override {
        if (offset > bytes_.size() || len > bytes_.size() - offset) {
            if (error) *error = "out of bounds";
            return false;
        }
        std::memcpy(dst, bytes_.data() + offset, len);
        return true;
    }
private:
    const std::vector<uint8_t> &bytes_;
};

static std::vector<uint8_t> MakeImage() {
    std::vector<uint8_t> image(35 * kSector, 0);
    const char magic[] = "MICROSOFT*XBOX*MEDIA";
    uint8_t *vd = image.data() + 32 * kSector;
    std::memcpy(vd, magic, sizeof(magic) - 1);
    std::memcpy(vd + kSector - (sizeof(magic) - 1), magic, sizeof(magic) - 1);
    le32(vd + 0x14, 33);          // root directory sector, relative to base
    le32(vd + 0x18, kSector);     // one-sector directory table

    uint8_t *dir = image.data() + 33 * kSector;
    const char name[] = "hello.bin";
    le16(dir + 0, 0);             // no left child
    le16(dir + 2, 0);             // no right child
    le32(dir + 4, 34);            // file sector
    le32(dir + 8, 5);             // file size
    dir[12] = 0;                  // normal file
    dir[13] = sizeof(name) - 1;
    std::memcpy(dir + 14, name, sizeof(name) - 1);
    std::memcpy(image.data() + 34 * kSector, "hello", 5);
    return image;
}
}

int main(int argc, char **argv) {
    assert(argc == 2);
    std::vector<uint8_t> image = MakeImage();

    MemoryReader memory(image);
    Snapshot from_memory;
    std::string error;
    assert(ParseXdvdfsReader(memory, "memory.xiso", &from_memory, &error));
    assert(from_memory.valid);
    assert(from_memory.entries.size() == 2);
    assert(from_memory.entries[1].path == "hello.bin");
    assert(from_memory.entries[1].start_sector == 34);
    assert(from_memory.entries[1].size == 5);

    const char *path = argv[1];
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(image.data()), image.size());
        assert(out.good());
    }
    Snapshot from_file;
    error.clear();
    assert(ParseXdvdfsImage(path, &from_file, &error));
    assert(from_file.valid);
    assert(from_file.entries.size() == from_memory.entries.size());
    assert(from_file.entries[1].path == from_memory.entries[1].path);
    assert(from_file.entries[1].start_sector == from_memory.entries[1].start_sector);
    assert(from_file.entries[1].size == from_memory.entries[1].size);

    std::cout << "PASS: mounted logical reader and raw-file reader parse identically\n";
    return 0;
}
