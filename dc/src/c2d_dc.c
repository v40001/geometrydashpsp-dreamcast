/*
c2d_dc.c - citro2d on the Dreamcast's PowerVR

The game draws two 3DS screens per frame: the top one (400x240) and the touch
screen (320x240). The Dreamcast shows them at 640x480, one at a time scaled to
fill the height, both side by side, or merged into one (the phone game's
layout, what the menus use).

Everything is transformed on the CPU as it is appended (the view matrix, then
the screen's place on the Dreamcast screen), so the PVR gets plain 2D
vertices and the batch only breaks when the texture, the mode or the blending
changes, like citro2d's.

Texture pages live in VRAM and are uploaded from the sheet file the first
time they are drawn: the Dreamcast's 8 MB of video memory cannot hold every
sheet at once (the PSP could keep them in its 32 MB of main RAM), so pages
are evicted least-recently-used when room is needed. A page that cannot be
made room for is skipped, not drawn with the wrong pixels.

The game uses citro2d's internals too (utils/gfx.c, through
utils/c2d_internal.h), so the batching follows that header's context.
*/
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <math.h>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <dc/biosfont.h>
#include <dc/fmath.h>

#include <citro2d.h>
#include "../../source/utils/c2d_internal.h"
#include "dc_port.h"
#include "main.h"           /* game_state, game_paused, in_level_complete */
#include "menus/settings.h" /* settingsState.fps60 */

#define DC_SCR_W   640
#define DC_SCR_H   480

/* one page's pixels: the largest the atlas makes is 512x512, 16 bits each */
#define STAGE_SIZE ( 512 * 512 * 2 )

/* a heavy official level peaks around 2000 sprites (8000 vertices) */
#define VTX_MAX     12288
#define IDX_MAX     ( VTX_MAX * 3 / 2 )

#define MAX_PAGES   256

C2Di_Context __C2Di_Context;

static C2Di_Vertex vtx_buf[VTX_MAX];
static u16 idx_buf[IDX_MAX];

/* the view (citro2d's model view), 2D affine: x' = a x + b y + tx */
typedef struct { float a, b, c, d, tx, ty; } Affine;
static Affine view;

/* where the current target lands on the Dreamcast screen
   (full: the target may draw anywhere on the screen, not just its rectangle) */
typedef struct { bool visible; float sx, sy, ox, oy; int w, h; bool full; } Placement;

/* merged view: where the touch screen sits on the top one (top screen units) */
static float merge_dx = 40, merge_dy = 0, merge_k = 1;
static C3D_RenderTarget *cur_target;
static Placement place;

static u32 fade_color;
static int tint_mode = C2D_TintMult;

/* the game's scissor, in the target's own coordinates */
static bool game_scissor;
static int sc_x0, sc_y0, sc_x1, sc_y1;

static C3D_TexEnv tex_envs[6];

typedef struct { float x, y, u, v; u32 c; } DC_Vert;

/* a sheet: the page metadata, and the file the pages are read from */
struct C2D_SpriteSheet_s {
	u32 num_images, num_pages;
	C3D_Tex *pages;
	Tex3DS_SubTexture *subtex;
	u16 *page_of;
	FILE *file;
	char path[256];
};


int psp_view = PSP_VIEW_TOP;

/* the frame: the PVR list, the draw order, the state last sent */
static bool in_frame;
static u32 frame_no;
static float z_next;

static bool tex_ok;            /* the current texture's page is in VRAM */
static C3D_Tex *cur_tex;
static bool blend_additive;
static bool cur_clip;
static int clip_x0, clip_y0, clip_x1, clip_y1;
static bool sent_clip;
static int sent_clip_rect[4];
static void *sent_hdr;

/*
In the merged view the touch screen is drawn after everything else, whatever
order the game draws its two screens in (the menus draw the touch screen
first): its primitives are kept here while it is the target, and sent at the
end of the frame.
*/
typedef struct {
	C3D_Tex *tex;
	u8 tex_ok, additive, clipped, n;
	s16 clip[4];
	DC_Vert v[4];
} DeferredPrim;

#define DEFER_PRIMS 2048
static bool deferring;
static DeferredPrim defer_buf[DEFER_PRIMS];
static int defer_count;

/* the pages the sheet loader made, for the VRAM eviction */
static C3D_Tex *pages[MAX_PAGES];
static int dc_page_count;

static u8 stage[STAGE_SIZE] __attribute__((aligned(32)));

static void DC_FlushVtxBuf( void );
static void DC_ApplyScissor( void );

/*
==============================================================================

Placement of the 3DS screens

==============================================================================
*/
static Placement DC_Place( const C3D_RenderTarget *t )
{
	Placement p = { false, 1, 1, 0, 0, 0, 0, false };

	if( !t || t->screen < 0 )
		return p;

	p.w = t->width;
	p.h = t->height;

	switch( psp_view )
	{
	case PSP_VIEW_MERGED:
		/* one screen, like the phone game: the top screen stretched to the
		   Dreamcast's, the touch screen's contents laid over it (drawn last,
		   see the deferred list below) */
		p.sx = p.sy = (float)DC_SCR_W / GSP_SCREEN_HEIGHT_TOP;
		p.oy = ( DC_SCR_H - GSP_SCREEN_WIDTH * p.sy ) * 0.5f;
		if( t->screen == GFX_BOTTOM )
		{
			p.ox = merge_dx * p.sx;
			p.oy += merge_dy * p.sy;
			p.sx *= merge_k;
			p.sy *= merge_k;
			p.full = true;
		}
		break;
	case PSP_VIEW_TOP:
		if( t->screen != GFX_TOP )
			return p;
		p.sx = p.sy = (float)DC_SCR_W / t->width;
		p.oy = ( DC_SCR_H - t->height * p.sy ) * 0.5f;
		break;
	case PSP_VIEW_BOTTOM:
		if( t->screen != GFX_BOTTOM )
			return p;
		p.sx = p.sy = (float)DC_SCR_H / t->height;
		p.ox = ( DC_SCR_W - t->width * p.sx ) * 0.5f;
		break;
	default: /* both, side by side: 400 + 320 wide */
		p.sx = p.sy = (float)DC_SCR_W / ( GSP_SCREEN_HEIGHT_TOP + GSP_SCREEN_HEIGHT_BOTTOM );
		p.ox = t->screen == GFX_TOP ? 0 : GSP_SCREEN_HEIGHT_TOP * p.sx;
		p.oy = ( DC_SCR_H - GSP_SCREEN_WIDTH * p.sy ) * 0.5f;
		break;
	}

	p.visible = true;
	return p;
}

/* a target's rectangle on the screen, clipped to it */
static void DC_TargetRect( const Placement *p, int *x0, int *y0, int *x1, int *y1 )
{
	if( p->full )
	{
		*x0 = *y0 = 0;
		*x1 = DC_SCR_W;
		*y1 = DC_SCR_H;
		return;
	}

	*x0 = (int)( p->ox );
	*y0 = (int)( p->oy );
	*x1 = (int)( p->ox + p->w * p->sx + 0.5f );
	*y1 = (int)( p->oy + p->h * p->sy + 0.5f );
	if( *x0 < 0 ) *x0 = 0;
	if( *y0 < 0 ) *y0 = 0;
	if( *x1 > DC_SCR_W ) *x1 = DC_SCR_W;
	if( *y1 > DC_SCR_H ) *y1 = DC_SCR_H;
}

