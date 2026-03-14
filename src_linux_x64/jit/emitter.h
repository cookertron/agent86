#pragma once
#include <cstdint>
#include <cstddef>

// Executable code buffer using mmap
class CodeBuffer {
public:
    CodeBuffer(size_t capacity = 4096);
    ~CodeBuffer();

    // Disable copy
    CodeBuffer(const CodeBuffer&) = delete;
    CodeBuffer& operator=(const CodeBuffer&) = delete;

    void emit8(uint8_t b);
    void emit16(uint16_t w);
    void emit32(uint32_t d);
    void emit64(uint64_t q);
    void emitBytes(const uint8_t* data, size_t len);

    size_t size() const { return pos_; }
    uint8_t* data() { return buf_; }
    void reset() { pos_ = 0; }

    // Get function pointer to emitted code
    template<typename F>
    F getFunc() { return reinterpret_cast<F>(buf_); }

    // Patch a 32-bit value at a given offset
    void patch32(size_t offset, uint32_t val);

    // Patch a single byte at a given offset
    void patch8(size_t offset, uint8_t val);

    // Current write position (for computing relative offsets)
    size_t cursor() const { return pos_; }

private:
    uint8_t* buf_;
    size_t   capacity_;
    size_t   pos_;
};
