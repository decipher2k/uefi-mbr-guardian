/*
Copyright 2026 Dennis Michael Heine

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied
See the License for the specific language governing permissions and
limitations under the License
*/

/*
 * gfx.c - Graphics subsystem implementation for MBR Guardian
 */

#include "gfx.h"
#include "font8x16.h"

/* gnu-efi uses 'SimplePointerProtocol' without the gEfi...Guid naming */
#ifndef gEfiSimplePointerProtocolGuid
extern EFI_GUID SimplePointerProtocol;
#define gEfiSimplePointerProtocolGuid SimplePointerProtocol
#endif

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */

GFX_CONTEXT gGfx;
MOUSE_STATE gMouse;

static EFI_BOOT_SERVICES               *gBS_gfx;
static EFI_SYSTEM_TABLE                 *gST_gfx;
static EFI_SIMPLE_POINTER_PROTOCOL     *gPointer = NULL;
static COLOR                           *cursor_save = NULL; /* Under-cursor backup */
static INT32                            cursor_save_x = 0, cursor_save_y = 0;
static BOOLEAN                          cursor_saved = FALSE;

/* Cached background gradient — computed once, blitted each frame. */
static COLOR                           *bg_cache = NULL;
static BOOLEAN                          bg_cache_valid = FALSE;

/* Dirty row tracking — only flip rows that were actually drawn. */
static INT32                            dirty_y0, dirty_y1;
static INT32                            prev_dirty_y0, prev_dirty_y1;

static inline void mark_dirty(INT32 y0, INT32 y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > (INT32)gGfx.screen_h) y1 = (INT32)gGfx.screen_h;
    if (y0 < dirty_y0) dirty_y0 = y0;
    if (y1 > dirty_y1) dirty_y1 = y1;
}

