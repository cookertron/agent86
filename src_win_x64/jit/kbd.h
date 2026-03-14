#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct MouseState;  // forward-declare (defined in video.h)

struct Keystroke {
    uint8_t ascii;
    uint8_t scancode;
    uint8_t modifiers;
};

struct KeyEvent {
    enum TriggerKind { TRIGGER_READ, TRIGGER_POLL };
    TriggerKind trigger;
    uint32_t count;       // 1-based trigger count
    std::string keys;     // raw key string (with \S \C \A escapes)
};

struct MouseEvent {
    uint16_t buttons = 0;
    uint16_t x = 0;
    uint16_t y = 0;
};

struct InputEvent {
    enum Kind { KIND_KEYS, KIND_MOUSE };
    Kind kind;
    std::string keys;     // valid when kind == KIND_KEYS
    MouseEvent mouse;     // valid when kind == KIND_MOUSE
};

class KeyboardBuffer {
public:
    void setEvents(std::vector<KeyEvent> triggered,
                   std::vector<InputEvent> sequential,
                   MouseState* mouse);
    // AH=00h: increment read counter, fire events, dequeue one key.
    // Returns false if buffer empty after event injection.
    bool blockingRead(Keystroke& out);
    // AH=01h: increment poll counter, fire events, peek front.
    // Sets has_key=true and out if buffer non-empty.
    bool poll(Keystroke& out, bool& has_key);
    // AH=02h: current modifier state
    uint8_t modifiers() const;
    uint32_t readCount() const { return read_count_; }
    // Called from INT 33h AX=0003h: if cursor points to a mouse event,
    // apply it and advance, then inject any following keys batch.
    void advanceMouseOnQuery();
    // Extended key pending byte (for INT 21h AH=06h two-byte protocol)
    bool hasPendingExtended() const { return has_pending_ext_; }
    uint8_t consumePendingExtended() { has_pending_ext_ = false; return pending_ext_byte_; }
    void setPendingExtended(uint8_t b) { has_pending_ext_ = true; pending_ext_byte_ = b; }

private:
    std::deque<Keystroke> buffer_;
    std::vector<KeyEvent> triggered_events_;
    std::vector<InputEvent> seq_events_;
    size_t seq_cursor_ = 0;
    MouseState* mouse_ = nullptr;
    uint32_t read_count_ = 0;
    uint32_t poll_count_ = 0;
    uint8_t modifiers_ = 0;

    bool has_pending_ext_ = false;
    uint8_t pending_ext_byte_ = 0;

    static uint8_t asciiToScancode(uint8_t ascii);
    void injectKeys(const std::string& keys);
    void advanceSequential();
};
