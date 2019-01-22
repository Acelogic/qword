#include <stdint.h>
#include <stddef.h>
#include <lib/klib.h>
#include <sys/apic.h>
#include <misc/tty.h>
#include <lib/lock.h>

#define MAX_CODE 0x57
#define CAPSLOCK 0x3a
#define RIGHT_SHIFT 0x36
#define LEFT_SHIFT 0x2a
#define RIGHT_SHIFT_REL 0xb6
#define LEFT_SHIFT_REL 0xaa
#define LEFT_CTRL 0x1d
#define LEFT_CTRL_REL 0x9d
#define KBD_BUF_SIZE 2048
#define BIG_BUF_SIZE 65536

static size_t kbd_buf_i = 0;
static char kbd_buf[KBD_BUF_SIZE];

static size_t big_buf_i = 0;
static char big_buf[BIG_BUF_SIZE];

static int capslock_active = 0;
static int shift_active = 0;
static int ctrl_active = 0;

static const char ascii_capslock[] = {
    '\0', '?', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '[', ']', '\n', '\0', 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ';', '\'', '`', '\0', '\\', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', ',', '.', '/', '\0', '\0', '\0', ' '
};

static const char ascii_shift[] = {
    '\0', '?', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', '\0', 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', '\0', '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', '\0', '\0', '\0', ' '
};

static const char ascii_shift_capslock[] = {
    '\0', '?', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '{', '}', '\n', '\0', 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ':', '"', '~', '\0', '|', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', '<', '>', '?', '\0', '\0', '\0', ' '
};

static const char ascii_nomod[] = {
    '\0', '?', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', '\0', 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', '\0', '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', '\0', '\0', '\0', ' '
};

void init_kbd(void) {
    io_apic_set_mask(0, 1, 1);
    return;
}

static lock_t kbd_read_lock = 1;

int kbd_read(char *buf, size_t count) {
    int wait = 1;

    spinlock_acquire(&termios_lock);
    if (!(termios.c_lflag & ICANON)) {
        spinlock_release(&termios_lock);
try_again:
        while (!spinlock_test_and_acquire(&kbd_read_lock))
            yield(10);
        if (!kbd_buf_i) {
            spinlock_release(&kbd_read_lock);
            yield(10);
            goto try_again;
        }
        size_t i = 0;
        for (; i < kbd_buf_i; i++) {
            buf[i] = kbd_buf[i];
            kbd_buf[i] = '\0';
        }
        kbd_buf_i = 0;
        spinlock_release(&kbd_read_lock);
        return i;
    }
    spinlock_release(&termios_lock);

    while (!spinlock_test_and_acquire(&kbd_read_lock)) {
        yield(10);
    }
    for (size_t i = 0; i < count; ) {
        if (big_buf_i) {
            buf[i++] = big_buf[0];
            big_buf_i--;
            for (size_t j = 0; j < big_buf_i; j++) {
                big_buf[j] = big_buf[j+1];
            }
            wait = 0;
        } else {
            if (wait) {
                spinlock_release(&kbd_read_lock);
                yield(10);
                while (!spinlock_test_and_acquire(&kbd_read_lock)) {
                    yield(10);
                }
            } else {
                spinlock_release(&kbd_read_lock);
                return (int)i;
            }
        }
    }

    spinlock_release(&kbd_read_lock);
    return (int)count;
}

void kbd_handler(uint8_t input_byte) {
    char c = '\0';

    spinlock_acquire(&kbd_read_lock);

    if (ctrl_active) {
        switch (input_byte) {
            case 0x2e:
                goto out;
            default:
                break;
        }
    }

    /* Update modifiers */
    if (input_byte == CAPSLOCK) {
        /* TODO LED stuff */
        capslock_active = !capslock_active;
        goto out;
    } else if (input_byte == LEFT_SHIFT || input_byte == RIGHT_SHIFT || input_byte == LEFT_SHIFT_REL || input_byte == RIGHT_SHIFT_REL) {
        shift_active = !shift_active;
        goto out;
    } else if (input_byte == LEFT_CTRL || input_byte == LEFT_CTRL_REL) {
        ctrl_active = !ctrl_active;
        goto out;
    } else {
        /* Assign the correct character for this scancode based on modifiers */
        if (input_byte < MAX_CODE) {
            if (!capslock_active && !shift_active)
                c = ascii_nomod[input_byte];
            else if (!capslock_active && shift_active)
                c = ascii_shift[input_byte];
            else if (capslock_active && shift_active)
                c = ascii_shift_capslock[input_byte];
            else
                c = ascii_capslock[input_byte];
        } else {
            goto out;
        }
    }

    spinlock_acquire(&termios_lock);
    switch (c) {
        case '\n':
            if (!(termios.c_lflag & ICANON))
                goto regular_character;
            if (kbd_buf_i == KBD_BUF_SIZE)
                break;
            kbd_buf[kbd_buf_i++] = c;
            if (termios.c_lflag & ECHO)
                tty_putchar(c);
            for (size_t i = 0; i < kbd_buf_i; i++) {
                if (big_buf_i == BIG_BUF_SIZE)
                    break;
                big_buf[big_buf_i++] = kbd_buf[i];
            }
            kbd_buf_i = 0;
            goto out;
        case '\b':
            if (!(termios.c_lflag & ICANON))
                goto regular_character;
            if (!kbd_buf_i)
                break;
            kbd_buf[--kbd_buf_i] = 0;
            if (termios.c_lflag & ECHO) {
                tty_putchar('\b');
                tty_putchar(' ');
                tty_putchar('\b');
            }
            goto out;
        default:
            break;
    }
regular_character:
    if (kbd_buf_i == KBD_BUF_SIZE)
        goto out;
    kbd_buf[kbd_buf_i++] = c;
    if (termios.c_lflag & ECHO)
        tty_putchar(c);
out:
    spinlock_release(&termios_lock);
    spinlock_release(&kbd_read_lock);
    return;
}