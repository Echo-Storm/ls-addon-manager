// Recordings of the frames an addon receives (.lsrec): lossless, for bug reports that can be replayed offline, tests on real footage and
// tuning. CPU only: the codec and the file; addon/recorder.h takes the frames off the GPU.
//
// The codec is QOI's (the "Quite OK Image" format, its specification is public domain), written here from the specification: runs, a
// 64-entry table of recent pixels, and small differences to the pixel before, on 4-byte units. Any frame format is lossless with it: 8-bit
// and 10-bit frames are one unit a pixel, half-float ones two.
//
// The file: a 128-byte header, then per frame a 32-byte header and its compressed bytes. One size and format a file.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nr::lsrec {

// Compresses rows of 4-byte units (unitsPerRow each, `pitch` bytes apart) and appends to `out`. Decompress writes exactly `units` units.
void Compress(const uint8_t* data, uint32_t unitsPerRow, uint32_t rows, uint32_t pitch, std::vector<uint8_t>& out);
bool Decompress(const uint8_t* data, size_t bytes, uint8_t* out, size_t units);

enum Content : uint32_t { kOwnEncoding = 0, kSdrView = 1, kLight = 2 };   // (older files have 0 there; 2 since 0.9.10)
enum Source : uint32_t { kCaptured = 1, kPresented = 2, kNisInput = 3 };   // LSFG's captured frame, the presented frame, NIS's input
enum Codec : uint32_t { kRaw = 0, kQoi = 1 };

#pragma pack(push, 1)
struct FileHeader {
    char magic[8] = { 'L', 'S', 'R', 'E', 'C', '0', '1', 0 };
    uint32_t headerBytes = sizeof(FileHeader);
    uint32_t version = 1;
    uint32_t width = 0, height = 0;
    uint32_t format = 0;          // DXGI_FORMAT (the view format: never a typeless one)
    uint32_t bytesPerPixel = 0;
    uint32_t frameCount = 0;
    uint32_t source = 0;          // Source
    int64_t qpcFrequency = 0;     // the frames' times are QueryPerformanceCounter ticks
    uint32_t content = 0;         // Content: 0 the format's own encoding; 1 an HDR frame's SDR view (0..1, sRGB-encoded) in a half-float texture;
                                  // 2 an HDR frame as light (linear, Rec.709, 1 = the SDR white) in a half-float texture (the upscalers since 0.9.10)
    uint32_t reserved[3] = {};
    char game[64] = {};           // the program in focus, lower case (may be empty)
};
struct FrameHeader {
    uint64_t index = 0;           // the addon's running count of frames offered: a gap is frames that were not recorded
    int64_t qpc = 0;
    uint32_t codec = kQoi;
    uint32_t bytes = 0;           // what follows
    uint32_t rawBytes = 0;        // width * height * bytesPerPixel
    uint32_t tag = 0;             // what the frame is, where the addon says (0 before 0.9.13): Tag
};
enum Tag : uint32_t { kUntagged = 0, kMadeBetween = 1, kReal = 2 };   // the presented frames with frame generation of our own
#pragma pack(pop)
static_assert(sizeof(FileHeader) == 128 && sizeof(FrameHeader) == 32, "the file layout");

// A frame compressed: its header and bytes.
struct Frame { FrameHeader header; std::vector<uint8_t> data; };

// Writes a whole file. The frames in order of index.
bool Write(const std::wstring& path, const FileHeader& header, const std::vector<const Frame*>& frames, std::string* error = nullptr);

// Reads a file frame by frame (the frames' positions are read at Open).
class Reader {
public:
    ~Reader() { Close(); }
    bool Open(const std::wstring& path, std::string* error = nullptr);
    void Close();
    const FileHeader& Header() const { return m_header; }
    size_t Count() const { return m_frames.size(); }
    const FrameHeader& FrameInfo(size_t i) const { return m_frames[i].header; }
    // The frame's pixels, tightly packed (width * bytesPerPixel a row).
    bool Read(size_t i, std::vector<uint8_t>& pixels);
private:
    struct Entry { FrameHeader header; int64_t offset; };
    FILE* m_file = nullptr;
    FileHeader m_header;
    std::vector<Entry> m_frames;
    std::vector<uint8_t> m_buffer;
};

uint32_t BytesPerPixel(uint32_t dxgiFormat);   // 4 or 8 for the formats a recording takes, 0 for others

} // namespace nr::lsrec