/*
==============================================================================

VRAM pages

==============================================================================
*/
static void DC_RegisterPage( C3D_Tex *t )
{
	if( dc_page_count < MAX_PAGES )
		pages[dc_page_count++] = t;
}

static void DC_UnregisterPage( C3D_Tex *t )
{
	int i;

	for( i = 0; i < dc_page_count; i++ )
		if( pages[i] == t )
		{
			pages[i] = pages[--dc_page_count];
			return;
		}
}

static bool DC_TexInUse( const C3D_Tex *t )
{
	return t == cur_tex;
}

/*
Make room for size bytes. Only pages not drawn in this frame go: the ones in
this frame are still read by the PVR when it renders the list.
*/
static bool DC_MakeRoom( size_t size )
{
	int guard = 0;

	while( pvr_mem_available() < size )
	{
		C3D_Tex *victim = NULL;
		int i;

		for( i = 0; i < dc_page_count; i++ )
		{
			C3D_Tex *t = pages[i];
			if( !t->loaded || t->last_used >= frame_no || DC_TexInUse( t ))
				continue;
			if( !victim || t->last_used < victim->last_used )
				victim = t;
		}
		if( !victim || ++guard > MAX_PAGES )
			return false;

		pvr_mem_free( victim->vram );
		victim->vram = NULL;
		victim->loaded = 0;
		victim->hdr_valid[0] = victim->hdr_valid[1] = victim->hdr_valid[2] = victim->hdr_valid[3] = 0;
	}
	return true;
}

static bool DC_EnsureTex( C3D_Tex *t )
{
	size_t size;

	if( !t || !t->sheet || !t->sheet->file )
		return false;
	if( t->loaded )
	{
		t->last_used = frame_no;
		return true;
	}
	if( t->failed )
		return false;

	size = t->data_size;
	if( pvr_mem_available() < size && !DC_MakeRoom( size ))
	{
		/* every page is in this frame: try again next frame */
		return false;
	}

	/* free space can be fragmented: evict until one block fits */
	while( !( t->vram = pvr_mem_malloc( size )))
		if( !DC_MakeRoom( pvr_mem_available() + 1 ))
			return false;

	if( fseek( t->sheet->file, t->file_offset, SEEK_SET ) != 0 )
	{
		psp_log( "page: seek failed\n" );
	}
	if( fread( stage, 1, t->data_size, t->sheet->file ) != t->data_size )
	{
		pvr_mem_free( t->vram );
		t->vram = NULL;
		t->failed = 1;
		psp_log( "sheet read failed: page %d of %s\n", (int)t->page_index, t->sheet->path );
		return false;
	}

	/* the file holds the pixels already twiddled, in PVR order */
	/* plain CPU copy: the store queues are busy feeding the open TA list */
	{
		const uint32_t *src = (const uint32_t *)stage;
		volatile uint32_t *dst = (volatile uint32_t *)t->vram;
		size_t i;
		for( i = 0; i < size / 4; i++ )
			dst[i] = src[i];
	}
	{
		static int shown;
		if( shown++ < 1000 )
			psp_log( "page ok: %s\n", t->sheet->path );
	}

	t->loaded = 1;
	t->last_used = frame_no;
	return true;
}

/*
==============================================================================

PVR submission

==============================================================================
*/
static inline u32 DC_PackColor( u32 c )
{
	return (( c >> 24 ) << 24 ) | (( c & 0xff ) << 16 ) | (( c >> 8 & 0xff ) << 8 ) | ( c >> 16 & 0xff );
}

static pvr_poly_hdr_t *DC_TexHeader( C3D_Tex *t, int idx )
{
	pvr_poly_cxt_t cxt;
	pvr_poly_hdr_t *hdr = (pvr_poly_hdr_t *)t->hdr[idx];

	if( t->hdr_valid[idx] )
		return hdr;

	pvr_poly_cxt_txr( &cxt, PVR_LIST_TR_POLY,
		PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_POW2_STRIDE,
		t->width >> t->fmt, t->height >> t->fmt, t->vram,
		t->filter == GPU_LINEAR ? PVR_FILTER_BILINEAR : PVR_FILTER_NEAREST );
	cxt.gen.culling = PVR_CULLING_NONE;
	cxt.gen.shading = true; /* the tint can differ per corner */
	cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
	cxt.depth.write = false;
	cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
	cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
	cxt.gen.clip_mode = ( idx & 1 ) ? PVR_USERCLIP_INSIDE : PVR_USERCLIP_DISABLE;
	cxt.blend.src = PVR_BLEND_SRCALPHA;
	cxt.blend.dst = ( idx & 2 ) ? PVR_BLEND_ONE : PVR_BLEND_INVSRCALPHA;
	cxt.blend.src_enable = true;
	cxt.blend.dst_enable = true;
	pvr_poly_compile( hdr, &cxt );
	t->hdr_valid[idx] = 1;
	return hdr;
}

static pvr_poly_hdr_t *DC_SolidHeader( int idx, bool clear )
{
	static u32 hdr[4][8] __attribute__((aligned(32)));
	static u8 valid[4];
	pvr_poly_cxt_t cxt;
	pvr_poly_hdr_t *h = (pvr_poly_hdr_t *)hdr[idx];

	if( valid[idx] )
		return h;

	pvr_poly_cxt_col( &cxt, PVR_LIST_TR_POLY );
	cxt.gen.culling = PVR_CULLING_NONE;
	cxt.gen.shading = true;
	cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
	cxt.depth.write = false;
	cxt.gen.clip_mode = ( idx & 1 ) ? PVR_USERCLIP_INSIDE : PVR_USERCLIP_DISABLE;
	if( clear )
	{
		cxt.blend.src_enable = false;
		cxt.blend.dst_enable = false;
	}
	else
	{
		cxt.blend.src = PVR_BLEND_SRCALPHA;
		cxt.blend.dst = ( idx & 2 ) ? PVR_BLEND_ONE : PVR_BLEND_INVSRCALPHA;
		cxt.blend.src_enable = true;
		cxt.blend.dst_enable = true;
	}
	pvr_poly_compile( h, &cxt );
	valid[idx] = 1;
	return h;
}

/* the clear headers live outside the texture cache: their own table */
static pvr_poly_hdr_t *DC_ClearHeader( int clipped )
{
	static u32 hdr[2][8] __attribute__((aligned(32)));
	static u8 valid[2];
	pvr_poly_cxt_t cxt;
	pvr_poly_hdr_t *h = (pvr_poly_hdr_t *)hdr[clipped ? 1 : 0];

	if( valid[clipped] )
		return h;
	pvr_poly_cxt_col( &cxt, PVR_LIST_TR_POLY );
	cxt.gen.culling = PVR_CULLING_NONE;
	cxt.gen.shading = true;
	cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
	cxt.depth.write = false;
	cxt.gen.clip_mode = clipped ? PVR_USERCLIP_INSIDE : PVR_USERCLIP_DISABLE;
	cxt.blend.src_enable = false;
	cxt.blend.dst_enable = false;
	pvr_poly_compile( h, &cxt );
	valid[clipped] = 1;
	return h;
}

