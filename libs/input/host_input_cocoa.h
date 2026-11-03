/*
 * ps3recomp - AppKit events -> the keyboard/mouse pad's ps3_host_input block
 *
 * The window that shows the game turns its key and mouse events into the
 * block cellPad.c reads (ps3emu/host_input.h). Two windows do that and share
 * this header: the window host (tools/host_window, whose block is in the
 * control page the game maps) and the Metal backend's own window
 * (rsx_metal_backend.m, a block in the game process).
 *
 *   keys            recorded by macOS virtual key code and swallowed (an
 *                   unhandled key beeps); a key typed with Cmd held is the
 *                   menu's and goes on to AppKit untouched. Shift, Control
 *                   and Option are keys too, from their flag changes.
 *   mouse camera    when the game asks for it (game_flags): with capture on,
 *                   a left click in the game captures the cursor (that click
 *                   presses nothing) and motion, buttons and the wheel count
 *                   only while captured; Esc, Cmd, another window or another
 *                   app release it. Esc still reaches the pad (START by
 *                   default: pause). With capture off, motion counts
 *                   whenever the window is key. Mouse events always go on to
 *                   AppKit (focus, the title bar).
 *
 * Main thread only. hic_event first for every event, hic_tick now and then
 * (it lets go of what the game no longer wants and of everything when the
 * app loses focus).
 */
#ifndef PS3RECOMP_HOST_INPUT_COCOA_H
#define PS3RECOMP_HOST_INPUT_COCOA_H

#import <AppKit/AppKit.h>
#include <math.h>
#include "../../include/ps3emu/host_input.h"

typedef struct {
    ps3_host_input* in;
    int             captured;
    double          cap_since;   /* motion from before the capture is not the game's */
} HostInputCocoa;

static inline void hic_release(HostInputCocoa* s)
{
    if (!s->captured) return;
    s->captured = 0;
    CGAssociateMouseAndMouseCursorPosition(true);
    [NSCursor unhide];
    atomic_store(&s->in->mouse_buttons, 0);
    atomic_store(&s->in->captured, 0);
}

static inline void hic_capture(HostInputCocoa* s, NSWindow* w)
{
    if (s->captured || !w) return;
    /* Park the cursor in the middle of the game, so it comes back there and
     * not wherever the motion would have carried it. CG's global space has
     * its origin at the top left of the primary screen, AppKit's at the
     * bottom left. */
    const NSRect r = [w convertRectToScreen:[[w contentView] frame]];
    NSArray<NSScreen*>* screens = [NSScreen screens];
    if (screens.count) {
        const CGFloat top = NSMaxY(screens[0].frame);
        CGWarpMouseCursorPosition(CGPointMake(NSMidX(r), top - NSMidY(r)));
    }
    CGAssociateMouseAndMouseCursorPosition(false);
    [NSCursor hide];
    s->captured  = 1;
    s->cap_since = [[NSProcessInfo processInfo] systemUptime] + 0.05;
    atomic_store(&s->in->mouse_dx, 0);
    atomic_store(&s->in->mouse_dy, 0);
    atomic_store(&s->in->captured, 1);
}

/* Focus went elsewhere: no key-up or mouse-up will arrive. */
static inline void hic_focus_lost(HostInputCocoa* s)
{
    ps3_host_input_release_all(s->in);
    hic_release(s);
}

static inline void hic_tick(HostInputCocoa* s)
{
    const uint32_t f = atomic_load(&s->in->game_flags);
    const uint32_t want = PS3_HOST_INPUT_MOUSE_CAMERA | PS3_HOST_INPUT_CAPTURE;
    if (s->captured && (f & want) != want) hic_release(s);
    if (![NSApp isActive]) hic_focus_lost(s);
}

