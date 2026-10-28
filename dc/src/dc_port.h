/*
dc_port.h - what the Dreamcast compatibility files share

Same interface as the PSP port's psp_port.h: the game code under __GD_PORT__
(ui_screen.c, level_select.c, ...) uses these names for the one-screen menu
layout and the touch cursor. The implementation lives in dc/src/ctru_dc.c and
dc/src/c2d_dc.c.
*/
#pragma once
#include <stdbool.h>
#include <3ds.h>

/* what the Dreamcast screen shows (chosen by ctru_dc.c from the game state) */
enum {
	PSP_VIEW_TOP,     /* the 3DS top screen, scaled to fill the height */
	PSP_VIEW_BOTTOM,  /* the touch screen, with a cursor */
	PSP_VIEW_BOTH,    /* both side by side, smaller */
	PSP_VIEW_MERGED,  /* one screen: the touch screen's contents over the top one */
	PSP_VIEW_COUNT
};
extern int psp_view;

/* c2d_dc.c, merged view: where the touch screen sits on the top one, in
   top screen units (x' = dx + x * k), and whether its scene is being drawn */
void psp_set_merge( float dx, float dy, float k );
void psp_get_merge( float *dx, float *dy, float *k );
bool psp_merged_bottom_now( void );

/* ui_screen.c: the menu on screen has a one-screen PSP layout */
extern bool psp_menu_merged;

/* ui_screen.c: what can be touched this frame (buttons, sliders, ...), for
   the d-pad to move between. Centers and half sizes in their screen's units */
typedef struct {
	float x, y, hw, hh;
	bool bottom;      /* on the touch screen */
	bool slider;
	bool in_list;     /* a row of a scrolling list: only there while it shows */
	float cx0, cy0, cx1, cy1; /* ... in this part of the screen */
	u32 keys;         /* the keys the button answers to by itself */
	const void *elem; /* compared, never followed */
} PspTarget;

#define PSP_MAX_TARGETS 96
extern PspTarget psp_targets[PSP_MAX_TARGETS];
extern int psp_target_count;

/* ui_list.c, around its rows' update: what part of the screen they show in */
void psp_list_clip( float x0, float y0, float x1, float y1 );
void psp_list_clip_end( void );

/* set by ctru_dc.c, applied by ui_screen.c when that slider updates:
   moves it a step (-1 / +1) */
extern const void *psp_slider_focus;
extern int psp_slider_nudge;

/* a log file on the VMU (dc_log.txt) */
void psp_log( const char *fmt, ... );
void psp_log_memory( const char *when );

/* c2d_dc.c */
void PSP_DrawRect2D( float x0, float y0, float x1, float y1, u32 color );
bool PSP_FullScreenRect( float *x, float *y, float *w, float *h );
bool PSP_ScreenToPsp( int screen, float x, float y, float *px, float *py );
/* shows the last frame if it is still waiting for the next one to begin
   (before anything slow, like loading: else it shows only after that) */
void PSP_PresentPending( void );
/* the vblank count when the last frame went on screen (59.94 a second) */
u32 PSP_PresentVcount( void );

/* ctru_dc.c: drawn last every frame (the touch cursor) */
void PSP_DrawOverlay( void );

/* c2d_dc.c: BOOT DEBUG, the background colour of the last startup step */
void dc_boot_stage( int stage );

/* one sine and cosine from the SH-4's FPU (sinf and cosf are library calls
   with a lot of range reduction). About 6 digits: for drawing, not for the
   physics, which stay on the game's own math. */
#include <dc/fmath.h>
static inline void psp_sincosf( float rad, float *s, float *c )
{
	fsincosr( rad, s, c );
}