/*
The user clip area, when it changes: a vertex command with the corners. The
PVR clips whole 32x32 tiles, counted from the bottom left, so the rectangle
snaps outwards to the tiles that cover it (libkgl's glScissor does the same).
*/
static void DC_SetClip( int x0, int y0, int x1, int y1 )
{
	/* the TA's user clip command: 8 words, tile coordinates (32x32) from the
	   top left, inclusive, as integers in words 4..7 */
	uint32_t c[8] __attribute__((aligned(32)));
	int sx, sy, ex, ey;
	const int tw = DC_SCR_W / 32, th = DC_SCR_H / 32;

	if( sent_clip && sent_clip_rect[0] == x0 && sent_clip_rect[1] == y0
		&& sent_clip_rect[2] == x1 && sent_clip_rect[3] == y1 )
		return;

	sx = x0 / 32;
	sy = y0 / 32;
	ex = ( x1 + 31 ) / 32 - 1;
	ey = ( y1 + 31 ) / 32 - 1;
	if( sx < 0 ) sx = 0;
	if( sy < 0 ) sy = 0;
	if( ex > tw - 1 ) ex = tw - 1;
	if( ey > th - 1 ) ey = th - 1;
	if( ex < sx ) ex = sx;
	if( ey < sy ) ey = sy;

	c[0] = PVR_CMD_USERCLIP;
	c[1] = c[2] = c[3] = 0;
	c[4] = sx; c[5] = sy; c[6] = ex; c[7] = ey;
	pvr_prim( c, sizeof( c ));
	sent_hdr = NULL; /* the next primitive needs its header again */

	sent_clip = true;
	sent_clip_rect[0] = x0;
	sent_clip_rect[1] = y0;
	sent_clip_rect[2] = x1;
	sent_clip_rect[3] = y1;
}

static void DC_UseHeader( void *hdr )
{
	if( sent_hdr == hdr )
		return;
	pvr_prim( hdr, sizeof( pvr_poly_hdr_t ));
	sent_hdr = hdr;
}

/*
The four vertices go out as they came (the game rotates sprites, lines and
9-slices): a PVR triangle strip, v0 v1 v2 v3, is the two triangles of the
quad citro2d appends.
*/
static void DC_EmitQuad( const DC_Vert *q, float z )
{
	pvr_vertex_t v;
	int i;

	if( cur_tex && !tex_ok )
		return;
	if( cur_clip )
		DC_SetClip( clip_x0, clip_y0, clip_x1, clip_y1 );
	DC_UseHeader( tex_ok ? (void *)DC_TexHeader( cur_tex, ( blend_additive ? 2 : 0 ) | ( cur_clip ? 1 : 0 ))
		: (void *)DC_SolidHeader(( blend_additive ? 2 : 0 ) | ( cur_clip ? 1 : 0 ), false ));

	v.z = z;
	v.oargb = 0;
	for( i = 0; i < 4; i++ )
	{
		v.flags = ( i == 3 ) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
		v.x = q[i].x; v.y = q[i].y; v.u = q[i].u; v.v = q[i].v;
		v.argb = DC_PackColor( q[i].c );
		pvr_prim( &v, sizeof( v ));
	}
}

static void DC_EmitTri( const DC_Vert *t, float z )
{
	pvr_vertex_t v;
	int i;

	if( cur_tex && !tex_ok )
		return;
	if( cur_clip )
		DC_SetClip( clip_x0, clip_y0, clip_x1, clip_y1 );
	DC_UseHeader( tex_ok ? (void *)DC_TexHeader( cur_tex, ( blend_additive ? 2 : 0 ) | ( cur_clip ? 1 : 0 ))
		: (void *)DC_SolidHeader(( blend_additive ? 2 : 0 ) | ( cur_clip ? 1 : 0 ), false ));

	v.z = z;
	v.oargb = 0;
	for( i = 0; i < 3; i++ )
	{
		v.flags = ( i == 2 ) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
		v.x = t[i].x; v.y = t[i].y; v.u = t[i].u; v.v = t[i].v;
		v.argb = DC_PackColor( t[i].c );
		pvr_prim( &v, sizeof( v ));
	}
}

static void DC_SubmitQuad( const DC_Vert *q, float z )
{
	if( deferring )
	{
		DeferredPrim *d;
		int i;

		if( defer_count >= DEFER_PRIMS )
			return;
		d = &defer_buf[defer_count++];
		d->tex = cur_tex; d->tex_ok = tex_ok; d->additive = blend_additive;
		d->clipped = cur_clip; d->n = 4;
		d->clip[0] = clip_x0; d->clip[1] = clip_y0; d->clip[2] = clip_x1; d->clip[3] = clip_y1;
		for( i = 0; i < 4; i++ )
			d->v[i] = q[i];
		return;
	}
	DC_EmitQuad( q, z );
}

static void DC_SubmitTri( const DC_Vert *t, float z )
{
	if( deferring )
	{
		DeferredPrim *d;
		int i;

		if( defer_count >= DEFER_PRIMS )
			return;
		d = &defer_buf[defer_count++];
		d->tex = cur_tex; d->tex_ok = tex_ok; d->additive = blend_additive;
		d->clipped = cur_clip; d->n = 3;
		d->clip[0] = clip_x0; d->clip[1] = clip_y0; d->clip[2] = clip_x1; d->clip[3] = clip_y1;
		for( i = 0; i < 3; i++ )
			d->v[i] = t[i];
		return;
	}
	DC_EmitTri( t, z );
}

/* the common case: an upright rectangle, its corners and their texels */
static void DC_SubmitRect( float x0, float y0, float x1, float y1,
	float u0, float v0, float u1, float v1,
	u32 c0, u32 c1, u32 c2, u32 c3, float z )
{
	DC_Vert q[4] = {
		{ x0, y0, u0, v0, c0 },
		{ x1, y0, u1, v0, c1 },
		{ x0, y1, u0, v1, c2 },
		{ x1, y1, u1, v1, c3 },
	};
	DC_SubmitQuad( q, z );
}

/*
==============================================================================

citro2d internals (utils/c2d_internal.h)

==============================================================================
*/
static bool DC_ModeTextured( u32 flags )
{
	return ( flags & C2DiF_Mode_Mask ) >= C2DiF_Mode_ImageSolid;
}

void C2Di_FlushVtxBuf( void )
{
	C2Di_Context *ctx = C2Di_GetContext();
	size_t n = ctx->idxBufPos;
	size_t i = 0;

	if( !in_frame || !place.visible )
	{
		ctx->idxBufPos = ctx->idxBufLastPos = 0;
		ctx->vtxBufPos = 0;
		return;
	}

	while( i + 3 <= n )
	{
		const u16 *x = &ctx->idxBuf[i];
		bool quad = ( i + 6 <= n ) && x[1] == x[0] + 1 && x[2] == x[0] + 2
			&& x[3] == x[2] && x[4] == x[1] && x[5] == x[0] + 3;

		if( quad )
		{
			DC_Vert q[4];
			int k;

			for( k = 0; k < 4; k++ )
			{
				const C2Di_Vertex *v = &ctx->vtxBuf[x[0] + k];
				q[k].x = v->pos[0];
				q[k].y = v->pos[1];
				q[k].u = v->texcoord[0];
				q[k].v = v->texcoord[1];
				q[k].c = v->color;
			}
			DC_SubmitQuad( q, z_next );
			z_next += 1.0f / 1024.0f;
			i += 6;
		}
		else
		{
			DC_Vert t[3];
			int k;

			for( k = 0; k < 3; k++ )
			{
				const C2Di_Vertex *v = &ctx->vtxBuf[x[k]];
				t[k].x = v->pos[0];
				t[k].y = v->pos[1];
				t[k].u = v->texcoord[0];
				t[k].v = v->texcoord[1];
				t[k].c = v->color;
			}
			DC_SubmitTri( t, z_next );
			z_next += 1.0f / 1024.0f;
			i += 3;
		}
	}

	ctx->idxBufPos = ctx->idxBufLastPos = 0;
	ctx->vtxBufPos = 0;
}

