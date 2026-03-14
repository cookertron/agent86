#pragma once
#include "cpu.h"
#include "kbd.h"
#include "dos_state.h"
#include "video.h"
#include <string>

// Handle DOS/BIOS interrupt calls
// Returns true if handled, false if unknown/unhandled
bool handleDOSInt(CPU8086& cpu, int intNum, std::string& output,
                  DosState& dos, VideoState& video, KeyboardBuffer* kbd = nullptr,
                  MouseState* mouse = nullptr);
