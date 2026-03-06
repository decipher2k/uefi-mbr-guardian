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
 * gfx.h - Graphics subsystem for MBR Guardian
 *
 * Provides framebuffer rendering via UEFI GOP, text drawing,
 * BMP icon loading, mouse cursor, and UI widget primitives.
 */

#ifndef GFX_H
#define GFX_H

#include <efi.h>
#include <efilib.h>

/* ------------------------------------------------------------------ */
/*  Color type (32-bit BGRX as used by GOP)                            */
/* ------------------------------------------------------------------ */

typedef UINT32 COLOR;

#define RGB(r,g,b)  ((COLOR)( ((UINT32)(b)) | ((UINT32)(g)<<8) | ((UINT32)(r)<<16) ))
#define RGBA(r,g,b,a) ((COLOR)( ((UINT32)(b)) | ((UINT32)(g)<<8) | ((UINT32)(r)<<16) | ((UINT32)(a)<<24) ))

#define COLOR_R(c)  ((UINT8)((c) >> 16))
#define COLOR_G(c)  ((UINT8)((c) >> 8))
#define COLOR_B(c)  ((UINT8)((c)))

/* ------------------------------------------------------------------ */
/*  Theme colors                                                       */
/* ------------------------------------------------------------------ */

#define COL_BG_DARK         RGB(18, 18, 24)
#define COL_BG_MID          RGB(28, 30, 42)
#define COL_BG_LIGHT        RGB(40, 44, 58)
#define COL_PANEL           RGB(32, 36, 52)
#define COL_PANEL_HOVER     RGB(45, 50, 72)
#define COL_PANEL_SELECTED  RGB(55, 62, 90)
#define COL_PANEL_BORDER    RGB(60, 66, 95)
#define COL_ACCENT          RGB(80, 140, 240)
#define COL_ACCENT_BRIGHT   RGB(100, 170, 255)
#define COL_ACCENT_DIM      RGB(50, 90, 160)
#define COL_RED             RGB(220, 60, 60)
#define COL_GREEN           RGB(60, 200, 100)
#define COL_YELLOW          RGB(240, 200, 60)
#define COL_ORANGE          RGB(240, 150, 40)
#define COL_TEXT            RGB(220, 225, 240)
#define COL_TEXT_DIM        RGB(140, 148, 170)
#define COL_TEXT_BRIGHT     RGB(255, 255, 255)
#define COL_TITLE_BAR       RGB(22, 24, 34)
#define COL_TOOLBAR         RGB(24, 26, 38)
#define COL_BUTTON          RGB(50, 56, 80)
#define COL_BUTTON_HOVER    RGB(65, 72, 105)
#define COL_BUTTON_ACTIVE   RGB(80, 140, 240)
#define COL_DIVIDER         RGB(50, 54, 72)
#define COL_SHADOW          RGB(0, 0, 0)
#define COL_TRANSPARENT     0x00000000

/* ------------------------------------------------------------------ */
/*  Screen and layout constants                                        */
/* ------------------------------------------------------------------ */

#define TITLE_BAR_H     48
#define TOOLBAR_H       56
#define TILE_W          200
#define TILE_H          160
#define TILE_PAD        20
#define TILE_ICON_SZ    64
#define TILE_CORNER_R   8
#define ICON_MAX_W      64
#define ICON_MAX_H      64
#define CURSOR_W        12
#define CURSOR_H        19
#define TOOLTIP_PAD     6
#define BTN_H           36
#define BTN_PAD_X       16
#define BTN_PAD_Y       8
#define SCROLLBAR_W     10

/* Max tiles visible */
#define MAX_TILES       64

/* ------------------------------------------------------------------ */
/*  Structures                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    INT32 x, y;
    UINT32 w, h;
} RECT;

typedef struct {
    UINT32 w, h;
    COLOR *pixels;  /* Allocated, w*h pixels */
} ICON_IMAGE;

/* Tile types */
#define TILE_TYPE_LEGACY    0
#define TILE_TYPE_UEFI      1

typedef struct {
    RECT    bounds;
    CHAR16  label[64];
    CHAR16  sublabel[128];
    UINT32  snap_index;     /* Index into snapshot/uefi_boot array */
    UINT32  tile_type;      /* TILE_TYPE_LEGACY or TILE_TYPE_UEFI */
    ICON_IMAGE *icon;       /* NULL = use default */
    BOOLEAN hover;
    BOOLEAN selected;
} TILE;

typedef struct {
    RECT    bounds;
    CHAR16  label[32];
    UINT32  id;
    BOOLEAN hover;
    BOOLEAN active;
    BOOLEAN enabled;
    COLOR   color;          /* Button accent color */
} BUTTON;

typedef struct {
    INT32 x, y;
    INT32 prev_x, prev_y;
    BOOLEAN left_click;
    BOOLEAN right_click;
    BOOLEAN left_pressed;
    BOOLEAN right_pressed;
    BOOLEAN visible;
} MOUSE_STATE;

typedef struct {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    UINT32  screen_w;
    UINT32  screen_h;
    UINT32  stride;         /* Pixels per scanline */
    UINTN   fb_size;        /* Size in bytes of GOP framebuffer */
    COLOR   *framebuf;      /* Back buffer */
    COLOR   *screen;        /* Direct GOP framebuffer pointer */
} GFX_CONTEXT;

/* ------------------------------------------------------------------ */
/*  Global context                                                     */
/* ------------------------------------------------------------------ */

extern GFX_CONTEXT gGfx;
extern MOUSE_STATE gMouse;

/* ------------------------------------------------------------------ */
/*  Initialization                                                     */
/* ------------------------------------------------------------------ */

