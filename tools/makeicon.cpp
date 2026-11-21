// Writes Wolfe's icon, as drawn by the program itself, to an .ico file:
//   wolfe-makeicon <path.ico>
// res/wolfe.ico is made with it; the executable carries that file, for
// Explorer, shortcuts and the list of installed applications.

#include "gui/shell.h"

#include <cstdio>
#include <vector>

namespace {

void put16(std::vector<unsigned char>& out, unsigned value) {
    out.push_back(value & 0xFF);
    out.push_back((value >> 8) & 0xFF);
}

void put32(std::vector<unsigned char>& out, unsigned value) {
    put16(out, value & 0xFFFF);
    put16(out, value >> 16);
}

// One image of an .ico: a 32-bit bitmap from the bottom row up, twice as tall
// as it is wide (the second half is the 1-bit mask, unused when there is alpha).
std::vector<unsigned char> image(int size) {
    const std::vector<uint32_t> pixels = wolfe::iconPixels(size);
    const int maskRow = ((size + 31) / 32) * 4;
    std::vector<unsigned char> out;
    put32(out, 40);
    put32(out, size);
    put32(out, size * 2);
    put16(out, 1);
    put16(out, 32);
    put32(out, 0);  // BI_RGB
    put32(out, size * size * 4 + maskRow * size);
    put32(out, 0);
    put32(out, 0);
    put32(out, 0);
    put32(out, 0);
    for (int row = size - 1; row >= 0; row--) {
        for (int column = 0; column < size; column++) put32(out, pixels[row * size + column]);
    }
    out.insert(out.end(), (size_t)maskRow * size, 0);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: wolfe-makeicon <path.ico>\n");
        return 2;
    }
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 256};
    const int count = sizeof(sizes) / sizeof(sizes[0]);
    std::vector<std::vector<unsigned char>> images;
    for (int size : sizes) images.push_back(image(size));

    std::vector<unsigned char> out;
    put16(out, 0);
    put16(out, 1);  // an icon
    put16(out, count);
    unsigned offset = 6 + 16 * count;
    for (int i = 0; i < count; i++) {
        out.push_back(sizes[i] >= 256 ? 0 : sizes[i]);
        out.push_back(sizes[i] >= 256 ? 0 : sizes[i]);
        out.push_back(0);
        out.push_back(0);
        put16(out, 1);
        put16(out, 32);
        put32(out, (unsigned)images[i].size());
        put32(out, offset);
        offset += (unsigned)images[i].size();
    }
    for (const auto& data : images) out.insert(out.end(), data.begin(), data.end());

    FILE* file = fopen(argv[1], "wb");
    if (!file) {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    fwrite(out.data(), 1, out.size(), file);
    fclose(file);
    return 0;
}
