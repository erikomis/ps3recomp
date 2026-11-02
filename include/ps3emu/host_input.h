/*
 * ps3recomp - keyboard and mouse state handed to the host pad (cellPad.c)
 *
 * The keyboard/mouse pad reads one of these. Whoever owns the window fills
 * it in:
 *
 *   - the window host (tools/host_window, macOS): the window lives in the
 *     launcher process and the game has none, so the block sits in the
 *     control page both processes map (rsx_host_surface.h). The launcher
 *     writes key and mouse state, the game reads it -- no messages, no
 *     extra thread, and it survives the game's exec like the rest of the
 *     page;
 *   - the Metal backend's own window (no window host): a block in the game
 *     process that the backend's event pump fills.
 *
 * Every field is a lock-free 32-bit atomic, so the two processes can share
 * it through the mapped page.
 *
 *   keys[4]          macOS virtual key codes 0-127 held down, one bit each
 *   mouse_dx/dy      motion since the game last drained it, 1/16 px
 *                    (deltaY > 0 = down); the game exchanges it with 0
 *   wheel            wheel turns since the last drain, 1/16 lines, + = away
 *                    from the player whatever the scroll-direction setting
 *   mouse_buttons    bit n = NSEvent buttonNumber n (1 left, 2 right,
 *                    4 middle, 8 button 4, ...)
 *   game_flags       written by the game: what the window should do with
 *                    the mouse (PS3_HOST_INPUT_*)
 *   captured         written by the window: 1 while the cursor is captured
 *
 * Plain C, header only: cellPad.c, the Metal backend, the window host and the
 * tests share it.
 */
#ifndef PS3EMU_HOST_INPUT_H
#define PS3EMU_HOST_INPUT_H

#include <stdatomic.h>
#include <stdint.h>

/* game_flags */
#define PS3_HOST_INPUT_MOUSE_CAMERA  0x1u   /* the mouse turns the camera now */
#define PS3_HOST_INPUT_CAPTURE       0x2u   /* ... and wants the cursor captured on a click */
#define PS3_HOST_INPUT_ACTIVE        0x4u   /* a cellPad that reads this block is running */

typedef struct {
    _Atomic uint32_t keys[4];
    _Atomic int32_t  mouse_dx, mouse_dy, wheel;
    _Atomic uint32_t mouse_buttons;
    _Atomic uint32_t game_flags;
    _Atomic uint32_t captured;
    uint32_t         reserved[6];
} ps3_host_input;

_Static_assert(sizeof(ps3_host_input) == 64, "ps3_host_input layout");

/* A key went down (1) or up (0). */
static inline void ps3_host_input_key(ps3_host_input* in, unsigned vk, int down)
{
    vk &= 127u;
    if (down) atomic_fetch_or(&in->keys[vk >> 5], 1u << (vk & 31));
    else      atomic_fetch_and(&in->keys[vk >> 5], ~(1u << (vk & 31)));
}

/* Nothing held: focus went elsewhere, so no key-up will arrive. */
static inline void ps3_host_input_release_all(ps3_host_input* in)
{
    for (int i = 0; i < 4; i++) atomic_store(&in->keys[i], 0);
    atomic_store(&in->mouse_buttons, 0);
    atomic_store(&in->wheel, 0);
}

#endif /* PS3EMU_HOST_INPUT_H */
