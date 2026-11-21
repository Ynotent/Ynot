#include "ynot/media.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ynot {

// ------------------------------------------------------------------ SHA-256

namespace {
constexpr std::array<uint32_t, 64> K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256Block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
               (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
}  // namespace

Sha256::Sha256() : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::update(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    total_ += len;
    while (len > 0) {
        size_t take = std::min(len, size_t(64) - bufLen_);
        std::memcpy(buf_ + bufLen_, p, take);
        bufLen_ += take;
        p += take;
        len -= take;
        if (bufLen_ == 64) {
            sha256Block(h_, buf_);
            bufLen_ = 0;
        }
    }
}

std::string Sha256::finalHex() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (bufLen_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (8 * (7 - i)));
    update(len, 8);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h_)
        for (int s = 28; s >= 0; s -= 4) out += hex[(v >> s) & 0xF];
    return out;
}

std::string sha256Hex(const void* data, size_t len) {
    Sha256 h;
    h.update(data, len);
    return h.finalHex();
}

std::string sha256File(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    Sha256 h;
    std::vector<char> buf(1 << 16);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        h.update(buf.data(), static_cast<size_t>(f.gcount()));
    }
    return h.finalHex();
}

std::string sha256Hex(const std::string& s) { return sha256Hex(s.data(), s.size()); }

// ------------------------------------------------------------------ WAV

namespace {
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
}  // namespace

std::vector<uint8_t> encodeWavFloat32(const AudioBuffer& buf) {
    std::vector<uint8_t> v;
    uint32_t dataBytes = static_cast<uint32_t>(buf.samples.size() * 4);
    v.insert(v.end(), {'R', 'I', 'F', 'F'});
    put32(v, 36 + dataBytes);
    v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(v, 16);
    put16(v, 3);  // IEEE float
    put16(v, static_cast<uint16_t>(buf.channels));
    put32(v, static_cast<uint32_t>(buf.sampleRate));
    put32(v, static_cast<uint32_t>(buf.sampleRate) * buf.channels * 4);
    put16(v, static_cast<uint16_t>(buf.channels * 4));
    put16(v, 32);
    v.insert(v.end(), {'d', 'a', 't', 'a'});
    put32(v, dataBytes);
    for (float f : buf.samples) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        put32(v, u);
    }
    return v;
}

AudioBuffer decodeWav(const std::vector<uint8_t>& b) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4))
        throw std::runtime_error("wav: not a RIFF/WAVE file");
    AudioBuffer out;
    int fmt = 0, bits = 0;
    bool haveFmt = false;
    size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const uint8_t* ck = b.data() + pos;
        uint32_t size = get32(ck + 4);
        size_t body = pos + 8;
        if (body + size > b.size()) size = static_cast<uint32_t>(b.size() - body);
        if (!std::memcmp(ck, "fmt ", 4) && size >= 16) {
            fmt = get16(b.data() + body);
            out.channels = get16(b.data() + body + 2);
            out.sampleRate = get32(b.data() + body + 4);
            bits = get16(b.data() + body + 14);
            if (fmt == 0xFFFE && size >= 26) fmt = get16(b.data() + body + 24);  // extensible
            haveFmt = true;
        } else if (!std::memcmp(ck, "data", 4)) {
            if (!haveFmt || out.channels <= 0) throw std::runtime_error("wav: data before fmt");
            const uint8_t* d = b.data() + body;
            int bps = bits / 8;
            size_t n = size / static_cast<size_t>(bps);
            out.samples.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const uint8_t* s = d + i * bps;
                float f = 0;
                if (fmt == 3 && bits == 32) std::memcpy(&f, s, 4);
                else if (fmt == 1 && bits == 16) f = static_cast<int16_t>(get16(s)) / 32768.0f;
                else if (fmt == 1 && bits == 24) {
                    int32_t x = (s[0] << 8) | (s[1] << 16) | (s[2] << 24);
                    f = static_cast<float>(x >> 8) / 8388608.0f;
                } else if (fmt == 1 && bits == 32)
                    f = static_cast<float>(static_cast<int32_t>(get32(s)) / 2147483648.0);
                else throw std::runtime_error("wav: unsupported sample format");
                out.samples[i] = f;
            }
            return out;
        }
        pos = body + size + (size & 1);
    }
    throw std::runtime_error("wav: no data chunk");
}

// ------------------------------------------------------------------ streaming writer

WavFileWriter::~WavFileWriter() {
    try { close(); } catch (...) {}
}

bool WavFileWriter::open(const std::string& path, double sampleRate, int channels) {
    close();
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) return false;
    channels_ = channels;
    frames_ = 0;
    AudioBuffer empty;
    empty.sampleRate = sampleRate;
    empty.channels = channels;
    auto header = encodeWavFloat32(empty);  // 44-byte header, sizes patched in close()
    out_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    return static_cast<bool>(out_);
}

bool WavFileWriter::write(const float* interleaved, int64_t frames) {
    if (!out_.is_open() || frames <= 0) return frames == 0;
    const uint64_t newBytes = static_cast<uint64_t>(frames_ + frames) * static_cast<uint64_t>(channels_) * 4u;
    if (newBytes > 0xFFFFFFFFull - 36) return false;  // RIFF limit (~3 h stereo at 48 kHz); RF64 planned
    // Little-endian hosts (all four targets) can write floats directly.
    out_.write(reinterpret_cast<const char*>(interleaved), static_cast<std::streamsize>(frames * channels_ * 4));
    frames_ += frames;
    return static_cast<bool>(out_);
}

void WavFileWriter::close() {
    if (!out_.is_open()) return;
    uint32_t dataBytes = static_cast<uint32_t>(frames_ * channels_ * 4);
    std::vector<uint8_t> v;
    put32(v, 36 + dataBytes);
    out_.seekp(4);
    out_.write(reinterpret_cast<const char*>(v.data()), 4);
    v.clear();
    put32(v, dataBytes);
    out_.seekp(40);
    out_.write(reinterpret_cast<const char*>(v.data()), 4);
    out_.close();
}

std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void writeFileBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace ynot
