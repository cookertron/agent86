#include "dos.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#endif

// ── Helpers ──────────────────────────────────────────────────────────────────

static inline uint32_t physAddr(uint16_t seg, uint16_t off) {
    return ((uint32_t)seg * 16 + off) & 0xFFFFF;
}

static void clearCF(CPU8086& cpu) {
    cpu.flags &= ~F_CF;
}

static void setError(CPU8086& cpu, uint16_t code) {
    cpu.flags |= F_CF;
    cpu.regs[R_AX] = code;
}

static uint16_t packDOSTime(int hour, int min, int sec) {
    return (uint16_t)((hour << 11) | (min << 5) | (sec / 2));
}

static uint16_t packDOSDate(int year, int month, int day) {
    return (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
}

// Write a 43-byte DTA record at physical address dta_phys in guest memory
#ifdef _WIN32
static void writeDTARecord(CPU8086& cpu, uint32_t dta_phys,
                           const WIN32_FIND_DATAA& fd) {
    uint8_t* mem = cpu.memory;

    // Offset 0x00: 21 reserved bytes (zero)
    memset(&mem[dta_phys], 0, 21);

    // Offset 0x15: file attributes (1 byte)
    uint8_t attr = 0;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)  attr |= 0x01;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)    attr |= 0x02;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)    attr |= 0x04;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) attr |= 0x10;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)   attr |= 0x20;
    mem[(dta_phys + 0x15) & 0xFFFFF] = attr;

    // Convert FILETIME to DOS date/time
    FILETIME local;
    WORD dosDate = 0, dosTime = 0;
    FileTimeToLocalFileTime(&fd.ftLastWriteTime, &local);
    FileTimeToDosDateTime(&local, &dosDate, &dosTime);

    // Offset 0x16: time (2 bytes LE)
    mem[(dta_phys + 0x16) & 0xFFFFF] = dosTime & 0xFF;
    mem[(dta_phys + 0x17) & 0xFFFFF] = (dosTime >> 8) & 0xFF;

    // Offset 0x18: date (2 bytes LE)
    mem[(dta_phys + 0x18) & 0xFFFFF] = dosDate & 0xFF;
    mem[(dta_phys + 0x19) & 0xFFFFF] = (dosDate >> 8) & 0xFF;

    // Offset 0x1A: file size (4 bytes LE)
    uint32_t size = fd.nFileSizeLow;
    mem[(dta_phys + 0x1A) & 0xFFFFF] = size & 0xFF;
    mem[(dta_phys + 0x1B) & 0xFFFFF] = (size >> 8) & 0xFF;
    mem[(dta_phys + 0x1C) & 0xFFFFF] = (size >> 16) & 0xFF;
    mem[(dta_phys + 0x1D) & 0xFFFFF] = (size >> 24) & 0xFF;

    // Offset 0x1E: filename (13 bytes ASCIIZ, uppercase, 8.3)
    const char* name = fd.cAlternateFileName[0] ? fd.cAlternateFileName : fd.cFileName;
    char upper[13];
    memset(upper, 0, 13);
    for (int i = 0; i < 12 && name[i]; i++)
        upper[i] = (char)toupper((unsigned char)name[i]);
    memcpy(&mem[(dta_phys + 0x1E) & 0xFFFFF], upper, 13);
}
#endif

// Write a character to the video framebuffer at the cursor, advancing cursor
// and scrolling as needed. Mirrors INT 10h AH=0Eh teletype behavior.
static void videoWriteChar(CPU8086& cpu, VideoState& video, uint8_t ch) {
    if (!video.active) return;
    switch (ch) {
        case '\r': video.cursor_col = 0; break;
        case '\n': video.cursor_row++; break;
        case '\b': if (video.cursor_col > 0) video.cursor_col--; break;
        case '\t': {
            int next = (video.cursor_col + 8) & ~7;
            if (next >= video.cols) next = video.cols - 1;
            video.cursor_col = next;
            break;
        }
        default: {
            uint32_t a = video.cellAddr(video.cursor_row, video.cursor_col);
            cpu.memory[a] = ch;
            video.cursor_col++;
            if (video.cursor_col >= video.cols) {
                video.cursor_col = 0;
                video.cursor_row++;
            }
            break;
        }
    }
    if (video.cursor_row >= video.rows) {
        uint32_t row_bytes = (uint32_t)video.cols * 2;
        memmove(&cpu.memory[video.vram_base],
                &cpu.memory[video.vram_base + row_bytes],
                (video.rows - 1) * row_bytes);
        for (int c = 0; c < video.cols; c++) {
            uint32_t a = video.cellAddr(video.rows - 1, c);
            cpu.memory[a] = ' ';
            cpu.memory[a + 1] = 0x07;
        }
        video.cursor_row = video.rows - 1;
    }
}