/* Fast 32-bit fill: writes two pixels at a time using 64-bit stores. */
static inline void
fill_row(COLOR *dst, UINT32 count, COLOR c)
{
    UINT64 pair = ((UINT64)c << 32) | (UINT64)c;
    UINT64 *d64 = (UINT64 *)dst;
    UINT32 pairs = count >> 1;
    for (UINT32 i = 0; i < pairs; i++)
        d64[i] = pair;
    if (count & 1)
        dst[count - 1] = c;
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                     */
/* ------------------------------------------------------------------ */

EFI_STATUS
gfx_init(EFI_SYSTEM_TABLE *st, EFI_BOOT_SERVICES *bs)
{
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    UINTN siz = 0;
    EFI_HANDLE *handles = NULL;

    gBS_gfx = bs;
    gST_gfx = st;

    /* Prefer GOP bound to the active console output. Some firmware exposes
       multiple GOP handles and index 0 may not be the visible display. */
    status = bs->HandleProtocol(st->ConsoleOutHandle,
                                &gEfiGraphicsOutputProtocolGuid,
                                (void **)&gop);

    if (EFI_ERROR(status)) {
        status = bs->LocateHandleBuffer(
            ByProtocol, &gEfiGraphicsOutputProtocolGuid,
            NULL, &siz, &handles
        );
        if (EFI_ERROR(status) || siz == 0) return EFI_NOT_FOUND;

        gop = NULL;
        for (UINTN i = 0; i < siz; i++) {
            status = bs->HandleProtocol(handles[i],
                                        &gEfiGraphicsOutputProtocolGuid,
                                        (void **)&gop);
            if (!EFI_ERROR(status) && gop) break;
        }
        if (handles) bs->FreePool(handles);
        if (EFI_ERROR(status) || !gop) return EFI_NOT_FOUND;
    }

    gGfx.gop = gop;

    /* Keep the firmware-selected mode for maximum compatibility.
       Mode switching causes black screens on some UEFI implementations. */

    if (gop->Mode->Info->PixelFormat == PixelBltOnly) {
        return EFI_UNSUPPORTED;
    }

    gGfx.screen_w = gop->Mode->Info->HorizontalResolution;
    gGfx.screen_h = gop->Mode->Info->VerticalResolution;
    gGfx.stride   = gop->Mode->Info->PixelsPerScanLine;
    gGfx.fb_size  = gop->Mode->FrameBufferSize;
    gGfx.screen   = (COLOR *)(UINTN)gop->Mode->FrameBufferBase;

    /* Allocate back buffer */
    UINTN fb_size = gGfx.stride * gGfx.screen_h * sizeof(COLOR);
    status = bs->AllocatePool(EfiLoaderData, fb_size, (void **)&gGfx.framebuf);
    if (EFI_ERROR(status)) return status;

    /* Allocate background gradient cache */
    status = bs->AllocatePool(EfiLoaderData, fb_size, (void **)&bg_cache);
    if (EFI_ERROR(status)) return status;
    bg_cache_valid = FALSE;

    /* Init dirty tracking to full-screen for first frame */
    dirty_y0 = 0;
    dirty_y1 = (INT32)gGfx.screen_h;
    prev_dirty_y0 = 0;
    prev_dirty_y1 = (INT32)gGfx.screen_h;

    /* Allocate cursor save area */
    status = bs->AllocatePool(EfiLoaderData,
        CURSOR_SPRITE_W * CURSOR_SPRITE_H * sizeof(COLOR),
        (void **)&cursor_save);
    if (EFI_ERROR(status)) return status;

    /* Init mouse state */
    gMouse.x = (INT32)(gGfx.screen_w / 2);
    gMouse.y = (INT32)(gGfx.screen_h / 2);
    gMouse.prev_x = gMouse.x;
    gMouse.prev_y = gMouse.y;
    gMouse.left_click = FALSE;
    gMouse.right_click = FALSE;
    gMouse.left_pressed = FALSE;
    gMouse.right_pressed = FALSE;
    gMouse.visible = TRUE;

    /* Disable text cursor */
    st->ConOut->EnableCursor(st->ConOut, FALSE);

    return EFI_SUCCESS;
}

void
gfx_shutdown(void)
{
    if (gGfx.framebuf) gBS_gfx->FreePool(gGfx.framebuf);
    if (bg_cache)       gBS_gfx->FreePool(bg_cache);
    if (cursor_save)    gBS_gfx->FreePool(cursor_save);
}

/* ------------------------------------------------------------------ */
/*  Framebuffer                                                        */
/* ------------------------------------------------------------------ */

void
gfx_begin_frame(void)
{
    prev_dirty_y0 = dirty_y0;
    prev_dirty_y1 = dirty_y1;
    dirty_y0 = (INT32)gGfx.screen_h;
    dirty_y1 = 0;
}

void
gfx_clear(COLOR c)
{
    UINT32 stride = gGfx.stride;
    UINT32 w = gGfx.screen_w;
    for (UINT32 py = 0; py < gGfx.screen_h; py++) {
        fill_row(&gGfx.framebuf[py * stride], w, c);
    }
    mark_dirty(0, (INT32)gGfx.screen_h);
}

void
gfx_cache_bg(void)
{
    UINTN size = (UINTN)gGfx.stride * gGfx.screen_h * sizeof(COLOR);
    CopyMem(bg_cache, gGfx.framebuf, size);
    bg_cache_valid = TRUE;
}

void
gfx_restore_bg(void)
{
    if (!bg_cache_valid) return;
    /* Only restore rows that were drawn on last frame */
    if (prev_dirty_y0 < prev_dirty_y1) {
        INT32 y0 = prev_dirty_y0, y1 = prev_dirty_y1;
        UINT32 stride = gGfx.stride;
        UINTN row_bytes = (UINTN)gGfx.screen_w * sizeof(COLOR);
        for (INT32 y = y0; y < y1; y++)
            CopyMem(&gGfx.framebuf[y * stride], &bg_cache[y * stride], row_bytes);
    }
}

void
gfx_flip(void)
{
    /* Compute the union of previous and current dirty rows */
    INT32 y0 = dirty_y0 < prev_dirty_y0 ? dirty_y0 : prev_dirty_y0;
    INT32 y1 = dirty_y1 > prev_dirty_y1 ? dirty_y1 : prev_dirty_y1;
    if (y0 >= y1) return; /* nothing to flip */
    if (y0 < 0) y0 = 0;
    if (y1 > (INT32)gGfx.screen_h) y1 = (INT32)gGfx.screen_h;

    if (gGfx.screen && gGfx.fb_size > 0) {
        /* Copy only dirty rows using 64-bit stores for MMIO throughput */
        UINT32 stride = gGfx.stride;
        for (INT32 y = y0; y < y1; y++) {
            UINT64 *src = (UINT64 *)&gGfx.framebuf[y * stride];
            UINT64 *dst = (UINT64 *)&gGfx.screen[y * stride];
            UINT32 qwords = (gGfx.screen_w * sizeof(COLOR)) / 8;
            for (UINT32 i = 0; i < qwords; i++)
                dst[i] = src[i];
        }
        return;
    }

    /* Blt fallback — partial region */
    gGfx.gop->Blt(
        gGfx.gop,
        (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)&gGfx.framebuf[y0 * gGfx.stride],
        EfiBltBufferToVideo,
        0, 0, 0, (UINTN)y0,
        gGfx.screen_w, (UINTN)(y1 - y0),
        gGfx.stride * sizeof(COLOR)
    );
}

void
gfx_flip_rect(INT32 x, INT32 y, UINT32 w, UINT32 h)
{
    /* Clip */
    if (x < 0) { w = (INT32)w + x > 0 ? w + (UINT32)x : 0; x = 0; }
    if (y < 0) { h = (INT32)h + y > 0 ? h + (UINT32)y : 0; y = 0; }
    if (x + (INT32)w > (INT32)gGfx.screen_w) w = gGfx.screen_w - (UINT32)x;
    if (y + (INT32)h > (INT32)gGfx.screen_h) h = gGfx.screen_h - (UINT32)y;
    if ((INT32)w <= 0 || (INT32)h <= 0) return;

    if (gGfx.screen && gGfx.fb_size > 0) {
        UINT32 stride = gGfx.stride;
        for (UINT32 row = 0; row < h; row++) {
            UINT32 off = ((UINT32)y + row) * stride + (UINT32)x;
            COLOR *src = &gGfx.framebuf[off];
            COLOR *dst = &gGfx.screen[off];
            for (UINT32 i = 0; i < w; i++)
                dst[i] = src[i];
        }
    } else {
        gGfx.gop->Blt(gGfx.gop,
            (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)&gGfx.framebuf[y * gGfx.stride + x],
            EfiBltBufferToVideo,
            0, 0, (UINTN)x, (UINTN)y, w, h,
            gGfx.stride * sizeof(COLOR));
    }
}

/* ------------------------------------------------------------------ */
/*  Drawing primitives                                                 */
/* ------------------------------------------------------------------ */

static inline void
put_pixel(INT32 x, INT32 y, COLOR c)
{
    if (x >= 0 && x < (INT32)gGfx.screen_w &&
        y >= 0 && y < (INT32)gGfx.screen_h) {
        gGfx.framebuf[y * gGfx.stride + x] = c;
        mark_dirty(y, y + 1);
    }
}

static inline COLOR
get_pixel(INT32 x, INT32 y)
{
    if (x >= 0 && x < (INT32)gGfx.screen_w &&
        y >= 0 && y < (INT32)gGfx.screen_h) {
        return gGfx.framebuf[y * gGfx.stride + x];
    }
    return 0;
}

void
gfx_pixel(INT32 x, INT32 y, COLOR c)
{
    put_pixel(x, y, c);
}

void
gfx_fill_rect(INT32 x, INT32 y, UINT32 w, UINT32 h, COLOR c)
{
    /* Clip */
    INT32 x1 = x, y1 = y;
    INT32 x2 = x + (INT32)w, y2 = y + (INT32)h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (INT32)gGfx.screen_w) x2 = (INT32)gGfx.screen_w;
    if (y2 > (INT32)gGfx.screen_h) y2 = (INT32)gGfx.screen_h;
    if (x1 >= x2 || y1 >= y2) return;

    UINT32 fill_w = (UINT32)(x2 - x1);
    for (INT32 py = y1; py < y2; py++) {
        fill_row(&gGfx.framebuf[py * gGfx.stride + x1], fill_w, c);
    }
    mark_dirty(y1, y2);
}

void
gfx_rect(INT32 x, INT32 y, UINT32 w, UINT32 h, COLOR c, UINT32 thickness)
{
    for (UINT32 t = 0; t < thickness; t++) {
        gfx_hline(x, y + t, w, c);
        gfx_hline(x, y + h - 1 - t, w, c);
        gfx_vline(x + t, y, h, c);
        gfx_vline(x + w - 1 - t, y, h, c);
    }
}

void
gfx_hline(INT32 x, INT32 y, UINT32 w, COLOR c)
{
    gfx_fill_rect(x, y, w, 1, c);
}

void
gfx_vline(INT32 x, INT32 y, UINT32 h, COLOR c)
{
    gfx_fill_rect(x, y, 1, h, c);
}

/* ------------------------------------------------------------------ */
/*  Alpha blending                                                     */
/* ------------------------------------------------------------------ */

