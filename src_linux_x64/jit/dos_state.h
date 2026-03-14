#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <glob.h>

struct DosState {
    FILE* handles[20];          // 0-4 = device sentinels, 5-19 = user files
    uint16_t dta_seg = 0;       // DTA segment (set by AH=1Ah, default=COM seg)
    uint16_t dta_addr = 0x80;   // DTA offset  (set by AH=1Ah, default=PSP+80h)
    std::string current_dir;    // no drive letter, no leading backslash

    // POSIX FindFirst/FindNext state
    std::vector<std::string> find_results;
    size_t find_index = 0;
    bool find_active = false;

    // Simple top-down paragraph memory allocator
    struct MemBlock { uint16_t seg; uint16_t size; bool free; };
    std::vector<MemBlock> mem_blocks;
    uint16_t mem_top = 0x9FFF;  // top available paragraph (below video memory at A000:0)

    DosState() {
        memset(handles, 0, sizeof(handles));
        // Sentinels for device handles — never dereferenced
        for (int i = 0; i < 5; i++)
            handles[i] = reinterpret_cast<FILE*>(static_cast<uintptr_t>(i + 1));
    }

    ~DosState() {
        // Close any open user file handles
        for (int i = 5; i < 20; i++) {
            if (handles[i]) { fclose(handles[i]); handles[i] = nullptr; }
        }
    }

    // Allocate a free handle slot (5-19). Returns -1 if full.
    int allocHandle(FILE* fp) {
        for (int i = 5; i < 20; i++) {
            if (!handles[i]) { handles[i] = fp; return i; }
        }
        return -1;
    }

    // Compute 20-bit physical address from segment:offset
    static inline uint32_t physAddr(uint16_t seg, uint16_t off) {
        return ((uint32_t)seg * 16 + off) & 0xFFFFF;
    }

    // Read ASCIIZ string from guest memory at seg:addr
    static std::string readASCIIZ(const uint8_t* memory, uint16_t seg, uint16_t addr) {
        std::string s;
        for (int i = 0; i < 256; i++) {
            uint32_t phys = physAddr(seg, (uint16_t)(addr + i));
            uint8_t ch = memory[phys];
            if (ch == 0) break;
            s += static_cast<char>(ch);
        }
        return s;
    }

    // Resolve a DOS path from guest memory: strip drive letter, convert \ to /
    std::string resolvePath(const uint8_t* memory, uint16_t seg, uint16_t addr) const {
        std::string raw = readASCIIZ(memory, seg, addr);
        // Strip drive letter (e.g. "C:\")
        if (raw.size() >= 2 && raw[1] == ':') {
            raw = raw.substr(2);
        }
        // Convert backslashes to forward slashes
        for (char& c : raw) { if (c == '\\') c = '/'; }
        // Strip leading slash
        if (!raw.empty() && raw[0] == '/') raw = raw.substr(1);
        // If we have a current_dir and path is relative, prepend it
        if (!current_dir.empty() && !raw.empty() && raw[0] != '/') {
            return current_dir + "/" + raw;
        }
        return raw;
    }

    // Memory allocator: allocate paragraphs, returns segment or 0 on failure
    // Segments below 0x1000 (physical 0x10000) are reserved for the COM program
    static const uint16_t MEM_FLOOR = 0x1000;

    uint16_t allocMemory(uint16_t paragraphs) {
        if (paragraphs == 0) return 0;
        if (mem_top < paragraphs) return 0;
        uint16_t seg = mem_top - paragraphs + 1;
        if (seg < MEM_FLOOR) return 0;  // don't overlap COM segment
        mem_blocks.push_back({seg, paragraphs, false});
        mem_top = seg - 1;
        return seg;
    }

    // Free a previously allocated block
    bool freeMemory(uint16_t seg) {
        for (auto& b : mem_blocks) {
            if (b.seg == seg && !b.free) {
                b.free = true;
                // Reclaim if topmost
                while (!mem_blocks.empty() && mem_blocks.back().free) {
                    mem_top = mem_blocks.back().seg + mem_blocks.back().size - 1;
                    mem_blocks.pop_back();
                }
                return true;
            }
        }
        return false;
    }

    // Resize a previously allocated block
    bool resizeMemory(uint16_t seg, uint16_t new_size) {
        for (size_t i = 0; i < mem_blocks.size(); i++) {
            if (mem_blocks[i].seg == seg && !mem_blocks[i].free) {
                // Only allow resize of topmost block (simplification)
                if (i == mem_blocks.size() - 1) {
                    // Not topmost, can't resize
                }
                // Actually: it IS the last block
                if (i == mem_blocks.size() - 1) {
                    uint16_t old_size = mem_blocks[i].size;
                    if (new_size <= old_size) {
                        // Shrink: reclaim paragraphs
                        mem_top += (old_size - new_size);
                        mem_blocks[i].size = new_size;
                        return true;
                    } else {
                        // Grow: check if space available
                        uint16_t extra = new_size - old_size;
                        if (seg >= extra + 1) {
                            uint16_t new_seg = seg - (new_size - old_size);
                            mem_blocks[i].seg = new_seg;
                            mem_blocks[i].size = new_size;
                            mem_top = new_seg - 1;
                            return true;
                        }
                    }
                }
                return false;
            }
        }
        return false;
    }

    // Get max available paragraphs (above MEM_FLOOR)
    uint16_t maxAvailable() const {
        return (mem_top >= MEM_FLOOR) ? (mem_top - MEM_FLOOR + 1) : 0;
    }
};