// ── Main handler ─────────────────────────────────────────────────────────────

bool handleDOSInt(CPU8086& cpu, int intNum, std::string& output,
                  DosState& dos, VideoState& video, KeyboardBuffer* kbd,
                  MouseState* mouse) {

    if (intNum == 0x20) {
        cpu.halted = true;
        return true;
    }

    if (intNum == 0x21) {
        uint8_t ah = (cpu.regs[R_AX] >> 8) & 0xFF;

        switch (ah) {

        // ── Console & I/O ────────────────────────────────────────────────

        case 0x01: {
            // AH=01h — Read character with echo
            if (!kbd) { setError(cpu, 0x06); return true; }
            Keystroke key;
            if (!kbd->blockingRead(key)) { setError(cpu, 0x06); return true; }
            cpu.regs[R_AX] = (cpu.regs[R_AX] & 0xFF00) | key.ascii;
            output += (char)key.ascii;
            videoWriteChar(cpu, video, key.ascii);
            return true;
        }

        case 0x02: {
            // AH=02h — Print character in DL
            char ch = (char)(cpu.regs[R_DX] & 0xFF);
            output += ch;
            videoWriteChar(cpu, video, (uint8_t)ch);
            return true;
        }

        case 0x06: {
            // AH=06h — Direct console I/O
            uint8_t dl = cpu.regs[R_DX] & 0xFF;
            if (dl == 0xFF) {
                // Input mode: non-blocking read
                if (kbd && kbd->hasPendingExtended()) {
                    // Second byte of extended key two-byte sequence
                    uint8_t b = kbd->consumePendingExtended();
                    cpu.regs[R_AX] = (cpu.regs[R_AX] & 0xFF00) | b;
                    cpu.flags &= ~F_ZF;
                } else if (kbd) {
                    Keystroke key;
                    bool has_key = false;
                    kbd->poll(key, has_key);
                    if (has_key) {
                        kbd->blockingRead(key); // consume it
                        if (key.ascii == 0x00) {
                            // Extended key: return 0x00 now, scan code on next call
                            cpu.regs[R_AX] = (cpu.regs[R_AX] & 0xFF00);
                            cpu.flags &= ~F_ZF;
                            kbd->setPendingExtended(key.scancode);
                        } else {
                            cpu.regs[R_AX] = (cpu.regs[R_AX] & 0xFF00) | key.ascii;
                            cpu.flags &= ~F_ZF;
                        }
                    } else {
                        cpu.regs[R_AX] = (cpu.regs[R_AX] & 0xFF00);
                        cpu.flags |= F_ZF;  // ZF=1 → no char
                    }
                } else {
                    cpu.regs[R_AX] = (cpu.regs[R_AX] & 0xFF00);
                    cpu.flags |= F_ZF;
                }
            } else {
                // Output mode: write DL to output
                output += (char)dl;
                videoWriteChar(cpu, video, dl);
            }
            return true;
        }

        case 0x09: {
            // AH=09h — Print $-terminated string at DS:DX
            uint16_t ds = cpu.sregs[S_DS];
            uint16_t addr = cpu.regs[R_DX];
            for (int i = 0; i < 65536; i++) {
                uint8_t ch = cpu.memory[physAddr(ds, addr)];
                if (ch == '$') break;
                output += (char)ch;
                videoWriteChar(cpu, video, ch);
                addr++;
            }
            return true;
        }

        // ── DTA ──────────────────────────────────────────────────────────

        case 0x1A: {
            // AH=1Ah — Set Disk Transfer Address (DS:DX)
            dos.dta_seg = cpu.sregs[S_DS];
            dos.dta_addr = cpu.regs[R_DX];
            return true;
        }

        // ── IVT stubs ───────────────────────────────────────────────────

        case 0x25: {
            // AH=25h — Set interrupt vector — silently ignore
            return true;
        }

        case 0x35: {
            // AH=35h — Get interrupt vector — stub: ES:BX = 0:0
            cpu.sregs[S_ES] = 0;
            cpu.regs[R_BX] = 0;
            return true;
        }

        // ── Date/Time ────────────────────────────────────────────────────

        case 0x2A: {
            // AH=2Ah — Get system date (hardcoded)
            cpu.regs[R_CX] = 2026;  // year
            cpu.regs[R_DX] = (3 << 8) | 1; // DH=month(3), DL=day(1)
            cpu.regs[R_AX] = (cpu.regs[R_AX] & 0xFF00) | 0; // AL=day of week (0=Sun)
            return true;
        }

        case 0x2C: {
            // AH=2Ch — Get system time (hardcoded)
            cpu.regs[R_CX] = (12 << 8) | 0;  // CH=hour, CL=min
            cpu.regs[R_DX] = (0 << 8) | 0;   // DH=sec, DL=1/100
            return true;
        }

        // ── DTA query ────────────────────────────────────────────────────

        case 0x2F: {
            // AH=2Fh — Get DTA address → ES:BX
            cpu.sregs[S_ES] = dos.dta_seg;
            cpu.regs[R_BX] = dos.dta_addr;
            return true;
        }

        // ── DOS version ──────────────────────────────────────────────────

        case 0x30: {
            // AH=30h — Get DOS version
            cpu.regs[R_AX] = (0 << 8) | 5; // AL=major(5), AH=minor(0)
            cpu.regs[R_BX] = 0;
            cpu.regs[R_CX] = 0;
            return true;
        }

        // ── Drive & Directory ────────────────────────────────────────────

        case 0x0E: {
            // AH=0Eh — Select default drive
            cpu.regs[R_AX] = (cpu.regs[R_AX] & 0xFF00) | 3;
            return true;
        }

        case 0x19: {
            // AH=19h — Get current default drive
            cpu.regs[R_AX] = (cpu.regs[R_AX] & 0xFF00) | 2; // C:
            return true;
        }

        case 0x3B: {
            // AH=3Bh — Change directory (DS:DX = ASCIIZ path)
            std::string path = dos.resolvePath(cpu.memory, cpu.sregs[S_DS], cpu.regs[R_DX]);
#ifdef _WIN32
            DWORD attrs = GetFileAttributesA(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                dos.current_dir = path;
                clearCF(cpu);
            } else {
                setError(cpu, 0x03); // path not found
            }
#else
            struct stat st;
            if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                dos.current_dir = path;
                clearCF(cpu);
            } else {
                setError(cpu, 0x03);
            }
#endif
            return true;
        }

        case 0x47: {
            // AH=47h — Get current directory (DS:SI = buffer)
            uint16_t ds = cpu.sregs[S_DS];
            uint16_t si = cpu.regs[R_SI];
            const std::string& dir = dos.current_dir;
            for (size_t i = 0; i < dir.size() && i < 63; i++) {
                char c = dir[i];
                if (c == '/') c = '\\';
                cpu.memory[physAddr(ds, (uint16_t)(si + i))] = (uint8_t)c;
            }
            cpu.memory[physAddr(ds, (uint16_t)(si + dir.size()))] = 0;
            clearCF(cpu);
            return true;
        }

        // ── File System ──────────────────────────────────────────────────

        case 0x3C: {
            // AH=3Ch — Create file (DS:DX = ASCIIZ path, CX = attributes)
            std::string path = dos.resolvePath(cpu.memory, cpu.sregs[S_DS], cpu.regs[R_DX]);
            FILE* fp = fopen(path.c_str(), "wb+");
            if (!fp) { setError(cpu, 0x03); return true; }
            int h = dos.allocHandle(fp);
            if (h < 0) { fclose(fp); setError(cpu, 0x04); return true; } // too many open files
            cpu.regs[R_AX] = (uint16_t)h;
            clearCF(cpu);
            return true;
        }

        case 0x3D: {
            // AH=3Dh — Open file (DS:DX = ASCIIZ path, AL = mode)
            std::string path = dos.resolvePath(cpu.memory, cpu.sregs[S_DS], cpu.regs[R_DX]);
            uint8_t al = cpu.regs[R_AX] & 0xFF;
            const char* mode;
            switch (al & 0x03) {
                case 0: mode = "rb"; break;   // read only
                case 1: mode = "r+b"; break;  // write only (open existing)
                case 2: mode = "r+b"; break;  // read/write
                default: mode = "rb"; break;
            }
            FILE* fp = fopen(path.c_str(), mode);
            if (!fp) { setError(cpu, 0x02); return true; } // file not found
            int h = dos.allocHandle(fp);
            if (h < 0) { fclose(fp); setError(cpu, 0x04); return true; }
            cpu.regs[R_AX] = (uint16_t)h;
            clearCF(cpu);
            return true;
        }

        case 0x3E: {
            // AH=3Eh — Close file (BX = handle)
            uint16_t bx = cpu.regs[R_BX];
            if (bx < 5) { clearCF(cpu); return true; } // skip device handles
            if (bx >= 20 || !dos.handles[bx]) { setError(cpu, 0x06); return true; } // invalid handle
            fclose(dos.handles[bx]);
            dos.handles[bx] = nullptr;
            clearCF(cpu);
            return true;
        }

        case 0x3F: {
            // AH=3Fh — Read file (BX = handle, CX = count, DS:DX = buffer)
            uint16_t bx = cpu.regs[R_BX];
            uint16_t cx = cpu.regs[R_CX];
            uint16_t dx = cpu.regs[R_DX];

            if (bx == 0) {
                // stdin: read from keyboard into DS:DX
                if (!kbd) { setError(cpu, 0x06); return true; }
                uint16_t ds = cpu.sregs[S_DS];
                uint16_t i = 0;
                for (; i < cx; i++) {
                    Keystroke key;
                    if (!kbd->blockingRead(key)) break;
                    cpu.memory[physAddr(ds, (uint16_t)(dx + i))] = key.ascii;
                    if (key.ascii == 0x0D) { i++; break; } // CR terminates
                }
                cpu.regs[R_AX] = i;
                clearCF(cpu);
                return true;
            }

            if (bx >= 5 && bx < 20 && dos.handles[bx]) {
                uint32_t phys = physAddr(cpu.sregs[S_DS], dx);
                size_t n = fread(&cpu.memory[phys], 1, cx, dos.handles[bx]);
                cpu.regs[R_AX] = (uint16_t)n;
                clearCF(cpu);
            } else {
                setError(cpu, 0x06); // invalid handle
            }
            return true;
        }

        case 0x40: {
            // AH=40h — Write file (BX = handle, CX = count, DS:DX = buffer)
            uint16_t bx = cpu.regs[R_BX];
            uint16_t cx = cpu.regs[R_CX];
            uint16_t dx = cpu.regs[R_DX];

            if (bx == 1 || bx == 2) {
                // stdout/stderr → dos_output_ + video (read from DS:DX)
                uint16_t ds = cpu.sregs[S_DS];
                for (uint16_t i = 0; i < cx; i++) {
                    uint8_t byte = cpu.memory[physAddr(ds, (uint16_t)(dx + i))];
                    output += (char)byte;
                    videoWriteChar(cpu, video, byte);
                }
                cpu.regs[R_AX] = cx;
                clearCF(cpu);
                return true;
            }

            if (bx >= 5 && bx < 20 && dos.handles[bx]) {
                if (cx == 0) {
                    // Truncate at current position
                    long pos = ftell(dos.handles[bx]);
#ifdef _WIN32
                    _chsize(_fileno(dos.handles[bx]), pos);
#else
                    ftruncate(fileno(dos.handles[bx]), pos);
#endif
                    cpu.regs[R_AX] = 0;
                } else {
                    uint32_t phys = physAddr(cpu.sregs[S_DS], dx);
                    size_t n = fwrite(&cpu.memory[phys], 1, cx, dos.handles[bx]);
                    fflush(dos.handles[bx]);
                    cpu.regs[R_AX] = (uint16_t)n;
                }
                clearCF(cpu);
            } else {
                setError(cpu, 0x06);
            }
            return true;
        }

        case 0x41: {
            // AH=41h — Delete file (DS:DX = ASCIIZ path)
            std::string path = dos.resolvePath(cpu.memory, cpu.sregs[S_DS], cpu.regs[R_DX]);
            if (remove(path.c_str()) == 0) {
                clearCF(cpu);
            } else {
                setError(cpu, 0x02); // file not found
            }
            return true;
        }

        case 0x42: {
            // AH=42h — Seek (BX = handle, AL = origin, CX:DX = offset)
            uint16_t bx = cpu.regs[R_BX];
            uint8_t al = cpu.regs[R_AX] & 0xFF;
            int32_t offset = (int32_t)(((uint32_t)cpu.regs[R_CX] << 16) | cpu.regs[R_DX]);

            if (bx < 5 || bx >= 20 || !dos.handles[bx]) {
                setError(cpu, 0x06); return true;
            }

            int origin;
            switch (al) {
                case 0: origin = SEEK_SET; break;
                case 1: origin = SEEK_CUR; break;
                case 2: origin = SEEK_END; break;
                default: setError(cpu, 0x01); return true;
            }

            if (fseek(dos.handles[bx], offset, origin) != 0) {
                setError(cpu, 0x19); // seek error
                return true;
            }

            long pos = ftell(dos.handles[bx]);
            cpu.regs[R_DX] = (uint16_t)((pos >> 16) & 0xFFFF);
            cpu.regs[R_AX] = (uint16_t)(pos & 0xFFFF);
            clearCF(cpu);
            return true;
        }

        case 0x43: {
            // AH=43h — Get/set file attributes
            uint8_t al = cpu.regs[R_AX] & 0xFF;
            std::string path = dos.resolvePath(cpu.memory, cpu.sregs[S_DS], cpu.regs[R_DX]);

            if (al == 0x00) {
                // Get attributes
#ifdef _WIN32
                DWORD attrs = GetFileAttributesA(path.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES) {
                    setError(cpu, 0x02); return true;
                }
                uint16_t dosAttr = 0;
                if (attrs & FILE_ATTRIBUTE_READONLY)  dosAttr |= 0x01;
                if (attrs & FILE_ATTRIBUTE_HIDDEN)    dosAttr |= 0x02;
                if (attrs & FILE_ATTRIBUTE_SYSTEM)    dosAttr |= 0x04;
                if (attrs & FILE_ATTRIBUTE_DIRECTORY) dosAttr |= 0x10;
                if (attrs & FILE_ATTRIBUTE_ARCHIVE)   dosAttr |= 0x20;
                cpu.regs[R_CX] = dosAttr;
                clearCF(cpu);
#else
                struct stat st;
                if (stat(path.c_str(), &st) != 0) {
                    setError(cpu, 0x02); return true;
                }
                cpu.regs[R_CX] = 0x20; // archive
                clearCF(cpu);
#endif
            } else {
                // Set attributes — stub, just succeed
                clearCF(cpu);
            }
            return true;
        }

        case 0x44: {
            // AH=44h — IOCTL
            uint8_t al = cpu.regs[R_AX] & 0xFF;

            if (al == 0x00) {
                // IOCTL: Get device info
                uint16_t bx = cpu.regs[R_BX];
                if (bx < 5) {
                    cpu.regs[R_DX] = 0x80D3; // device: STDIN/STDOUT/etc
                } else if (bx < 20 && dos.handles[bx]) {
                    cpu.regs[R_DX] = 0x0000; // disk file
                } else {
                    setError(cpu, 0x06); return true;
                }
                clearCF(cpu);
                return true;
            }

            if (al == 0x09) {
                // IOCTL: Is drive remote
                uint8_t drive = cpu.regs[R_BX] & 0xFF;
                if (drive <= 3) {
                    clearCF(cpu);
                    cpu.regs[R_DX] = 0x0000;
                } else {
                    cpu.flags |= F_CF;
                }
                return true;
            }

            return false;
        }

        // ── FindFirst / FindNext ─────────────────────────────────────────

        case 0x4E: {
            // AH=4Eh — FindFirst (DS:DX = ASCIIZ pattern, CX = attr mask)
#ifdef _WIN32
            std::string pattern = dos.resolvePath(cpu.memory, cpu.sregs[S_DS], cpu.regs[R_DX]);
            // Close any previous search
            if (dos.find_handle != INVALID_HANDLE_VALUE) {
                FindClose(dos.find_handle);
                dos.find_handle = INVALID_HANDLE_VALUE;
                dos.find_active = false;
            }
            dos.find_handle = FindFirstFileA(pattern.c_str(), &dos.find_data);
            if (dos.find_handle == INVALID_HANDLE_VALUE) {
                setError(cpu, 0x12); // no more files
                return true;
            }
            dos.find_active = true;
            writeDTARecord(cpu, physAddr(dos.dta_seg, dos.dta_addr), dos.find_data);
            clearCF(cpu);
#else
            setError(cpu, 0x12);
#endif
            return true;
        }

        case 0x4F: {
            // AH=4Fh — FindNext
#ifdef _WIN32
            if (!dos.find_active || dos.find_handle == INVALID_HANDLE_VALUE) {
                setError(cpu, 0x12); return true;
            }
            if (!FindNextFileA(dos.find_handle, &dos.find_data)) {
                FindClose(dos.find_handle);
                dos.find_handle = INVALID_HANDLE_VALUE;
                dos.find_active = false;
                setError(cpu, 0x12); // no more files
                return true;
            }
            writeDTARecord(cpu, physAddr(dos.dta_seg, dos.dta_addr), dos.find_data);
            clearCF(cpu);
#else
            setError(cpu, 0x12);
#endif
            return true;
        }

        // ── Memory management ────────────────────────────────────────────

        case 0x48: {
            // AH=48h — Allocate memory (BX = paragraphs)
            uint16_t bx = cpu.regs[R_BX];
            uint16_t seg = dos.allocMemory(bx);
            if (seg == 0) {
                cpu.flags |= F_CF;
                cpu.regs[R_AX] = 0x08; // insufficient memory
                cpu.regs[R_BX] = dos.maxAvailable();
            } else {
                cpu.regs[R_AX] = seg;
                clearCF(cpu);
            }
            return true;
        }

        case 0x49: {
            // AH=49h — Free memory (ES = segment)
            if (dos.freeMemory(cpu.sregs[S_ES])) {
                clearCF(cpu);
            } else {
                setError(cpu, 0x09); // invalid memory block
            }
            return true;
        }

        case 0x4A: {
            // AH=4Ah — Resize memory (ES = segment, BX = new size in paragraphs)
            if (dos.resizeMemory(cpu.sregs[S_ES], cpu.regs[R_BX])) {
                clearCF(cpu);
            } else {
                cpu.flags |= F_CF;
                cpu.regs[R_AX] = 0x08;
                cpu.regs[R_BX] = dos.maxAvailable();
            }
            return true;
        }

        // ── Process & Environment ────────────────────────────────────────

        case 0x4C: {
            // AH=4Ch — Terminate with return code
            cpu.halted = true;
            return true;
        }

        case 0x56: {
            // AH=56h — Rename file (DS:DX = old, ES:DI = new)
            std::string oldPath = dos.resolvePath(cpu.memory, cpu.sregs[S_DS], cpu.regs[R_DX]);
            std::string newPath = dos.resolvePath(cpu.memory, cpu.sregs[S_ES], cpu.regs[R_DI]);
            if (rename(oldPath.c_str(), newPath.c_str()) == 0) {
                clearCF(cpu);
            } else {
                setError(cpu, 0x05); // access denied
            }
            return true;
        }

        case 0x57: {
            // AH=57h — Get/set file date/time
            uint8_t al = cpu.regs[R_AX] & 0xFF;
            uint16_t bx = cpu.regs[R_BX];

            if (al == 0x00) {
                // Get date/time
                if (bx < 5 || bx >= 20 || !dos.handles[bx]) {
                    setError(cpu, 0x06); return true;
                }
#ifdef _WIN32
                struct _stat st;
                if (_fstat(_fileno(dos.handles[bx]), &st) == 0) {
                    struct tm* t = localtime(&st.st_mtime);
                    cpu.regs[R_CX] = packDOSTime(t->tm_hour, t->tm_min, t->tm_sec);
                    cpu.regs[R_DX] = packDOSDate(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
                    clearCF(cpu);
                } else {
                    setError(cpu, 0x06);
                }
#else
                struct stat st;
                if (fstat(fileno(dos.handles[bx]), &st) == 0) {
                    struct tm* t = localtime(&st.st_mtime);
                    cpu.regs[R_CX] = packDOSTime(t->tm_hour, t->tm_min, t->tm_sec);
                    cpu.regs[R_DX] = packDOSDate(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
                    clearCF(cpu);
                } else {
                    setError(cpu, 0x06);
                }
#endif
            } else {
                // Set date/time — stub, just succeed
                clearCF(cpu);
            }
            return true;
        }

        case 0x62: {
            // AH=62h — Get PSP segment
            cpu.regs[R_BX] = 0; // flat .COM model
            return true;
        }

        default:
            return false;
        }
    }

    // ── INT 10h — BIOS Video ────────────────────────────────────────────

    if (intNum == 0x10) {
        uint8_t ah = (cpu.regs[R_AX] >> 8) & 0xFF;

        switch (ah) {
        case 0x00: {
            // AH=00h — Set video mode (AL = mode)
            // Clear VRAM with spaces + attr 0x07
            if (video.active) {
                for (int i = 0; i < video.rows * video.cols; i++) {
                    cpu.memory[video.vram_base + (uint32_t)i * 2] = ' ';
                    cpu.memory[video.vram_base + (uint32_t)i * 2 + 1] = 0x07;
                }
                video.cursor_row = 0;
                video.cursor_col = 0;
            }
            return true;
        }
        case 0x01: {
            // AH=01h — Set cursor shape (CH=start scan line, CL=end scan line)
            // CH bit 5 set = cursor hidden
            uint8_t ch = (cpu.regs[R_CX] >> 8) & 0xFF;
            uint8_t cl = cpu.regs[R_CX] & 0xFF;
            video.cursor_hidden = (ch & 0x20) != 0;
            video.cursor_start = ch & 0x1F;
            video.cursor_end = cl & 0x1F;
            return true;
        }
        case 0x02: {
            // AH=02h — Set cursor position (BH=page, DH=row, DL=col)
            video.cursor_row = (cpu.regs[R_DX] >> 8) & 0xFF;
            video.cursor_col = cpu.regs[R_DX] & 0xFF;
            return true;
        }
        case 0x03: {
            // AH=03h — Get cursor position (BH=page)
            cpu.regs[R_DX] = ((uint16_t)video.cursor_row << 8) | (uint8_t)video.cursor_col;
            uint8_t ch = video.cursor_start;
            if (video.cursor_hidden) ch |= 0x20;
            cpu.regs[R_CX] = ((uint16_t)ch << 8) | video.cursor_end;
            return true;
        }
        case 0x06: {
            // AH=06h — Scroll up (AL=lines, BH=attr, CX=upper-left, DX=lower-right)
            if (!video.active) return true;
            uint8_t lines = cpu.regs[R_AX] & 0xFF;
            uint8_t attr = (cpu.regs[R_BX] >> 8) & 0xFF;
            int top = (cpu.regs[R_CX] >> 8) & 0xFF;
            int left = cpu.regs[R_CX] & 0xFF;
            int bottom = (cpu.regs[R_DX] >> 8) & 0xFF;
            int right = cpu.regs[R_DX] & 0xFF;
            if (bottom >= video.rows) bottom = video.rows - 1;
            if (right >= video.cols) right = video.cols - 1;
            int width = right - left + 1;
            int height = bottom - top + 1;
            if (lines == 0 || lines >= height) {
                // Clear window
                for (int r = top; r <= bottom; r++) {
                    for (int c = left; c <= right; c++) {
                        uint32_t a = video.cellAddr(r, c);
                        cpu.memory[a] = ' ';
                        cpu.memory[a + 1] = attr;
                    }
                }
            } else {
                // Scroll up
                for (int r = top; r <= bottom - lines; r++) {
                    for (int c = left; c <= right; c++) {
                        uint32_t dst = video.cellAddr(r, c);
                        uint32_t src = video.cellAddr(r + lines, c);
                        cpu.memory[dst] = cpu.memory[src];
                        cpu.memory[dst + 1] = cpu.memory[src + 1];
                    }
                }
                // Clear vacated bottom lines
                for (int r = bottom - lines + 1; r <= bottom; r++) {
                    for (int c = left; c <= right; c++) {
                        uint32_t a = video.cellAddr(r, c);
                        cpu.memory[a] = ' ';
                        cpu.memory[a + 1] = attr;
                    }
                }
            }
            return true;
        }
        case 0x07: {
            // AH=07h — Scroll down (AL=lines, BH=attr, CX=upper-left, DX=lower-right)
            if (!video.active) return true;
            uint8_t lines = cpu.regs[R_AX] & 0xFF;
            uint8_t attr = (cpu.regs[R_BX] >> 8) & 0xFF;
            int top = (cpu.regs[R_CX] >> 8) & 0xFF;
            int left = cpu.regs[R_CX] & 0xFF;
            int bottom = (cpu.regs[R_DX] >> 8) & 0xFF;
            int right = cpu.regs[R_DX] & 0xFF;
            if (bottom >= video.rows) bottom = video.rows - 1;
            if (right >= video.cols) right = video.cols - 1;
            int height = bottom - top + 1;
            if (lines == 0 || lines >= height) {
                for (int r = top; r <= bottom; r++) {
                    for (int c = left; c <= right; c++) {
                        uint32_t a = video.cellAddr(r, c);
                        cpu.memory[a] = ' ';
                        cpu.memory[a + 1] = attr;
                    }
                }
            } else {
                for (int r = bottom; r >= top + lines; r--) {
                    for (int c = left; c <= right; c++) {
                        uint32_t dst = video.cellAddr(r, c);
                        uint32_t src = video.cellAddr(r - lines, c);
                        cpu.memory[dst] = cpu.memory[src];
                        cpu.memory[dst + 1] = cpu.memory[src + 1];
                    }
                }
                for (int r = top; r < top + lines; r++) {
                    for (int c = left; c <= right; c++) {
                        uint32_t a = video.cellAddr(r, c);
                        cpu.memory[a] = ' ';
                        cpu.memory[a + 1] = attr;
                    }
                }
            }
            return true;
        }
        case 0x08: {
            // AH=08h — Read char+attr at cursor (BH=page)
            if (video.active) {
                uint32_t a = video.cellAddr(video.cursor_row, video.cursor_col);
                uint8_t ch = cpu.memory[a];
                uint8_t at = cpu.memory[a + 1];
                cpu.regs[R_AX] = ((uint16_t)at << 8) | ch;
            } else {
                cpu.regs[R_AX] = 0x0720; // space with normal attr
            }
            return true;
        }
        case 0x09: {
            // AH=09h — Write char+attr at cursor (AL=char, BL=attr, CX=count)
            if (!video.active) return true;
            uint8_t ch = cpu.regs[R_AX] & 0xFF;
            uint8_t attr = cpu.regs[R_BX] & 0xFF;
            uint16_t count = cpu.regs[R_CX];
            int r = video.cursor_row, c = video.cursor_col;
            for (uint16_t i = 0; i < count && r < video.rows; i++) {
                uint32_t a = video.cellAddr(r, c);
                cpu.memory[a] = ch;
                cpu.memory[a + 1] = attr;
                c++;
                if (c >= video.cols) { c = 0; r++; }
            }
            return true;
        }
        case 0x0A: {
            // AH=0Ah — Write char only at cursor (AL=char, CX=count)
            if (!video.active) return true;
            uint8_t ch = cpu.regs[R_AX] & 0xFF;
            uint16_t count = cpu.regs[R_CX];
            int r = video.cursor_row, c = video.cursor_col;
            for (uint16_t i = 0; i < count && r < video.rows; i++) {
                uint32_t a = video.cellAddr(r, c);
                cpu.memory[a] = ch; // keep existing attribute
                c++;
                if (c >= video.cols) { c = 0; r++; }
            }
            return true;
        }
        case 0x0E: {
            // AH=0Eh — Teletype output (AL=char, BH=page, BL=fgcolor)
            uint8_t ch = cpu.regs[R_AX] & 0xFF;
            output += (char)ch;
            videoWriteChar(cpu, video, ch);
            return true;
        }
        case 0x0F: {
            // AH=0Fh — Get current video mode
            cpu.regs[R_AX] = ((uint16_t)video.cols << 8) | video.modeNumber();
            cpu.regs[R_BX] = (cpu.regs[R_BX] & 0x00FF); // BH=0 (page)
            return true;
        }
        default:
            return false;
        }
    }

    // ── INT 16h — BIOS Keyboard ──────────────────────────────────────────

    if (intNum == 0x16) {
        uint8_t ah = (cpu.regs[R_AX] >> 8) & 0xFF;

        switch (ah) {
            case 0x00: {
                if (!kbd) return false;  // no keyboard → terminate
                Keystroke key;
                if (!kbd->blockingRead(key)) return false;
                cpu.regs[R_AX] = ((uint16_t)key.scancode << 8) | key.ascii;
                return true;
            }
            case 0x01: {
                if (kbd) {
                    Keystroke key;
                    bool has_key = false;
                    kbd->poll(key, has_key);
                    if (has_key) {
                        cpu.regs[R_AX] = ((uint16_t)key.scancode << 8) | key.ascii;
                        cpu.flags &= ~F_ZF;
                    } else {
                        cpu.flags |= F_ZF;
                    }
                } else {
                    // No keyboard buffer: always report no key
                    cpu.flags |= F_ZF;
                }
                return true;
            }
            case 0x02: {
                uint8_t al = kbd ? kbd->modifiers() : 0;
                cpu.regs[R_AX] = (cpu.regs[R_AX] & 0xFF00) | al;
                return true;
            }
            default:
                return false;
        }
    }

    // ── INT 33h — Mouse Driver ─────────────────────────────────────────

    if (intNum == 0x33 && mouse) {
        uint16_t ax = cpu.regs[R_AX];

        switch (ax) {
        case 0x0000: {
            // AX=0000h — Reset mouse / detect driver
            mouse->present = true;
            mouse->visible = false;
            mouse->x = 0;
            mouse->y = 0;
            mouse->buttons = 0;
            // Default ranges for 80x25 text mode
            mouse->h_min = 0;
            mouse->h_max = (uint16_t)(video.active ? (video.cols * 8 - 1) : 639);
            mouse->v_min = 0;
            mouse->v_max = (uint16_t)(video.active ? (video.rows * 8 - 1) : 199);
            cpu.regs[R_AX] = 0xFFFF; // mouse driver installed
            cpu.regs[R_BX] = 3;      // 3 buttons
            return true;
        }
        case 0x0001: {
            // AX=0001h — Show mouse cursor
            mouse->visible = true;
            return true;
        }
        case 0x0002: {
            // AX=0002h — Hide mouse cursor
            mouse->visible = false;
            return true;
        }
        case 0x0003: {
            // AX=0003h — Get mouse position and button state
            // Advance sequential event stream: apply pending mouse event if any
            if (kbd) kbd->advanceMouseOnQuery();
            cpu.regs[R_BX] = mouse->buttons;
            cpu.regs[R_CX] = mouse->x;
            cpu.regs[R_DX] = mouse->y;
            return true;
        }
        case 0x0007: {
            // AX=0007h — Set horizontal range (CX=min, DX=max)
            mouse->h_min = cpu.regs[R_CX];
            mouse->h_max = cpu.regs[R_DX];
            // Clamp current X
            if (mouse->x < mouse->h_min) mouse->x = mouse->h_min;
            if (mouse->x > mouse->h_max) mouse->x = mouse->h_max;
            return true;
        }
        case 0x0008: {
            // AX=0008h — Set vertical range (CX=min, DX=max)
            mouse->v_min = cpu.regs[R_CX];
            mouse->v_max = cpu.regs[R_DX];
            // Clamp current Y
            if (mouse->y < mouse->v_min) mouse->y = mouse->v_min;
            if (mouse->y > mouse->v_max) mouse->y = mouse->v_max;
            return true;
        }
        default:
            return false;
        }
    }

    return false;
}