/* 1 = the event was the pad's (swallow it), 0 = pass it on to AppKit. */
static inline int hic_event(HostInputCocoa* s, NSEvent* ev, NSWindow* w)
{
    ps3_host_input* in = s->in;
    const NSEventType t = [ev type];
    const uint32_t f = atomic_load(&in->game_flags);
    const int camera = (f & PS3_HOST_INPUT_MOUSE_CAMERA) != 0;
    const int cap_on = (f & PS3_HOST_INPUT_CAPTURE) != 0;
    const int key_win = w && [w isKeyWindow];

    switch (t) {
    case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged: {
        const int take = camera && (cap_on ? (s->captured && [ev timestamp] >= s->cap_since) : key_win);
        if (take) {
            atomic_fetch_add(&in->mouse_dx, (int32_t)lround([ev deltaX] * 16.0));
            atomic_fetch_add(&in->mouse_dy, (int32_t)lround([ev deltaY] * 16.0));
        }
        return 0;
    }
    case NSEventTypeLeftMouseDown:  case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseDown: case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseDown: case NSEventTypeOtherMouseUp: {
        uint32_t bit = 1u;
        if (t == NSEventTypeRightMouseDown || t == NSEventTypeRightMouseUp) bit = 2u;
        if (t == NSEventTypeOtherMouseDown || t == NSEventTypeOtherMouseUp) {
            const NSInteger n = [ev buttonNumber];   /* middle = 4, button 4 = 8, ... */
            bit = 1u << (n < 2 ? 2 : n > 7 ? 7 : n);
        }
        const int down = (t == NSEventTypeLeftMouseDown || t == NSEventTypeRightMouseDown ||
                          t == NSEventTypeOtherMouseDown);
        if (!down) { atomic_fetch_and(&in->mouse_buttons, ~bit); return 0; }
        if (!camera || !w || [ev window] != w) return 0;
        const int inside = NSPointInRect([ev locationInWindow], [[w contentView] frame]);
        if (!cap_on) {
            if (inside && key_win) atomic_fetch_or(&in->mouse_buttons, bit);
        } else if (s->captured) {
            atomic_fetch_or(&in->mouse_buttons, bit);
        } else if (inside && bit == 1u) {
            hic_capture(s, w);
        }
        return 0;
    }
    case NSEventTypeScrollWheel:
        if (camera && (cap_on ? s->captured : ([ev window] == w && key_win))) {
            const double dy = [ev isDirectionInvertedFromDevice] ? -[ev deltaY] : [ev deltaY];
            atomic_fetch_add(&in->wheel, (int32_t)lround(dy * 16.0));
        }
        return 0;
    case NSEventTypeFlagsChanged: {
        /* Cmd releases the mouse, so Cmd+Tab and the rest of the system's
         * shortcuts find a cursor. */
        if (s->captured && ([ev modifierFlags] & NSEventModifierFlagCommand)) hic_release(s);
        /* the device-dependent bits tell left from right */
        static const struct { unsigned short vk; NSUInteger bit; } mods[] = {
            {0x38, 0x02}, {0x3C, 0x04},       /* Shift, right Shift */
            {0x3B, 0x01}, {0x3E, 0x2000},     /* Control, right Control */
            {0x3A, 0x20}, {0x3D, 0x40},       /* Option, right Option */
        };
        const unsigned vk = [ev keyCode] & 127u;
        for (unsigned i = 0; i < sizeof mods / sizeof mods[0]; i++)
            if (mods[i].vk == vk) ps3_host_input_key(in, vk, ([ev modifierFlags] & mods[i].bit) != 0);
        return 0;
    }
    case NSEventTypeKeyDown: {
        if ([ev modifierFlags] & NSEventModifierFlagCommand) return 0;   /* the menu's */
        const unsigned vk = [ev keyCode] & 127u;
        if (vk == 0x35) hic_release(s);                                   /* Esc */
        ps3_host_input_key(in, vk, 1);
        return 1;
    }
    case NSEventTypeKeyUp:
        ps3_host_input_key(in, [ev keyCode] & 127u, 0);
        return 1;
    default:
        return 0;
    }
}

#endif /* PS3RECOMP_HOST_INPUT_COCOA_H */