COLOR
gfx_blend(COLOR bg, COLOR fg, UINT8 alpha)
{
    /* Approximate /255 with >>8 after +128 bias for rounding. */
    UINT32 inv = 255 - alpha;
    UINT32 r = (COLOR_R(fg) * alpha + COLOR_R(bg) * inv + 128) >> 8;
    UINT32 g = (COLOR_G(fg) * alpha + COLOR_G(bg) * inv + 128) >> 8;
    UINT32 b = (COLOR_B(fg) * alpha + COLOR_B(bg) * inv + 128) >> 8;
    return RGB(r, g, b);
}

void
gfx_fill_rect_alpha(INT32 x, INT32 y, UINT32 w, UINT32 h, COLOR c, UINT8 alpha)
{
    INT32 x1 = x < 0 ? 0 : x;
    INT32 y1 = y < 0 ? 0 : y;
    INT32 x2 = x + (INT32)w; if (x2 > (INT32)gGfx.screen_w) x2 = (INT32)gGfx.screen_w;
    INT32 y2 = y + (INT32)h; if (y2 > (INT32)gGfx.screen_h) y2 = (INT32)gGfx.screen_h;

    for (INT32 py = y1; py < y2; py++) {
        COLOR *row = &gGfx.framebuf[py * gGfx.stride];
        for (INT32 px = x1; px < x2; px++)
            row[px] = gfx_blend(row[px], c, alpha);
    }
    mark_dirty(y1, y2);
}

/* ------------------------------------------------------------------ */
/*  Gradients                                                          */
/* ------------------------------------------------------------------ */

void
gfx_gradient_v(INT32 x, INT32 y, UINT32 w, UINT32 h, COLOR top, COLOR bottom)
{
    UINT32 div = h > 1 ? h - 1 : 1;
    for (UINT32 row = 0; row < h; row++) {
        UINT32 inv = div - row;
        COLOR c = RGB(
            (COLOR_R(top) * inv + COLOR_R(bottom) * row) / div,
            (COLOR_G(top) * inv + COLOR_G(bottom) * row) / div,
            (COLOR_B(top) * inv + COLOR_B(bottom) * row) / div
        );
        gfx_fill_rect(x, y + (INT32)row, w, 1, c);
    }
}

void
gfx_gradient_h(INT32 x, INT32 y, UINT32 w, UINT32 h, COLOR left, COLOR right)
{
    for (UINT32 col = 0; col < w; col++) {
        UINT32 r = (COLOR_R(left) * (w - 1 - col) + COLOR_R(right) * col) / (w > 1 ? w - 1 : 1);
        UINT32 g = (COLOR_G(left) * (w - 1 - col) + COLOR_G(right) * col) / (w > 1 ? w - 1 : 1);
        UINT32 b = (COLOR_B(left) * (w - 1 - col) + COLOR_B(right) * col) / (w > 1 ? w - 1 : 1);
        gfx_fill_rect(x + col, y, 1, h, RGB(r, g, b));
    }
}

/* ------------------------------------------------------------------ */
/*  Rounded rectangles (filled)                                        */
/* ------------------------------------------------------------------ */

static void
fill_circle_quarter(INT32 cx, INT32 cy, UINT32 r, UINT32 quarter, COLOR c)
{
    /* Scanline-based: compute x-span for each y, then fill_row. */
    INT32 ri = (INT32)r;
    INT32 r2 = ri * ri;
    for (INT32 dy = 0; dy <= ri; dy++) {
        /* Find max dx where dx*dx + dy*dy <= r*r */
        INT32 max_dx = 0;
        while ((max_dx + 1) * (max_dx + 1) + dy * dy <= r2)
            max_dx++;

        INT32 py, x0;
        switch (quarter) {
            case 0: py = cy - dy; x0 = cx - max_dx; break;
            case 1: py = cy - dy; x0 = cx;          break;
            case 2: py = cy + dy; x0 = cx;          break;
            case 3: py = cy + dy; x0 = cx - max_dx; break;
            default: return;
        }
        UINT32 span = (UINT32)(max_dx + 1);
        /* Clip and draw scanline */
        if (py < 0 || py >= (INT32)gGfx.screen_h) continue;
        if (x0 < 0) { span += (UINT32)x0; x0 = 0; }
        if (x0 + (INT32)span > (INT32)gGfx.screen_w)
            span = (UINT32)((INT32)gGfx.screen_w - x0);
        if ((INT32)span <= 0) continue;
        fill_row(&gGfx.framebuf[py * gGfx.stride + x0], span, c);
        mark_dirty(py, py + 1);
    }
}

void
gfx_fill_rounded_rect(INT32 x, INT32 y, UINT32 w, UINT32 h, UINT32 r, COLOR c)
{
    if (r == 0 || w < r * 2 || h < r * 2) {
        gfx_fill_rect(x, y, w, h, c);
        return;
    }

    /* Center body */
    gfx_fill_rect(x + r, y, w - 2 * r, h, c);
    /* Left strip */
    gfx_fill_rect(x, y + r, r, h - 2 * r, c);
    /* Right strip */
    gfx_fill_rect(x + w - r, y + r, r, h - 2 * r, c);

    /* Corners */
    fill_circle_quarter(x + r,     y + r,     r, 0, c);
    fill_circle_quarter(x + w - r - 1, y + r,     r, 1, c);
    fill_circle_quarter(x + w - r - 1, y + h - r - 1, r, 2, c);
    fill_circle_quarter(x + r,     y + h - r - 1, r, 3, c);
}

void
gfx_rounded_rect(INT32 x, INT32 y, UINT32 w, UINT32 h, UINT32 r, COLOR c, UINT32 thickness)
{
    /* Approximate: draw outer rounded, clear inner */
    gfx_fill_rounded_rect(x, y, w, h, r, c);
    if (thickness < w / 2 && thickness < h / 2) {
        /* Use panel/bg color for inner - caller should handle or use
           fill_rounded_rect + border approach */
    }
    /* Simple approach: just draw the outer shape for now */
    /* For borders, we draw a filled rounded rect and then a smaller one on top */
}

/* ------------------------------------------------------------------ */
/*  Shadow                                                             */
/* ------------------------------------------------------------------ */

void
ui_draw_shadow(INT32 x, INT32 y, UINT32 w, UINT32 h, UINT32 spread)
{
    /* Simple 2-step shadow: avoids per-pixel alpha blending. */
    (void)spread;
    gfx_fill_rect(x + 3, y + (INT32)h, w, 3, RGB(6, 6, 8));
    gfx_fill_rect(x + (INT32)w, y + 3, 3, h, RGB(6, 6, 8));
}

/* ------------------------------------------------------------------ */
/*  Text rendering                                                     */
/* ------------------------------------------------------------------ */