void C2Di_Update( void )
{
	C2Di_Context *ctx = C2Di_GetContext();

	if( !( ctx->flags & ( C2DiF_DirtyTex | C2DiF_DirtyMode )))
		return;

	C2Di_FlushVtxBuf();

	if( in_frame )
	{
		cur_tex = DC_ModeTextured( ctx->flags ) ? ctx->curTex : NULL;
		tex_ok = cur_tex ? DC_EnsureTex( cur_tex ) : false;
		if( cur_tex && !tex_ok && cur_tex->failed == 1 )
		{
			cur_tex->failed = 2;
			psp_log( "page %d of %s not in VRAM: its sprites are skipped\n",
				(int)cur_tex->page_index, cur_tex->sheet ? cur_tex->sheet->path : "?" );
		}
	}

	ctx->flags &= ~( C2DiF_DirtyTex | C2DiF_DirtyMode | C2DiF_DirtyProj | C2DiF_DirtyMdlv | C2DiF_DirtyFade );
}

void C2Di_CalcQuad( C2Di_Quad *quad, const C2D_DrawParams *params )
{
	const float w = fabsf( params->pos.w );
	const float h = fabsf( params->pos.h );

	quad->topLeft[0]  = -params->center.x;
	quad->topLeft[1]  = -params->center.y;
	quad->topRight[0] = -params->center.x + w;
	quad->topRight[1] = -params->center.y;
	quad->botLeft[0]  = -params->center.x;
	quad->botLeft[1]  = -params->center.y + h;
	quad->botRight[0] = -params->center.x + w;
	quad->botRight[1] = -params->center.y + h;

	if( params->angle != 0.0f )
	{
		float c, s;
		psp_sincosf( params->angle, &s, &c );
#define ROTATE( _v ) do { \
			float a = _v[0] * c - _v[1] * s; \
			float b = _v[1] * c + _v[0] * s; \
			_v[0] = a; \
			_v[1] = b; \
		} while( 0 )
		ROTATE( quad->topLeft );
		ROTATE( quad->topRight );
		ROTATE( quad->botLeft );
		ROTATE( quad->botRight );
#undef ROTATE
	}

	quad->topLeft[0]  += params->pos.x;
	quad->topLeft[1]  += params->pos.y;
	quad->topRight[0] += params->pos.x;
	quad->topRight[1] += params->pos.y;
	quad->botLeft[0]  += params->pos.x;
	quad->botLeft[1]  += params->pos.y;
	quad->botRight[0] += params->pos.x;
	quad->botRight[1] += params->pos.y;
}

void C2Di_AppendTri( void )
{
	C2Di_Context *ctx = C2Di_GetContext();
	u16 v = ctx->vtxBufPos;

	ctx->idxBuf[ctx->idxBufPos++] = v + 0;
	ctx->idxBuf[ctx->idxBufPos++] = v + 1;
	ctx->idxBuf[ctx->idxBufPos++] = v + 2;
}

void C2Di_AppendQuad( void )
{
	C2Di_Context *ctx = C2Di_GetContext();
	u16 v = ctx->vtxBufPos;

	ctx->idxBuf[ctx->idxBufPos++] = v + 0;
	ctx->idxBuf[ctx->idxBufPos++] = v + 1;
	ctx->idxBuf[ctx->idxBufPos++] = v + 2;
	ctx->idxBuf[ctx->idxBufPos++] = v + 2;
	ctx->idxBuf[ctx->idxBufPos++] = v + 1;
	ctx->idxBuf[ctx->idxBufPos++] = v + 3;
}

/*
The vertex goes in finished: through the view, onto its place on the screen,
the texture coordinates normalized (the PVR wants them so) and the color
with the tint's blend already applied. citro2d's "mult" tint is
texture * mix(white, color, blend), which is the PVR's modulate with that
mixed color.
*/
void C2Di_AppendVtx( float x, float y, float z, float u, float v, float ptx, float pty, u32 color )
{
	C2Di_Context *ctx = C2Di_GetContext();
	C2Di_Vertex *vtx = &ctx->vtxBuf[ctx->vtxBufPos++];
	float vx = view.a * x + view.b * y + view.tx;
	float vy = view.c * x + view.d * y + view.ty;

	(void)z;
	(void)ptx;

	vtx->pos[0] = place.ox + vx * place.sx;
	vtx->pos[1] = place.oy + vy * place.sy;
	vtx->pos[2] = 0;

	if( DC_ModeTextured( ctx->flags ) && ctx->curTex )
	{
		/* the 3DS keeps its textures upside down: v is the other way round */
		vtx->texcoord[0] = u;
		vtx->texcoord[1] = 1.0f - v;

		if( pty < 1.0f )
		{
			/* blend 0 leaves the texture as it is, 1 multiplies by the color */
			u32 r = color & 0xff, g = ( color >> 8 ) & 0xff, b = ( color >> 16 ) & 0xff;
			float k = pty < 0.0f ? 0.0f : pty;
			r = (u32)( 255.0f - ( 255.0f - r ) * k );
			g = (u32)( 255.0f - ( 255.0f - g ) * k );
			b = (u32)( 255.0f - ( 255.0f - b ) * k );
			color = ( color & 0xff000000 ) | r | ( g << 8 ) | ( b << 16 );
		}
	}
	else
	{
		vtx->texcoord[0] = vtx->texcoord[1] = 0;
	}

	vtx->ptcoord[0] = ptx;
	vtx->ptcoord[1] = pty;
	vtx->color = color;
}

/* room for a primitive, flushing what is there if it doesn't fit */
static bool DC_Reserve( unsigned idx, unsigned vtx )
{
	C2Di_Context *ctx = C2Di_GetContext();

	if( !( ctx->flags & C2DiF_Active ))
		return false;
	if( !C2Di_CheckBufSpace( ctx, idx, vtx ))
		C2Di_FlushVtxBuf();
	return true;
}

/*
==============================================================================

citro2d

==============================================================================
*/
bool C2D_Init( size_t maxObjects )
{
	C2Di_Context *ctx = C2Di_GetContext();

	(void)maxObjects;
	memset( ctx, 0, sizeof( *ctx ));
	ctx->vtxBuf = vtx_buf;
	ctx->idxBuf = idx_buf;
	ctx->vtxBufSize = VTX_MAX;
	ctx->idxBufSize = IDX_MAX;
	ctx->flags = C2DiF_Active | C2DiF_DirtyAny;
	C2D_SetTintMode( C2D_TintSolid );
	C2D_ViewReset();
	return true;
}

