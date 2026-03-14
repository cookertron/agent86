#include "kbd.h"
#include "video.h"  // MouseState definition

uint8_t KeyboardBuffer::asciiToScancode(uint8_t ascii) {
    // a-z / A-Z → standard PC AT scancodes
    if (ascii >= 'a' && ascii <= 'z') {
        static const uint8_t map[] = {
            0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17, // a-i
            0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13, // j-r
            0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C        // s-z
        };
        return map[ascii - 'a'];
    }
    if (ascii >= 'A' && ascii <= 'Z')
        return asciiToScancode(ascii - 'A' + 'a');

    // 0-9
    if (ascii == '0') return 0x0B;
    if (ascii >= '1' && ascii <= '9') return (uint8_t)(0x02 + (ascii - '1'));

    switch (ascii) {
        case '\r': case '\n': return 0x1C; // Enter
        case ' ':  return 0x39;
        case 0x1B: return 0x01; // Esc
        case '\t': return 0x0F;
        case '\b': return 0x0E; // Backspace
        case '-':  return 0x0C;
        case '=':  return 0x0D;
        case '[':  return 0x1A;
        case ']':  return 0x1B;
        case ';':  return 0x27;
        case '\'': return 0x28;
        case '`':  return 0x29;
        case '\\': return 0x2B;
        case ',':  return 0x33;
        case '.':  return 0x34;
        case '/':  return 0x35;
        default:   return 0x00;
    }
}

void KeyboardBuffer::injectKeys(const std::string& keys) {
    uint8_t mods = 0;
    for (size_t i = 0; i < keys.size(); i++) {
        char ch = keys[i];
        if (ch == '\\' && i + 1 < keys.size()) {
            char next = keys[i + 1];
            if (next == 'S') { mods ^= 0x03; i++; continue; }  // toggle shift bits 0+1
            if (next == 'C') { mods ^= 0x04; i++; continue; }  // toggle ctrl bit 2
            if (next == 'A') { mods ^= 0x08; i++; continue; }  // toggle alt bit 3
            // Not a modifier escape — fall through to push '\\' as a key
        }
        uint8_t ascii = (uint8_t)ch;
        // Null byte followed by another byte = extended key pair (scancode in next byte)
        if (ascii == 0x00 && i + 1 < keys.size()) {
            uint8_t sc = (uint8_t)keys[++i];
            buffer_.push_back({0x00, sc, mods});
            continue;
        }
        uint8_t sc = asciiToScancode(ascii);
        // Ctrl+letter: real BIOS returns control code (letter - 0x40)
        if ((mods & 0x04) && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) {
            ascii = (uint8_t)((ch & 0x1F));  // toupper & subtract 0x40
        }
        // Alt+letter: real BIOS returns ascii=0x00 with the letter's scan code
        if ((mods & 0x08) && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) {
            ascii = 0x00;
        }
        buffer_.push_back({ascii, sc, mods});
    }
}

void KeyboardBuffer::advanceSequential() {
    while (seq_cursor_ < seq_events_.size()) {
        auto& ev = seq_events_[seq_cursor_];
        if (ev.kind == InputEvent::KIND_MOUSE) {
            // Mouse events are barriers — stop here, wait for INT 33h query
            break;
        }
        // KIND_KEYS: inject only when buffer is empty
        if (!buffer_.empty()) break;
        injectKeys(ev.keys);
        seq_cursor_++;
    }
}

void KeyboardBuffer::advanceMouseOnQuery() {
    // Called from INT 33h AX=0003h — consume pending mouse event and advance.
    // Only fire the barrier when the keyboard buffer is empty, ensuring all
    // keys from the preceding keys event are fully consumed first.
    if (buffer_.empty() &&
        seq_cursor_ < seq_events_.size() &&
        seq_events_[seq_cursor_].kind == InputEvent::KIND_MOUSE) {
        auto& ev = seq_events_[seq_cursor_];
        if (mouse_) {
            mouse_->buttons = ev.mouse.buttons;
            mouse_->x = ev.mouse.x;
            mouse_->y = ev.mouse.y;
        }
        seq_cursor_++;
        // After applying mouse, inject any following keys batch
        advanceSequential();
    }
}

void KeyboardBuffer::setEvents(std::vector<KeyEvent> triggered,
                               std::vector<InputEvent> sequential,
                               MouseState* mouse) {
    triggered_events_ = std::move(triggered);
    seq_events_ = std::move(sequential);
    seq_cursor_ = 0;
    mouse_ = mouse;
    read_count_ = 0;
    poll_count_ = 0;
    modifiers_ = 0;
    has_pending_ext_ = false;
    pending_ext_byte_ = 0;
    buffer_.clear();
    advanceSequential();  // prime: drain leading mouse events + first keys batch
}

bool KeyboardBuffer::blockingRead(Keystroke& out) {
    read_count_++;
    // Fire matching triggered events
    for (auto& ev : triggered_events_) {
        if (ev.trigger == KeyEvent::TRIGGER_READ && ev.count == read_count_) {
            injectKeys(ev.keys);
        }
    }
    advanceSequential();
    if (buffer_.empty()) return false;
    out = buffer_.front();
    buffer_.pop_front();
    modifiers_ = out.modifiers;
    // After consuming a key, buffer may be empty — advance to next sequential batch
    advanceSequential();
    return true;
}

bool KeyboardBuffer::poll(Keystroke& out, bool& has_key) {
    poll_count_++;
    // Fire matching triggered events
    for (auto& ev : triggered_events_) {
        if (ev.trigger == KeyEvent::TRIGGER_POLL && ev.count == poll_count_) {
            injectKeys(ev.keys);
        }
    }
    advanceSequential();
    if (buffer_.empty()) {
        has_key = false;
        return true;
    }
    out = buffer_.front();
    has_key = true;
    modifiers_ = out.modifiers;
    return true;
}

uint8_t KeyboardBuffer::modifiers() const {
    return modifiers_;
}