void
gfx_char(INT32 x, INT32 y, CHAR16 ch, COLOR c, UINT32 scale)
{
    if (ch < FONT_FIRST || ch > FONT_LAST) return;
    const UINT8 *glyph = font8x16_data[ch - FONT_FIRST];

    for (UINT32 row = 0; row < FONT_H; row++) {
        UINT8 bits = glyph[row];
        for (UINT32 col = 0; col < FONT_W; col++) {
            if (bits & (0x80 >> col)) {
                if (scale == 1) {
                    put_pixel(x + col, y + row, c);
                } else {
                    gfx_fill_rect(x + col * scale, y + row * scale, scale, scale, c);
                }
            }
        }
    }
}

void
gfx_text(INT32 x, INT32 y, const CHAR16 *str, COLOR c, UINT32 scale)
{
    INT32 cx = x;
    while (*str) {
        if (*str == L'\n') {
            cx = x;
            y += FONT_H * scale + 2;
        } else {
            gfx_char(cx, y, *str, c, scale);
            cx += FONT_W * scale;
        }
        str++;
    }
}

void
gfx_text_centered(INT32 cx, INT32 y, const CHAR16 *str, COLOR c, UINT32 scale)
{
    UINT32 w = gfx_text_width(str, scale);
    gfx_text(cx - (INT32)(w / 2), y, str, c, scale);
}

UINT32
gfx_text_width(const CHAR16 *str, UINT32 scale)
{
    UINT32 len = 0;
    while (*str) { len++; str++; }
    return len * FONT_W * scale;
}

UINT32
gfx_text_height(UINT32 scale)
{
    return FONT_H * scale;
}

/* ------------------------------------------------------------------ */
/*  BMP Loader                                                         */
/* ------------------------------------------------------------------ */

#pragma pack(1)
typedef struct {
    UINT16 type;        /* 'BM' */
    UINT32 size;
    UINT16 reserved1;
    UINT16 reserved2;
    UINT32 offset;
    UINT32 header_size;
    INT32  width;
    INT32  height;
    UINT16 planes;
    UINT16 bpp;
    UINT32 compression;
    UINT32 image_size;
    INT32  xppm;
    INT32  yppm;
    UINT32 colors_used;
    UINT32 colors_important;
} BMP_HEADER;
#pragma pack()