void C2D_Fini( void )
{
}

void C2D_Prepare( void )
{
	C2Di_Context *ctx = C2Di_GetContext();
	ctx->flags |= C2DiF_Active | C2DiF_DirtyAny;
	ctx->curTex = NULL;
}

void C2D_Flush( void )
{
	C2Di_FlushVtxBuf();
}

void C2D_SceneSize( u32 width, u32 height, bool tilt )
{
	(void)width; (void)height; (void)tilt;
}

static void DC_FadeRect( int x0, int y0, int x1, int y1 )
{
	C2D_Flush();
	cur_clip = false;
	sent_hdr = NULL;
	cur_tex = NULL;
	tex_ok = false;
	DC_UseHeader( (void *)DC_SolidHeader( 0, false ));
	DC_SubmitRect( (float)x0, (float)y0, (float)x1, (float)y1, 0, 0, 0, 0,
		fade_color, fade_color, fade_color, fade_color, z_next );
	z_next += 1.0f / 1024.0f;
	C2Di_GetContext()->flags |= C2DiF_DirtyTex;
}

static void DC_DrawFade( void )
{
	int x0, y0, x1, y1;

	/* merged: once over everything, at the end of the frame */
	if( !in_frame || !place.visible || !( fade_color >> 24 ) || psp_view == PSP_VIEW_MERGED )
		return;

	DC_TargetRect( &place, &x0, &y0, &x1, &y1 );
	DC_FadeRect( x0, y0, x1, y1 );
	DC_ApplyScissor();
}

static void DC_ApplyScissor( void )
{
	int x0, y0, x1, y1;

	if( !place.visible )
		return;

	DC_TargetRect( &place, &x0, &y0, &x1, &y1 );

	if( game_scissor )
	{
		int gx0 = (int)( place.ox + sc_x0 * place.sx );
		int gy0 = (int)( place.oy + sc_y0 * place.sy );
		int gx1 = (int)( place.ox + sc_x1 * place.sx + 0.5f );
		int gy1 = (int)( place.oy + sc_y1 * place.sy + 0.5f );
		if( gx0 > x0 ) x0 = gx0;
		if( gy0 > y0 ) y0 = gy0;
		if( gx1 < x1 ) x1 = gx1;
		if( gy1 < y1 ) y1 = gy1;
		if( x1 < x0 ) x1 = x0;
		if( y1 < y0 ) y1 = y0;
	}

	cur_clip = !( x0 <= 0 && y0 <= 0 && x1 >= DC_SCR_W && y1 >= DC_SCR_H );
	clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
}

void C2D_SceneTarget( C3D_RenderTarget *target )
{
	C2Di_FlushVtxBuf();
	DC_DrawFade(); /* the previous scene's */

	cur_target = target;
	place = DC_Place( target );
	game_scissor = false;
	cur_clip = false;
	sent_hdr = NULL;

	if( in_frame )
	{
		bool defer = psp_view == PSP_VIEW_MERGED && target && target->screen == GFX_BOTTOM;
		if( defer && !deferring )
			deferring = true;
		else if( !defer && deferring )
			deferring = false;
		DC_ApplyScissor();
	}

	C2Di_GetContext()->flags |= C2DiF_DirtyAny;
}

void C2D_TargetClear( C3D_RenderTarget *target, u32 color )
{
	Placement p = DC_Place( target );
	int x0, y0, x1, y1;

	/* merged: the touch screen's clear would wipe the top screen under it */
	if( !in_frame || !p.visible || p.full )
		return;

	C2Di_FlushVtxBuf();
	DC_TargetRect( &p, &x0, &y0, &x1, &y1 );

	cur_clip = false;
	sent_hdr = NULL;
	DC_UseHeader( (void *)DC_ClearHeader( false ));
	DC_SubmitRect( (float)x0, (float)y0, (float)x1, (float)y1, 0, 0, 0, 0,
		color, color, color, color, z_next );
	z_next += 1.0f / 1024.0f;

	C2Di_GetContext()->flags |= C2DiF_DirtyTex;
	DC_ApplyScissor();
}

void C2D_Fade( u32 color )
{
	fade_color = color;
}

void C2D_SetTintMode( C2D_TintMode mode )
{
	C2Di_Context *ctx = C2Di_GetContext();
	u32 new_mode;

	tint_mode = mode;
	switch( mode )
	{
	default:
	case C2D_TintSolid: new_mode = C2DiF_Mode_ImageSolid; break;
	case C2D_TintMult:  new_mode = C2DiF_Mode_ImageMult;  break;
	case C2D_TintLuma:  new_mode = C2DiF_Mode_ImageLuma;  break;
	}

	ctx->flags = ( ctx->flags & ~C2DiF_TintMode_Mask ) | ( new_mode << ( C2DiF_TintMode_Shift - C2DiF_Mode_Shift ));
}

C3D_RenderTarget *C2D_CreateScreenTarget( gfxScreen_t screen, gfx3dSide_t side )
{
	C3D_RenderTarget *t = C3D_RenderTargetCreate( 240, screen == GFX_TOP ? 400 : 320, GPU_RB_RGBA8, GPU_RB_DEPTH16 );
	C3D_RenderTargetSetOutput( t, screen, side, 0 );
	return t;
}

/* ---- view ---------------------------------------------------------- */

void C2D_ViewReset( void )
{
	C2Di_FlushVtxBuf();
	view.a = view.d = 1;
	view.b = view.c = view.tx = view.ty = 0;
}

void C2D_ViewSave( C3D_Mtx *m )
{
	memset( m, 0, sizeof( *m ));
	m->r[0].x = view.a; m->r[0].y = view.b; m->r[0].w = view.tx;
	m->r[1].x = view.c; m->r[1].y = view.d; m->r[1].w = view.ty;
	m->r[2].z = 1;
	m->r[3].w = 1;
}

void C2D_ViewRestore( const C3D_Mtx *m )
{
	view.a = m->r[0].x; view.b = m->r[0].y; view.tx = m->r[0].w;
	view.c = m->r[1].x; view.d = m->r[1].y; view.ty = m->r[1].w;
}

/* all of them apply on the right, as citro2d's: the new one acts first */
void C2D_ViewTranslate( float x, float y )
{
	view.tx += view.a * x + view.b * y;
	view.ty += view.c * x + view.d * y;
}

void C2D_ViewScale( float x, float y )
{
	view.a *= x; view.c *= x;
	view.b *= y; view.d *= y;
}

void C2D_ViewRotate( float r )
{
	float c = cosf( r ), s = sinf( r );
	Affine o = view;

	view.a = o.a * c + o.b * s;
	view.b = -o.a * s + o.b * c;
	view.c = o.c * c + o.d * s;
	view.d = -o.c * s + o.d * c;
}

void C2D_ViewShear( float x, float y )
{
	Affine o = view;

	view.a = o.a + o.b * y;
	view.b = o.a * x + o.b;
	view.c = o.c + o.d * y;
	view.d = o.c * x + o.d;
}

/* ---- drawing ------------------------------------------------------- */