EFI_STATUS gfx_init(EFI_SYSTEM_TABLE *st, EFI_BOOT_SERVICES *bs);
void       gfx_shutdown(void);

/* ------------------------------------------------------------------ */
/*  Framebuffer operations                                             */
/* ------------------------------------------------------------------ */

void gfx_clear(COLOR c);
void gfx_flip(void);       /* Copy backbuffer to screen */
void gfx_flip_rect(INT32 x, INT32 y, UINT32 w, UINT32 h); /* Copy small rect to screen */
void gfx_begin_frame(void);/* Reset dirty tracking for new frame */
void gfx_cache_bg(void);   /* Snapshot framebuffer as cached background */
void gfx_restore_bg(void); /* Restore cached background (fast memcpy) */

/* ------------------------------------------------------------------ */
/*  Drawing primitives                                                 */
/* ------------------------------------------------------------------ */

void gfx_pixel(INT32 x, INT32 y, COLOR c);
void gfx_fill_rect(INT32 x, INT32 y, UINT32 w, UINT32 h, COLOR c);
void gfx_rect(INT32 x, INT32 y, UINT32 w, UINT32 h, COLOR c, UINT32 thickness);
void gfx_fill_rounded_rect(INT32 x, INT32 y, UINT32 w, UINT32 h, UINT32 r, COLOR c);
void gfx_rounded_rect(INT32 x, INT32 y, UINT32 w, UINT32 h, UINT32 r, COLOR c, UINT32 thickness);
void gfx_hline(INT32 x, INT32 y, UINT32 w, COLOR c);
void gfx_vline(INT32 x, INT32 y, UINT32 h, COLOR c);
void gfx_gradient_v(INT32 x, INT32 y, UINT32 w, UINT32 h, COLOR top, COLOR bottom);
void gfx_gradient_h(INT32 x, INT32 y, UINT32 w, UINT32 h, COLOR left, COLOR right);

/* Alpha blending (a = 0..255) */
COLOR gfx_blend(COLOR bg, COLOR fg, UINT8 alpha);
void gfx_fill_rect_alpha(INT32 x, INT32 y, UINT32 w, UINT32 h, COLOR c, UINT8 alpha);

/* ------------------------------------------------------------------ */
/*  Text rendering                                                     */
/* ------------------------------------------------------------------ */

/* Scale: 1 = 8x16, 2 = 16x32, etc. */
void gfx_char(INT32 x, INT32 y, CHAR16 ch, COLOR c, UINT32 scale);
void gfx_text(INT32 x, INT32 y, const CHAR16 *str, COLOR c, UINT32 scale);
void gfx_text_centered(INT32 cx, INT32 y, const CHAR16 *str, COLOR c, UINT32 scale);
UINT32 gfx_text_width(const CHAR16 *str, UINT32 scale);
UINT32 gfx_text_height(UINT32 scale);

/* ------------------------------------------------------------------ */
/*  Icon / BMP handling                                                */
/* ------------------------------------------------------------------ */

ICON_IMAGE *gfx_load_bmp(EFI_FILE_PROTOCOL *root, const CHAR16 *path);
ICON_IMAGE *gfx_create_default_icon(UINT8 type_id, COLOR accent);
void        gfx_free_icon(ICON_IMAGE *icon, EFI_BOOT_SERVICES *bs);
void        gfx_draw_icon(INT32 x, INT32 y, const ICON_IMAGE *icon);
void        gfx_draw_icon_scaled(INT32 x, INT32 y, UINT32 target_w, UINT32 target_h, const ICON_IMAGE *icon);

/* ------------------------------------------------------------------ */
/*  Mouse                                                              */
/* ------------------------------------------------------------------ */

EFI_STATUS  mouse_init(EFI_BOOT_SERVICES *bs);
void        mouse_poll(void);
void        mouse_draw(void);
void        mouse_save_under(void);
void        mouse_restore_under(void);
void        mouse_update_cursor(void);
BOOLEAN     mouse_in_rect(const RECT *r);
BOOLEAN     mouse_clicked_rect(const RECT *r);   /* Left click */
BOOLEAN     mouse_rclicked_rect(const RECT *r);  /* Right click */

/* ------------------------------------------------------------------ */
/*  UI Widgets                                                         */
/* ------------------------------------------------------------------ */

void ui_draw_tile(TILE *tile);
void ui_draw_button(BUTTON *btn);
void ui_draw_titlebar(const CHAR16 *title, UINT32 snap_count, UINT32 disk_count);
void ui_draw_toolbar(BUTTON *buttons, UINTN count);
void ui_draw_statusbar(const CHAR16 *msg);
void ui_draw_shadow(INT32 x, INT32 y, UINT32 w, UINT32 h, UINT32 spread);

/* Dialog boxes */
UINTN ui_dialog_yesno(const CHAR16 *title, const CHAR16 *message);
void  ui_dialog_info(const CHAR16 *title, const CHAR16 *message);
void  ui_dialog_input(const CHAR16 *prompt, CHAR16 *buf, UINTN max);
UINTN ui_dialog_file_select(EFI_FILE_PROTOCOL *root, const CHAR16 *dir,
                            const CHAR16 *filter, CHAR16 *selected, UINTN max);

/* ------------------------------------------------------------------ */
/*  Mouse cursor sprite (embedded)                                     */
/* ------------------------------------------------------------------ */

#define CURSOR_SPRITE_W 12
#define CURSOR_SPRITE_H 19

/* 0=transparent, 1=black border, 2=white fill */
static const UINT8 cursor_sprite[CURSOR_SPRITE_H][CURSOR_SPRITE_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0,0},
    {1,2,2,1,0,1,2,2,1,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0},
    {1,1,0,0,0,0,1,2,2,1,0,0},
    {1,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
};

#endif /* GFX_H */