ICON_IMAGE *
gfx_load_bmp(EFI_FILE_PROTOCOL *root, const CHAR16 *path)
{
    EFI_FILE_PROTOCOL *file;
    EFI_STATUS status;

    status = root->Open(root, &file, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return NULL;

    /* Read header */
    BMP_HEADER hdr;
    UINTN hdr_size = sizeof(BMP_HEADER);
    status = file->Read(file, &hdr_size, &hdr);
    if (EFI_ERROR(status) || hdr.type != 0x4D42) {
        file->Close(file);
        return NULL;
    }

    /* Only support 24-bit and 32-bit uncompressed BMPs */
    if (hdr.compression != 0 || (hdr.bpp != 24 && hdr.bpp != 32)) {
        file->Close(file);
        return NULL;
    }

    INT32 w = hdr.width;
    INT32 h = hdr.height < 0 ? -hdr.height : hdr.height;
    BOOLEAN bottom_up = (hdr.height > 0);

    /* Limit icon size */
    if (w > 256 || h > 256 || w <= 0 || h <= 0) {
        file->Close(file);
        return NULL;
    }

    /* Allocate icon */
    ICON_IMAGE *icon;
    status = gBS_gfx->AllocatePool(EfiLoaderData, sizeof(ICON_IMAGE), (void **)&icon);
    if (EFI_ERROR(status)) { file->Close(file); return NULL; }

    icon->w = (UINT32)w;
    icon->h = (UINT32)h;
    status = gBS_gfx->AllocatePool(EfiLoaderData, w * h * sizeof(COLOR), (void **)&icon->pixels);
    if (EFI_ERROR(status)) {
        gBS_gfx->FreePool(icon);
        file->Close(file);
        return NULL;
    }

    /* Seek to pixel data */
    status = file->SetPosition(file, hdr.offset);
    if (EFI_ERROR(status)) {
        gBS_gfx->FreePool(icon->pixels);
        gBS_gfx->FreePool(icon);
        file->Close(file);
        return NULL;
    }

    /* Read pixel rows */
    UINT32 row_stride = ((w * (hdr.bpp / 8) + 3) / 4) * 4; /* BMP rows are 4-byte aligned */
    UINT8 *row_buf;
    status = gBS_gfx->AllocatePool(EfiLoaderData, row_stride, (void **)&row_buf);
    if (EFI_ERROR(status)) {
        gBS_gfx->FreePool(icon->pixels);
        gBS_gfx->FreePool(icon);
        file->Close(file);
        return NULL;
    }

    for (INT32 row = 0; row < h; row++) {
        UINTN rsize = row_stride;
        file->Read(file, &rsize, row_buf);

        INT32 dest_row = bottom_up ? (h - 1 - row) : row;
        COLOR *dest = &icon->pixels[dest_row * w];

        for (INT32 col = 0; col < w; col++) {
            if (hdr.bpp == 24) {
                UINT8 b = row_buf[col * 3 + 0];
                UINT8 g = row_buf[col * 3 + 1];
                UINT8 r = row_buf[col * 3 + 2];
                dest[col] = RGB(r, g, b);
            } else { /* 32-bit */
                UINT8 b = row_buf[col * 4 + 0];
                UINT8 g = row_buf[col * 4 + 1];
                UINT8 r = row_buf[col * 4 + 2];
                /* UINT8 a = row_buf[col * 4 + 3]; */
                dest[col] = RGB(r, g, b);
            }
        }
    }

    gBS_gfx->FreePool(row_buf);
    file->Close(file);
    return icon;
}

/* ------------------------------------------------------------------ */
/*  Default icons (generated procedurally)                             */
/* ------------------------------------------------------------------ */

ICON_IMAGE *
gfx_create_default_icon(UINT8 type_id, COLOR accent)
{
    ICON_IMAGE *icon;
    EFI_STATUS status;

    status = gBS_gfx->AllocatePool(EfiLoaderData, sizeof(ICON_IMAGE), (void **)&icon);
    if (EFI_ERROR(status)) return NULL;

    icon->w = 64;
    icon->h = 64;
    status = gBS_gfx->AllocatePool(EfiLoaderData, 64 * 64 * sizeof(COLOR), (void **)&icon->pixels);
    if (EFI_ERROR(status)) { gBS_gfx->FreePool(icon); return NULL; }

    /* Fill with gradient background */
    COLOR darker = RGB(COLOR_R(accent) / 3, COLOR_G(accent) / 3, COLOR_B(accent) / 3);
    for (UINT32 y = 0; y < 64; y++) {
        for (UINT32 x = 0; x < 64; x++) {
            /* Rounded rectangle mask */
            BOOLEAN inside = TRUE;
            INT32 dx, dy, r = 10;
            if (x < (UINT32)r && y < (UINT32)r) { dx = r - x; dy = r - y; if (dx*dx + dy*dy > r*r) inside = FALSE; }
            if (x >= 64 - r && y < (UINT32)r) { dx = x - 63 + r; dy = r - y; if (dx*dx + dy*dy > r*r) inside = FALSE; }
            if (x >= 64 - r && y >= 64 - r) { dx = x - 63 + r; dy = y - 63 + r; if (dx*dx + dy*dy > r*r) inside = FALSE; }
            if (x < (UINT32)r && y >= 64 - r) { dx = r - x; dy = y - 63 + r; if (dx*dx + dy*dy > r*r) inside = FALSE; }

            if (inside) {
                UINT32 cr = (COLOR_R(accent) * (64 - y) + COLOR_R(darker) * y) / 64;
                UINT32 cg = (COLOR_G(accent) * (64 - y) + COLOR_G(darker) * y) / 64;
                UINT32 cb = (COLOR_B(accent) * (64 - y) + COLOR_B(darker) * y) / 64;
                icon->pixels[y * 64 + x] = RGB(cr, cg, cb);
            } else {
                icon->pixels[y * 64 + x] = COL_TRANSPARENT;
            }
        }
    }

    /* Draw a simple symbol based on type */
    COLOR white = RGB(255, 255, 255);
    switch (type_id) {
        case 0x07: /* NTFS - Windows logo approx */
            for (int sy = 18; sy < 46; sy++)
                for (int sx = 18; sx < 46; sx++) {
                    if ((sx < 31 && sy < 31) ||
                        (sx >= 33 && sy < 31) ||
                        (sx < 31 && sy >= 33) ||
                        (sx >= 33 && sy >= 33))
                        icon->pixels[sy * 64 + sx] = white;
                }
            break;

        case 0x83: /* Linux - penguin silhouette approx */
            for (int sy = 12; sy < 52; sy++)
                for (int sx = 20; sx < 44; sx++) {
                    INT32 dx = sx - 32, dy = sy - 30;
                    if (sy < 24) { /* Head */
                        if (dx*dx + (dy+8)*(dy+8) < 100)
                            icon->pixels[sy * 64 + sx] = white;
                    } else { /* Body */
                        INT32 bw = 10 + (sy - 24) / 3;
                        if (sx >= 32 - bw && sx <= 32 + bw)
                            icon->pixels[sy * 64 + sx] = white;
                    }
                }
            break;

        case 0xAF: /* macOS HFS+ - apple approx */
        case 0xA5: /* FreeBSD */
            /* Simple circle with bite */
            for (int sy = 14; sy < 50; sy++)
                for (int sx = 18; sx < 46; sx++) {
                    INT32 dx = sx - 32, dy = sy - 34;
                    if (dx*dx + dy*dy < 225)
                        icon->pixels[sy * 64 + sx] = white;
                }
            break;

        default: /* Generic disk icon */
            /* HDD shape */
            for (int sy = 20; sy < 44; sy++)
                for (int sx = 14; sx < 50; sx++)
                    icon->pixels[sy * 64 + sx] = white;
            /* Drive bay line */
            for (int sx = 14; sx < 50; sx++)
                icon->pixels[32 * 64 + sx] = accent;
            /* LED */
            for (int sy = 35; sy < 39; sy++)
                for (int sx = 42; sx < 46; sx++)
                    icon->pixels[sy * 64 + sx] = RGB(0, 255, 0);
            break;
    }

    return icon;
}

void
gfx_free_icon(ICON_IMAGE *icon, EFI_BOOT_SERVICES *bs)
{
    if (icon) {
        if (icon->pixels) bs->FreePool(icon->pixels);
        bs->FreePool(icon);
    }
}

void
gfx_draw_icon(INT32 x, INT32 y, const ICON_IMAGE *icon)
{
    if (!icon || !icon->pixels) return;
    for (UINT32 iy = 0; iy < icon->h; iy++) {
        for (UINT32 ix = 0; ix < icon->w; ix++) {
            COLOR c = icon->pixels[iy * icon->w + ix];
            if (c != COL_TRANSPARENT)
                put_pixel(x + ix, y + iy, c);
        }
    }
}

void
gfx_draw_icon_scaled(INT32 x, INT32 y, UINT32 tw, UINT32 th, const ICON_IMAGE *icon)
{
    if (!icon || !icon->pixels) return;
    for (UINT32 iy = 0; iy < th; iy++) {
        UINT32 sy = iy * icon->h / th;
        for (UINT32 ix = 0; ix < tw; ix++) {
            UINT32 sx = ix * icon->w / tw;
            COLOR c = icon->pixels[sy * icon->w + sx];
            if (c != COL_TRANSPARENT)
                put_pixel(x + ix, y + iy, c);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Mouse handling                                                     */
/* ------------------------------------------------------------------ */

EFI_STATUS
mouse_init(EFI_BOOT_SERVICES *bs)
{
    EFI_STATUS status;
    UINTN count = 0;
    EFI_HANDLE *handles = NULL;

    status = bs->LocateHandleBuffer(
        ByProtocol, &gEfiSimplePointerProtocolGuid,
        NULL, &count, &handles
    );
    if (EFI_ERROR(status) || count == 0) {
        gMouse.visible = FALSE;
        return EFI_NOT_FOUND;
    }

    gPointer = NULL;
    UINTN best_score = 0;

    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_POINTER_PROTOCOL *ptr = NULL;
        status = bs->HandleProtocol(
            handles[i], &gEfiSimplePointerProtocolGuid,
            (void **)&ptr
        );
        if (EFI_ERROR(status) || !ptr || !ptr->Mode) continue;

        /* Prefer higher-resolution devices, but still accept unknown (0/0). */
        UINTN score = (UINTN)ptr->Mode->ResolutionX + (UINTN)ptr->Mode->ResolutionY;
        if (!gPointer || score > best_score) {
            gPointer = ptr;
            best_score = score;
        }
    }
    if (handles) bs->FreePool(handles);

    if (!gPointer) {
        gMouse.visible = FALSE;
        return EFI_NOT_FOUND;
    }

    /* Reset pointer */
    status = gPointer->Reset(gPointer, TRUE);
    if (EFI_ERROR(status)) {
        gMouse.visible = FALSE;
        return status;
    }
    return EFI_SUCCESS;
}

void
mouse_poll(void)
{
    if (!gPointer) return;

    EFI_SIMPLE_POINTER_STATE state;
    EFI_STATUS status;
    INT64 dx_counts = 0;
    INT64 dy_counts = 0;
    BOOLEAN saw_state = FALSE;

    /* Save previous click state */
    BOOLEAN prev_left = gMouse.left_pressed;
    BOOLEAN prev_right = gMouse.right_pressed;

    /* Drain all queued pointer updates so motion stays fluid. */
    for (;;) {
        status = gPointer->GetState(gPointer, &state);
        if (EFI_ERROR(status)) break;
        saw_state = TRUE;
        dx_counts += state.RelativeMovementX;
        dy_counts += state.RelativeMovementY;
        gMouse.left_pressed = state.LeftButton;
        gMouse.right_pressed = state.RightButton;
    }

    if (saw_state) {
        /* Accumulate sub-pixel movement to avoid integer truncation. */
        static INT64 acc_x = 0, acc_y = 0;
        const INT64 max_step_px = 48;

        acc_x += dx_counts;
        acc_y += dy_counts;

        INT64 px = 0;
        INT64 py = 0;

        /* If firmware provides counts/mm, map to pixels/mm.
           If not, treat counts as raw deltas to avoid coarse jumps. */
        if (gPointer->Mode->ResolutionX > 0 || gPointer->Mode->ResolutionY > 0) {
            const INT64 target_px_per_mm = 8;

            if (gPointer->Mode->ResolutionX > 0) {
                INT64 res_x = (INT64)gPointer->Mode->ResolutionX;
                px = (acc_x * target_px_per_mm) / res_x;
                acc_x -= (px * res_x) / target_px_per_mm;
            } else {
                px = acc_x;
                acc_x = 0;
            }

            if (gPointer->Mode->ResolutionY > 0) {
                INT64 res_y = (INT64)gPointer->Mode->ResolutionY;
                py = (acc_y * target_px_per_mm) / res_y;
                acc_y -= (py * res_y) / target_px_per_mm;
            } else {
                py = acc_y;
                acc_y = 0;
            }
        } else {
            /* Unknown resolution is common in UEFI. 1:1 feels smoother than
               dividing by an arbitrary large constant. */
            px = acc_x;
            py = acc_y;
            acc_x = 0;
            acc_y = 0;
        }

        /* Clamp per-frame jump and keep remainder in accumulator for smoothness. */
        if (px > max_step_px) {
            acc_x += (px - max_step_px);
            px = max_step_px;
        } else if (px < -max_step_px) {
            acc_x += (px + max_step_px);
            px = -max_step_px;
        }

        if (py > max_step_px) {
            acc_y += (py - max_step_px);
            py = max_step_px;
        } else if (py < -max_step_px) {
            acc_y += (py + max_step_px);
            py = -max_step_px;
        }

        gMouse.x += (INT32)px;
        gMouse.y += (INT32)py;

        /* Clamp to screen */
        if (gMouse.x < 0) gMouse.x = 0;
        if (gMouse.y < 0) gMouse.y = 0;
        if (gMouse.x >= (INT32)gGfx.screen_w) gMouse.x = (INT32)gGfx.screen_w - 1;
        if (gMouse.y >= (INT32)gGfx.screen_h) gMouse.y = (INT32)gGfx.screen_h - 1;
    }

    /* Detect click (press→release) */
    gMouse.left_click = (prev_left && !gMouse.left_pressed);
    gMouse.right_click = (prev_right && !gMouse.right_pressed);
}

void
mouse_save_under(void)
{
    if (!cursor_save || !gMouse.visible) return;
    cursor_save_x = gMouse.x;
    cursor_save_y = gMouse.y;
    for (INT32 cy = 0; cy < CURSOR_SPRITE_H; cy++)
        for (INT32 cx = 0; cx < CURSOR_SPRITE_W; cx++)
            cursor_save[cy * CURSOR_SPRITE_W + cx] = get_pixel(cursor_save_x + cx, cursor_save_y + cy);
    cursor_saved = TRUE;
}

void
mouse_restore_under(void)
{
    if (!cursor_saved || !cursor_save) return;
    for (INT32 cy = 0; cy < CURSOR_SPRITE_H; cy++)
        for (INT32 cx = 0; cx < CURSOR_SPRITE_W; cx++) {
            INT32 px = cursor_save_x + cx, py = cursor_save_y + cy;
            if (px >= 0 && px < (INT32)gGfx.screen_w &&
                py >= 0 && py < (INT32)gGfx.screen_h)
                gGfx.framebuf[py * gGfx.stride + px] = cursor_save[cy * CURSOR_SPRITE_W + cx];
        }
    cursor_saved = FALSE;
}

void
mouse_draw(void)
{
    if (!gMouse.visible) return;

    for (INT32 cy = 0; cy < CURSOR_SPRITE_H; cy++) {
        for (INT32 cx = 0; cx < CURSOR_SPRITE_W; cx++) {
            UINT8 val = cursor_sprite[cy][cx];
            if (val == 0) continue;
            COLOR c = (val == 1) ? RGB(0, 0, 0) : RGB(255, 255, 255);
            put_pixel(gMouse.x + cx, gMouse.y + cy, c);
        }
    }
}

void
mouse_update_cursor(void)
{
    if (!gMouse.visible) return;
    /* Erase old cursor from framebuf, push to screen */
    mouse_restore_under();
    gfx_flip_rect(cursor_save_x, cursor_save_y, CURSOR_SPRITE_W, CURSOR_SPRITE_H);
    /* Draw new cursor */
    mouse_save_under();
    mouse_draw();
    gfx_flip_rect(gMouse.x, gMouse.y, CURSOR_SPRITE_W, CURSOR_SPRITE_H);
}

BOOLEAN
mouse_in_rect(const RECT *r)
{
    return (gMouse.x >= r->x && gMouse.x < r->x + (INT32)r->w &&
            gMouse.y >= r->y && gMouse.y < r->y + (INT32)r->h);
}

BOOLEAN
mouse_clicked_rect(const RECT *r)
{
    return gMouse.left_click && mouse_in_rect(r);
}

BOOLEAN
mouse_rclicked_rect(const RECT *r)
{
    return gMouse.right_click && mouse_in_rect(r);
}

/* ------------------------------------------------------------------ */
/*  UI Widgets                                                         */
/* ------------------------------------------------------------------ */

void
ui_draw_tile(TILE *tile)
{
    RECT *b = &tile->bounds;

    /* Shadow */
    ui_draw_shadow(b->x, b->y, b->w, b->h, 6);

    /* Background */
    COLOR bg = COL_PANEL;
    if (tile->selected) bg = COL_PANEL_SELECTED;
    else if (tile->hover) bg = COL_PANEL_HOVER;

    gfx_fill_rounded_rect(b->x, b->y, b->w, b->h, TILE_CORNER_R, bg);

    /* Border */
    if (tile->selected) {
        /* Bright accent border for selected */
        gfx_fill_rounded_rect(b->x - 2, b->y - 2, b->w + 4, b->h + 4,
                              TILE_CORNER_R + 1, COL_ACCENT);
        gfx_fill_rounded_rect(b->x, b->y, b->w, b->h, TILE_CORNER_R, bg);
    } else if (tile->hover) {
        gfx_fill_rounded_rect(b->x - 1, b->y - 1, b->w + 2, b->h + 2,
                              TILE_CORNER_R + 1, COL_PANEL_BORDER);
        gfx_fill_rounded_rect(b->x, b->y, b->w, b->h, TILE_CORNER_R, bg);
    }

    /* Icon (centered horizontally) */
    INT32 icon_x = b->x + (INT32)(b->w - TILE_ICON_SZ) / 2;
    INT32 icon_y = b->y + 16;

    if (tile->icon) {
        gfx_draw_icon_scaled(icon_x, icon_y, TILE_ICON_SZ, TILE_ICON_SZ, tile->icon);
    } else {
        /* Placeholder */
        gfx_fill_rounded_rect(icon_x, icon_y, TILE_ICON_SZ, TILE_ICON_SZ, 8, COL_BG_LIGHT);
        gfx_text_centered(b->x + b->w / 2, icon_y + 22, L"?", COL_TEXT_DIM, 2);
    }

    /* Label */
    INT32 label_y = icon_y + TILE_ICON_SZ + 10;
    UINT32 tw = gfx_text_width(tile->label, 1);
    INT32 label_x = b->x + (INT32)(b->w - tw) / 2;
    if (label_x < b->x + 4) label_x = b->x + 4;
    gfx_text(label_x, label_y, tile->label, COL_TEXT, 1);

    /* Sublabel */
    if (tile->sublabel[0]) {
        INT32 sub_y = label_y + 18;
        UINT32 sw = gfx_text_width(tile->sublabel, 1);
        INT32 sub_x = b->x + (INT32)(b->w - sw) / 2;
        if (sub_x < b->x + 4) sub_x = b->x + 4;
        gfx_text(sub_x, sub_y, tile->sublabel, COL_TEXT_DIM, 1);
    }
}

void
ui_draw_button(BUTTON *btn)
{
    RECT *b = &btn->bounds;
    COLOR bg = btn->active ? COL_BUTTON_ACTIVE :
               btn->hover  ? COL_BUTTON_HOVER : COL_BUTTON;

    if (!btn->enabled) bg = COL_BG_LIGHT;

    COLOR fg_color = btn->color ? btn->color : COL_ACCENT;
    if (btn->active) fg_color = COL_TEXT_BRIGHT;

    gfx_fill_rounded_rect(b->x, b->y, b->w, b->h, 6, bg);

    /* Text centered in button */
    COLOR tc = btn->enabled ? COL_TEXT : COL_TEXT_DIM;
    if (btn->active) tc = COL_TEXT_BRIGHT;
    gfx_text_centered(b->x + b->w / 2, b->y + (b->h - FONT_H) / 2, btn->label, tc, 1);

    /* Color accent line at top of active button */
    if (btn->active) {
        gfx_fill_rect(b->x + 4, b->y, b->w - 8, 2, fg_color);
    }
}

void
ui_draw_titlebar(const CHAR16 *title, UINT32 snap_count, UINT32 disk_count)
{
    /* Title bar background */
    gfx_gradient_v(0, 0, gGfx.screen_w, TITLE_BAR_H, COL_TITLE_BAR, COL_BG_DARK);

    /* Accent line at bottom */
    gfx_fill_rect(0, TITLE_BAR_H - 1, gGfx.screen_w, 1, COL_ACCENT_DIM);

    /* Title text */
    gfx_text(20, (TITLE_BAR_H - 32) / 2, title, COL_ACCENT_BRIGHT, 2);

    /* Status on right */
    CHAR16 status[80];
    CHAR16 *p = status;

    /* Build status string manually */
    const CHAR16 *s1 = L"Disks: ";
    while (*s1) *p++ = *s1++;
    if (disk_count < 10) *p++ = L'0' + (CHAR16)disk_count;
    else { *p++ = L'0' + (CHAR16)(disk_count / 10); *p++ = L'0' + (CHAR16)(disk_count % 10); }

    const CHAR16 *s2 = L"  |  Snapshots: ";
    while (*s2) *p++ = *s2++;
    if (snap_count < 10) *p++ = L'0' + (CHAR16)snap_count;
    else { *p++ = L'0' + (CHAR16)(snap_count / 10); *p++ = L'0' + (CHAR16)(snap_count % 10); }
    *p = L'\0';

    UINT32 sw = gfx_text_width(status, 1);
    gfx_text((INT32)(gGfx.screen_w - sw - 20), (TITLE_BAR_H - FONT_H) / 2,
             status, COL_TEXT_DIM, 1);
}

void
ui_draw_toolbar(BUTTON *buttons, UINTN count)
{
    INT32 ty = (INT32)(gGfx.screen_h - TOOLBAR_H);

    /* Toolbar background */
    gfx_fill_rect(0, ty, gGfx.screen_w, TOOLBAR_H, COL_TOOLBAR);
    gfx_fill_rect(0, ty, gGfx.screen_w, 1, COL_DIVIDER);

    /* Layout buttons centered */
    UINT32 total_w = 0;
    for (UINTN i = 0; i < count; i++)
        total_w += buttons[i].bounds.w + 8;
    total_w -= 8; /* Remove last gap */

    INT32 bx = ((INT32)gGfx.screen_w - (INT32)total_w) / 2;
    INT32 by = ty + (TOOLBAR_H - BTN_H) / 2;

    for (UINTN i = 0; i < count; i++) {
        buttons[i].bounds.x = bx;
        buttons[i].bounds.y = by;
        buttons[i].bounds.h = BTN_H;

        /* Update hover state */
        buttons[i].hover = mouse_in_rect(&buttons[i].bounds);

        ui_draw_button(&buttons[i]);
        bx += (INT32)buttons[i].bounds.w + 8;
    }
}

void
ui_draw_statusbar(const CHAR16 *msg)
{
    INT32 y = (INT32)(gGfx.screen_h - TOOLBAR_H - 24);
    gfx_fill_rect(0, y, gGfx.screen_w, 24, COL_BG_DARK);
    gfx_text(20, y + 4, msg, COL_TEXT_DIM, 1);
}

/* ------------------------------------------------------------------ */
/*  Dialog boxes                                                       */
/* ------------------------------------------------------------------ */

static void
draw_dialog_frame(INT32 dx, INT32 dy, UINT32 dw, UINT32 dh, const CHAR16 *title)
{
    /* Dim background */
    gfx_fill_rect_alpha(0, 0, gGfx.screen_w, gGfx.screen_h, COL_SHADOW, 160);

    /* Shadow */
    ui_draw_shadow(dx, dy, dw, dh, 12);

    /* Dialog body */
    gfx_fill_rounded_rect(dx, dy, dw, dh, 10, COL_PANEL);

    /* Title bar */
    gfx_fill_rounded_rect(dx, dy, dw, 40, 10, COL_ACCENT_DIM);
    gfx_fill_rect(dx, dy + 30, dw, 10, COL_ACCENT_DIM);
    gfx_text_centered(dx + dw / 2, dy + 10, title, COL_TEXT_BRIGHT, 1);

    /* Divider */
    gfx_hline(dx, dy + 40, dw, COL_DIVIDER);
}

UINTN
ui_dialog_yesno(const CHAR16 *title, const CHAR16 *message)
{
    UINT32 dw = 420, dh = 180;
    INT32 dx = ((INT32)gGfx.screen_w - (INT32)dw) / 2;
    INT32 dy = ((INT32)gGfx.screen_h - (INT32)dh) / 2;

    BUTTON btn_yes = { .bounds = {dx + dw/2 - 140, dy + dh - 50, 120, 36},
                       .label = L"Yes", .id = 1, .hover = FALSE,
                       .active = FALSE, .enabled = TRUE, .color = COL_GREEN };
    BUTTON btn_no  = { .bounds = {dx + dw/2 + 20, dy + dh - 50, 120, 36},
                       .label = L"No", .id = 0, .hover = FALSE,
                       .active = FALSE, .enabled = TRUE, .color = COL_RED };

    for (;;) {
        mouse_poll();

        draw_dialog_frame(dx, dy, dw, dh, title);

        /* Message */
        gfx_text_centered(dx + dw / 2, dy + 70, message, COL_TEXT, 1);

        /* Buttons */
        btn_yes.hover = mouse_in_rect(&btn_yes.bounds);
        btn_no.hover  = mouse_in_rect(&btn_no.bounds);

        ui_draw_button(&btn_yes);
        ui_draw_button(&btn_no);

        mouse_draw();
        gfx_flip();

        if (mouse_clicked_rect(&btn_yes.bounds)) return 1;
        if (mouse_clicked_rect(&btn_no.bounds))  return 0;

        /* Keyboard */
        EFI_INPUT_KEY key;
        EFI_STATUS ks = gST_gfx->ConIn->ReadKeyStroke(gST_gfx->ConIn, &key);
        if (!EFI_ERROR(ks)) {
            if (key.UnicodeChar == L'\r' || key.UnicodeChar == L'y' || key.UnicodeChar == L'Y')
                return 1;
            if (key.ScanCode == 0x17 || key.UnicodeChar == L'n' || key.UnicodeChar == L'N') /* ESC */
                return 0;
        }
    }
}

void
ui_dialog_info(const CHAR16 *title, const CHAR16 *message)
{
    UINT32 dw = 420, dh = 160;
    INT32 dx = ((INT32)gGfx.screen_w - (INT32)dw) / 2;
    INT32 dy = ((INT32)gGfx.screen_h - (INT32)dh) / 2;

    BUTTON btn_ok = { .bounds = {dx + dw/2 - 60, dy + dh - 50, 120, 36},
                      .label = L"OK", .id = 0, .hover = FALSE,
                      .active = FALSE, .enabled = TRUE, .color = COL_ACCENT };

    for (;;) {
        mouse_poll();

        draw_dialog_frame(dx, dy, dw, dh, title);
        gfx_text_centered(dx + dw / 2, dy + 65, message, COL_TEXT, 1);

        btn_ok.hover = mouse_in_rect(&btn_ok.bounds);
        ui_draw_button(&btn_ok);

        mouse_draw();
        gfx_flip();

        if (mouse_clicked_rect(&btn_ok.bounds)) return;
        if (gMouse.left_click) return; /* Click anywhere */
    }
}

void
ui_dialog_input(const CHAR16 *prompt, CHAR16 *buf, UINTN max)
{
    UINT32 dw = 500, dh = 180;
    INT32 dx = ((INT32)gGfx.screen_w - (INT32)dw) / 2;
    INT32 dy = ((INT32)gGfx.screen_h - (INT32)dh) / 2;

    UINTN pos = 0;
    buf[0] = L'\0';

    /* Find existing text length */
    while (buf[pos] && pos < max - 1) pos++;

    BUTTON btn_ok = { .bounds = {dx + dw/2 - 60, dy + dh - 50, 120, 36},
                      .label = L"OK", .id = 0, .hover = FALSE,
                      .active = FALSE, .enabled = TRUE, .color = COL_GREEN };

    for (;;) {
        mouse_poll();

        draw_dialog_frame(dx, dy, dw, dh, prompt);

        /* Input field */
        INT32 field_x = dx + 20;
        INT32 field_y = dy + 65;
        UINT32 field_w = dw - 40;
        UINT32 field_h = 32;

        gfx_fill_rounded_rect(field_x, field_y, field_w, field_h, 4, COL_BG_DARK);
        gfx_text(field_x + 8, field_y + 8, buf, COL_TEXT_BRIGHT, 1);

        /* Cursor blink */
        UINT32 cur_x = field_x + 8 + gfx_text_width(buf, 1);
        gfx_fill_rect(cur_x, field_y + 6, 2, 20, COL_ACCENT);

        btn_ok.hover = mouse_in_rect(&btn_ok.bounds);
        ui_draw_button(&btn_ok);

        mouse_draw();
        gfx_flip();

        if (mouse_clicked_rect(&btn_ok.bounds)) return;

        /* Keyboard input - need to check for key events */
        /* This will be polled from the main application */
    }
}

/* File selection dialog */
UINTN
ui_dialog_file_select(EFI_FILE_PROTOCOL *root, const CHAR16 *dir,
                      const CHAR16 *filter, CHAR16 *selected, UINTN max)
{
    (void)root; (void)dir; (void)filter; (void)selected; (void)max;
    /* Placeholder - full implementation in main.c */
    return 0;
}