bool C2D_DrawImage( C2D_Image img, const C2D_DrawParams *params, const C2D_ImageTint *tints )
{
	static const C2D_Tint default_tint[4] = {
		{ 0xFFu << 24, 0.0f }, { 0xFFu << 24, 0.0f }, { 0xFFu << 24, 0.0f }, { 0xFFu << 24, 0.0f }
	};
	C2Di_Context *ctx = C2Di_GetContext();
	const C2D_Tint *tint = tints ? tints->corners : default_tint;
	float tl[2], tr[2], bl[2], br[2];
	C2Di_Quad quad;

	if( !img.tex || !img.subtex )
		return false;
	if( !DC_Reserve( 6, 4 ))
		return false;

	C2Di_SetMode(( ctx->flags & C2DiF_TintMode_Mask ) >> ( C2DiF_TintMode_Shift - C2DiF_Mode_Shift ));
	C2Di_SetTex( img.tex );
	C2Di_Update();

	C2Di_CalcQuad( &quad, params );

	Tex3DS_SubTextureTopLeft( img.subtex, &tl[0], &tl[1] );
	Tex3DS_SubTextureTopRight( img.subtex, &tr[0], &tr[1] );
	Tex3DS_SubTextureBottomLeft( img.subtex, &bl[0], &bl[1] );
	Tex3DS_SubTextureBottomRight( img.subtex, &br[0], &br[1] );

	if( params->pos.w < 0 )
	{
		C2Di_SwapUV( tl, tr );
		C2Di_SwapUV( bl, br );
	}
	if( params->pos.h < 0 )
	{
		C2Di_SwapUV( tl, bl );
		C2Di_SwapUV( tr, br );
	}

	C2Di_AppendQuad();
	C2Di_AppendVtx( quad.topLeft[0],  quad.topLeft[1],  params->depth, tl[0], tl[1], 0, tint[0].blend, tint[0].color );
	C2Di_AppendVtx( quad.topRight[0], quad.topRight[1], params->depth, tr[0], tr[1], 0, tint[1].blend, tint[1].color );
	C2Di_AppendVtx( quad.botLeft[0],  quad.botLeft[1],  params->depth, bl[0], bl[1], 0, tint[2].blend, tint[2].color );
	C2Di_AppendVtx( quad.botRight[0], quad.botRight[1], params->depth, br[0], br[1], 0, tint[3].blend, tint[3].color );
	return true;
}

static void DC_SolidMode( void )
{
	C2Di_SetMode( C2DiF_Mode_Solid );
	C2Di_Update();
}

bool C2D_DrawTriangle( float x0, float y0, u32 clr0, float x1, float y1, u32 clr1,
	float x2, float y2, u32 clr2, float depth )
{
	if( !DC_Reserve( 3, 3 ))
		return false;

	DC_SolidMode();
	C2Di_AppendTri();
	C2Di_AppendVtx( x0, y0, depth, -1, -1, 0, 0, clr0 );
	C2Di_AppendVtx( x1, y1, depth, -1, -1, 0, 0, clr1 );
	C2Di_AppendVtx( x2, y2, depth, -1, -1, 0, 0, clr2 );
	return true;
}

bool C2D_DrawLine( float x0, float y0, u32 clr0, float x1, float y1, u32 clr1, float thickness, float depth )
{
	float dx = x1 - x0, dy = y1 - y0;
	float len = sqrtf( dx * dx + dy * dy );
	float th, nx, ny;

	if( len <= 0.0f || !DC_Reserve( 6, 4 ))
		return false;

	th = thickness / 2.0f / len;
	nx = -dy * th;
	ny = dx * th;

	DC_SolidMode();
	C2Di_AppendQuad();
	C2Di_AppendVtx( x0 + nx, y0 + ny, depth, -1, -1, 0, 0, clr0 );
	C2Di_AppendVtx( x0 - nx, y0 - ny, depth, -1, -1, 0, 0, clr0 );
	C2Di_AppendVtx( x1 + nx, y1 + ny, depth, -1, -1, 0, 0, clr1 );
	C2Di_AppendVtx( x1 - nx, y1 - ny, depth, -1, -1, 0, 0, clr1 );
	return true;
}

bool C2D_DrawRectangle( float x, float y, float z, float w, float h, u32 clr0, u32 clr1, u32 clr2, u32 clr3 )
{
	if( !DC_Reserve( 6, 4 ))
		return false;

	DC_SolidMode();
	C2Di_AppendQuad();
	C2Di_AppendVtx( x,     y,     z, -1, -1, 0, 0, clr0 );
	C2Di_AppendVtx( x + w, y,     z, -1, -1, 0, 0, clr1 );
	C2Di_AppendVtx( x,     y + h, z, -1, -1, 0, 0, clr2 );
	C2Di_AppendVtx( x + w, y + h, z, -1, -1, 0, 0, clr3 );
	return true;
}

bool C2D_DrawEllipse( float x, float y, float z, float w, float h, u32 clr0, u32 clr1, u32 clr2, u32 clr3 )
{
	int i, n = 32;
	float cx = x + w / 2, cy = y + h / 2;

	(void)clr1; (void)clr2; (void)clr3;
	for( i = 0; i < n; i++ )
	{
		float a0 = i * 2 * M_PI / n, a1 = ( i + 1 ) * 2 * M_PI / n;
		C2D_DrawTriangle( cx, cy, clr0,
			cx + cosf( a0 ) * w / 2, cy + sinf( a0 ) * h / 2, clr0,
			cx + cosf( a1 ) * w / 2, cy + sinf( a1 ) * h / 2, clr0, z );
	}
	return true;
}

/*
==============================================================================

citro3d

==============================================================================
*/
bool C3D_Init( size_t cmdBufSize )
{
	pvr_init_params_t params = {
		.opb_sizes = { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_0 },
		.vertex_buf_size = 512 * 1024,
		.opb_overflow_count = 2,
		.dma_enabled = 0,
		.fsaa_enabled = 0,
		.autosort_disabled = 1, /* draw in the order given, like citro2d */
		/* one vertex buffer: the next frame waits for the render anyway */
		.vbuf_doublebuf_disabled = 1,
	};

	(void)cmdBufSize;

	/* BOOT DEBUG: yellow means C3D_Init was reached */
	vid_set_mode( DM_640x480, PM_RGB565 );
	vid_clear( 160, 160, 0 );
	bfont_draw_str_vram_fmt( 8, 8, true, "gd-dreamcast: C3D_Init" );

	params.opb_sizes[0] = PVR_BINSIZE_0;
	params.opb_sizes[2] = PVR_BINSIZE_16;
	params.vertex_buf_size = 256 * 1024;
	params.autosort_disabled = 0;
	if( pvr_init( &params ) < 0 )
		return false;

	pvr_set_bg_color( 0, 0, 0 );

	return true;
}

void C3D_Fini( void )
{
	pvr_shutdown();
}

