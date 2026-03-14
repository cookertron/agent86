#include "emitter.h"
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <stdlib.h>
#endif

CodeBuffer::CodeBuffer(size_t capacity) : capacity_(capacity), pos_(0) {
#ifdef _WIN32
    buf_ = (uint8_t*)VirtualAlloc(nullptr, capacity_, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
    if (!buf_) throw std::runtime_error("VirtualAlloc failed");
#else
    buf_ = (uint8_t*)mmap(nullptr, capacity_, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf_ == MAP_FAILED) throw std::runtime_error("mmap failed");
#endif
}

CodeBuffer::~CodeBuffer() {
#ifdef _WIN32
    if (buf_) VirtualFree(buf_, 0, MEM_RELEASE);
#else
    if (buf_) munmap(buf_, capacity_);
#endif
}

void CodeBuffer::emit8(uint8_t b) {
    if (pos_ + 1 > capacity_) throw std::runtime_error("CodeBuffer overflow");
    buf_[pos_++] = b;
}

void CodeBuffer::emit16(uint16_t w) {
    if (pos_ + 2 > capacity_) throw std::runtime_error("CodeBuffer overflow");
    memcpy(&buf_[pos_], &w, 2);
    pos_ += 2;
}

void CodeBuffer::emit32(uint32_t d) {
    if (pos_ + 4 > capacity_) throw std::runtime_error("CodeBuffer overflow");
    memcpy(&buf_[pos_], &d, 4);
    pos_ += 4;
}

void CodeBuffer::emit64(uint64_t q) {
    if (pos_ + 8 > capacity_) throw std::runtime_error("CodeBuffer overflow");
    memcpy(&buf_[pos_], &q, 8);
    pos_ += 8;
}

void CodeBuffer::emitBytes(const uint8_t* data, size_t len) {
    if (pos_ + len > capacity_) throw std::runtime_error("CodeBuffer overflow");
    memcpy(&buf_[pos_], data, len);
    pos_ += len;
}

void CodeBuffer::patch32(size_t offset, uint32_t val) {
    memcpy(&buf_[offset], &val, 4);
}

void CodeBuffer::patch8(size_t offset, uint8_t val) {
    buf_[offset] = val;
}