/* BOOT DEBUG: the background colour says how far the startup got */
void dc_boot_stage( int stage )
{
	static const float col[][3] = {
		{ 0.6f, 0.0f, 0.0f }, { 0.0f, 0.6f, 0.0f }, { 0.0f, 0.0f, 0.6f },
		{ 0.6f, 0.6f, 0.0f }, { 0.6f, 0.0f, 0.6f }, { 0.0f, 0.6f, 0.6f },
		{ 0.6f, 0.3f, 0.0f }, { 0.6f, 0.6f, 0.6f }, { 0.3f, 0.3f, 0.3f },
		{ 0.3f, 0.0f, 0.3f },
	};
	int k;

	if( stage < 0 || stage > 9 )
		return;
	pvr_wait_ready();
	pvr_set_bg_color( col[stage][0], col[stage][1], col[stage][2] );
	for( k = 0; k < 2; k++ )
	{
		pvr_scene_begin();
		pvr_list_begin( PVR_LIST_TR_POLY );
		pvr_list_finish();
		pvr_scene_finish();
		pvr_wait_ready();
	}
}

/*
The frame is shown when the next one begins: between the two the game runs
its logic while the PVR is still drawing, instead of one waiting for the other.
*/
static u32 vblank_count;

void PSP_PresentPending( void )
{
	/* pvr_wait_ready in the next C3D_FrameBegin does the waiting */
}

u32 PSP_PresentVcount( void )
{
	return vblank_count;
}

bool C3D_FrameBegin( u8 flags )
{
	(void)flags;

	pvr_wait_ready();
	pvr_scene_begin();
	pvr_list_begin( PVR_LIST_TR_POLY );

	in_frame = true;
	frame_no++;
	vblank_count++;
	z_next = 1.0f;
	sent_hdr = NULL;
	sent_clip = false;
	cur_clip = false;
	cur_tex = NULL;
	tex_ok = false;
	deferring = false;
	defer_count = 0;

	C2D_Prepare();
	place = DC_Place( cur_target );
	if( psp_view == PSP_VIEW_MERGED && cur_target && cur_target->screen == GFX_BOTTOM )
		deferring = true;
	DC_ApplyScissor();
	return true;
}

/* the touch screen's draws, kept while the top screen is drawn, go last */
static void DC_ReplayDeferred( void )
{
	int i;

	deferring = false;
	for( i = 0; i < defer_count; i++ )
	{
		DeferredPrim *d = &defer_buf[i];
		cur_tex = d->tex;
		tex_ok = d->tex_ok;
		blend_additive = d->additive;
		cur_clip = d->clipped;
		clip_x0 = d->clip[0]; clip_y0 = d->clip[1];
		clip_x1 = d->clip[2]; clip_y1 = d->clip[3];
		if( d->n == 4 )
			DC_EmitQuad( d->v, z_next );
		else
			DC_EmitTri( d->v, z_next );
		z_next += 1.0f / 1024.0f;
	}
	defer_count = 0;
}

void C3D_FrameEnd( u8 flags )
{
	(void)flags;

	C2Di_FlushVtxBuf();
	DC_DrawFade();
	DC_ReplayDeferred();

	if( psp_view == PSP_VIEW_MERGED && ( fade_color >> 24 ))
		DC_FadeRect( 0, 0, DC_SCR_W, DC_SCR_H );

	PSP_DrawOverlay();

	in_frame = false;

	pvr_list_finish();
	pvr_scene_finish();
}

bool PSP_FullScreenRect( float *x, float *y, float *w, float *h )
{
	Placement p;
	float x0, y0, x1, y1;

	if( !cur_target )
		return false;

	p = DC_Place( cur_target );
	if( !p.visible )
		return false;

	x0 = -p.ox / p.sx;
	x1 = ( DC_SCR_W - p.ox ) / p.sx;
	y0 = -p.oy / p.sy;
	y1 = ( DC_SCR_H - p.oy ) / p.sy;

	*x = x0;
	*y = y0;
	*w = x1 - x0;
	*h = y1 - y0;
	return true;
}

void PSP_DrawRect2D( float x0, float y0, float x1, float y1, u32 color )
{
	if( !in_frame )
		return;

	C2Di_FlushVtxBuf();
	cur_clip = false;
	sent_hdr = NULL;
	cur_tex = NULL;
	tex_ok = false;
	DC_UseHeader( (void *)DC_SolidHeader( 0, false ));
	DC_SubmitRect( x0, y0, x1, y1, 0, 0, 0, 0, color, color, color, color, z_next );
	z_next += 1.0f / 1024.0f;
	C2Di_GetContext()->flags |= C2DiF_DirtyTex;
	DC_ApplyScissor();
}

bool PSP_ScreenToPsp( int screen, float x, float y, float *px, float *py )
{
	C3D_RenderTarget t = { screen, 0, screen == GFX_TOP ? 400 : 320, 240, 0, false };
	Placement p = DC_Place( &t );

	if( !p.visible )
		return false;
	*px = p.ox + x * p.sx;
	*py = p.oy + y * p.sy;
	return true;
}

void C3D_AlphaBlend( GPU_BLENDEQUATION colorEq, GPU_BLENDEQUATION alphaEq,
	GPU_BLENDFACTOR srcClr, GPU_BLENDFACTOR dstClr, GPU_BLENDFACTOR srcAlpha, GPU_BLENDFACTOR dstAlpha )
{
	(void)alphaEq; (void)srcAlpha; (void)dstAlpha;

	C2Di_FlushVtxBuf();

	/* the game asks for src alpha / one when it wants additive */
	blend_additive = ( dstClr == GPU_ONE );
	if( colorEq != GPU_BLEND_ADD )
		blend_additive = false;
}

void C3D_SetScissor( GPU_SCISSORMODE mode, u32 left, u32 top, u32 right, u32 bottom )
{
	C2Di_FlushVtxBuf();

	if( mode == GPU_SCISSOR_DISABLE || !cur_target )
	{
		game_scissor = false;
	}
	else
	{
		/* the 3DS framebuffer stands on its side: undo utils/gfx.c set_scissor */
		int W = cur_target->width, H = cur_target->height;
		sc_x0 = W - (int)bottom;
		sc_x1 = W - (int)top;
		sc_y0 = H - (int)right;
		sc_y1 = H - (int)left;
		game_scissor = true;
	}

	if( in_frame )
		DC_ApplyScissor();
}

void C3D_TexSetFilter( C3D_Tex *tex, GPU_TEXTURE_FILTER_PARAM magFilter, GPU_TEXTURE_FILTER_PARAM minFilter )
{
	(void)minFilter;
	if( tex && tex->filter != magFilter )
	{
		tex->hdr_valid[0] = tex->hdr_valid[1] = tex->hdr_valid[2] = tex->hdr_valid[3] = 0;
		tex->filter = magFilter;
		if( tex == C2Di_GetContext()->curTex )
			C2Di_GetContext()->flags |= C2DiF_DirtyTex;
	}
}

C3D_RenderTarget *C3D_RenderTargetCreate( int width, int height, GPU_COLORBUF colorFmt, GPU_DEPTHBUF depthFmt )
{
	C3D_RenderTarget *t = calloc( 1, sizeof( *t ));

	(void)colorFmt; (void)depthFmt;
	if( !t )
		return NULL;
	/* the 3DS framebuffer is on its side: "width" is the 240 and "height" the long one */
	t->screen = -1;
	t->width = height;
	t->height = width;
	if( t->width > 400 ) t->width = 400;
	if( t->height > 240 ) t->height = 240;
	return t;
}

void C3D_RenderTargetDelete( C3D_RenderTarget *target )
{
	if( target == cur_target )
		cur_target = NULL;
	free( target );
}

void C3D_RenderTargetSetOutput( C3D_RenderTarget *target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags )
{
	(void)transferFlags;
	if( !target )
		return;
	target->screen = side == GFX_RIGHT ? -1 : screen;
	target->side = side;
	target->width = screen == GFX_TOP ? 400 : 320;
	target->height = 240;
}

float C3D_GetProcessingTime( void ) { return 0; }
float C3D_GetDrawingTime( void ) { return 0; }
float C3D_GetCmdBufUsage( void ) { return 0; }

C3D_TexEnv *C3D_GetTexEnv( int id ) { return &tex_envs[id < 6 ? id : 0]; }
void C3D_TexEnvInit( C3D_TexEnv *env ) { memset( env, 0, sizeof( *env )); }
void C3D_TexEnvSrc( C3D_TexEnv *env, C3D_TexEnvMode mode, int s1, int s2, int s3 ) { (void)env; (void)mode; (void)s1; (void)s2; (void)s3; }
void C3D_TexEnvFunc( C3D_TexEnv *env, C3D_TexEnvMode mode, int param ) { (void)env; (void)mode; (void)param; }

void psp_set_merge( float dx, float dy, float k )
{
	merge_dx = dx;
	merge_dy = dy;
	merge_k = k > 0 ? k : 1;
}

void psp_get_merge( float *dx, float *dy, float *k )
{
	*dx = merge_dx;
	*dy = merge_dy;
	*k = merge_k;
}

bool psp_merged_bottom_now( void )
{
	return deferring;
}

/*
==============================================================================

Sprite sheets (.dcs, dc/tools/mkatlas.py)

==============================================================================
*/

typedef struct { u16 w, h; u8 fmt, pad[3]; u32 size; } DcsPage;
typedef struct { u16 page, x, y, w, h, pad; } DcsImage;

C2D_SpriteSheet C2D_SpriteSheetLoad( const char *filename )
{
	char path[256];
	char *dot;
	FILE *f;
	C2D_SpriteSheet sheet;
	char magic[4];
	u32 i;

	/* the game asks for the 3DS name: romfs:/gfx/x.t3x -> romfs:/gfx/x.dcs */
	snprintf( path, sizeof( path ), "%s", filename );
	if(( dot = strrchr( path, '.' )) && !strcmp( dot, ".t3x" ))
		strcpy( dot, ".dcs" );

	psp_log( "sheet: %s\n", path );
	if( !( f = fopen( path, "rb" )))
	{
		psp_log( "C2D_SpriteSheetLoad: can't open %s\n", path );
		return NULL;
	}
	psp_log( "sheet: opened\n" );

	sheet = calloc( 1, sizeof( *sheet ));
	if( !sheet )
		goto fail;
	snprintf( sheet->path, sizeof( sheet->path ), "%s", path );
	sheet->file = f;

	if( fread( magic, 1, 4, f ) != 4 || memcmp( magic, "DCS1", 4 )
		|| fread( &sheet->num_images, 4, 1, f ) != 1 || fread( &sheet->num_pages, 4, 1, f ) != 1 )
		goto fail;
	psp_log( "sheet: header %u imgs %u pages\n", sheet->num_images, sheet->num_pages );

	if( sheet->num_pages > MAX_PAGES || sheet->num_images > 65535 )
		goto fail;

	sheet->pages = memalign( 32, sheet->num_pages * sizeof( C3D_Tex ));
	if( !sheet->pages )
		goto fail;
	memset( sheet->pages, 0, sheet->num_pages * sizeof( C3D_Tex ));

	for( i = 0; i < sheet->num_pages; i++ )
	{
		DcsPage pg;
		C3D_Tex *t = &sheet->pages[i];

		if( fread( &pg, sizeof( pg ), 1, f ) != 1 )
			goto fail;

		t->width = pg.w;
		t->height = pg.h;
		t->fmt = pg.fmt;
		t->filter = GPU_LINEAR;
		t->data_size = pg.size;
		t->file_offset = (u32)ftell( f );
		t->sheet = sheet;
		t->page_index = i;

		DC_RegisterPage( t );
		if( !DC_EnsureTex( t ))
			psp_log( "sheet: page %u of %s did not fit in VRAM\n", i, path );
		if( fseek( f, t->file_offset + pg.size, SEEK_SET ) != 0 )
			goto fail;
	}

	psp_log( "sheet: pages read\n" );
	sheet->subtex = calloc( sheet->num_images, sizeof( Tex3DS_SubTexture ));
	sheet->page_of = calloc( sheet->num_images, sizeof( u16 ));
	if( !sheet->subtex || !sheet->page_of )
		goto fail;

	for( i = 0; i < sheet->num_images; i++ )
	{
		DcsImage im;
		const C3D_Tex *t;
		Tex3DS_SubTexture *s = &sheet->subtex[i];

		if( fread( &im, sizeof( im ), 1, f ) != 1 || im.page >= sheet->num_pages )
			goto fail;

		t = &sheet->pages[im.page];
		sheet->page_of[i] = im.page;
		s->width = im.w;
		s->height = im.h;
		s->left = (float)im.x / t->width;
		s->right = (float)( im.x + im.w ) / t->width;
		s->top = 1.0f - (float)im.y / t->height;
		s->bottom = 1.0f - (float)( im.y + im.h ) / t->height;
	}

	return sheet;

fail:
	psp_log( "C2D_SpriteSheetLoad: %s is broken\n", path );
	if( sheet && sheet->pages )
	{
		for( i = 0; i < sheet->num_pages; i++ )
			DC_UnregisterPage( &sheet->pages[i] );
		free( sheet->pages );
	}
	if( sheet )
	{
		free( sheet->subtex );
		free( sheet->page_of );
		free( sheet );
	}
	fclose( f );
	return NULL;
}

C2D_SpriteSheet C2D_SpriteSheetLoadFromMem( const void *data, size_t size )
{
	(void)data; (void)size;
	return NULL;
}

void C2D_SpriteSheetFree( C2D_SpriteSheet sheet )
{
	u32 i;

	if( !sheet )
		return;

	/* the PVR may still be reading these pages: wait for the last frame */
	pvr_wait_render_done();

	for( i = 0; i < sheet->num_pages; i++ )
	{
		C3D_Tex *t = &sheet->pages[i];
		if( t->loaded && t->vram )
			pvr_mem_free( t->vram );
		DC_UnregisterPage( t );
	}
	free( sheet->pages );
	free( sheet->subtex );
	free( sheet->page_of );
	if( sheet->file )
		fclose( sheet->file );
	free( sheet );
}

size_t C2D_SpriteSheetCount( C2D_SpriteSheet sheet )
{
	return sheet ? sheet->num_images : 0;
}

C2D_Image C2D_SpriteSheetGetImage( C2D_SpriteSheet sheet, size_t index )
{
	/* a missing image still needs a subtexture: the game reads its size */
	static const Tex3DS_SubTexture empty = { 1, 1, 0, 1, 0, 1 };
	C2D_Image img = { NULL, &empty };

	if( !sheet || index >= sheet->num_images )
	{
		psp_log( "C2D_SpriteSheetGetImage: no image %d\n", (int)index );
		return img;
	}
	img.tex = &sheet->pages[sheet->page_of[index]];
	img.subtex = &sheet->subtex[index];
	return img;
}
