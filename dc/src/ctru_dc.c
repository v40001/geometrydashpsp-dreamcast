/*
ctru_dc.c - libctru on the Dreamcast: time, threads, input, paths and the rest

Controls (the 3DS has a touch screen the Dreamcast lacks):
  A          A, and a touch at the cursor while the touch screen is shown
  B          B            X          Y          Y          X
  Start      Start        L/R        L / R
  D-pad      in the menus: from button to button; playing: the d-pad
  Analog     moves the cursor when the touch screen is shown, else the circle pad
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <malloc.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>

#include <kos.h>
#include <kos/dbglog.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/video.h>
#include <dc/biosfont.h>
#include <kos/timer.h>
#include <dc/fs_iso9660.h>
#include <kos/fs_ramdisk.h>

#include <3ds.h>
#include <citro2d.h>
#include "dc_port.h"
#include "romfs_dc.h"
#include "main.h" /* game_state */

/*
==============================================================================

Log

==============================================================================
*/
static char log_ring[64 * 1024];
static size_t log_used;

void psp_log( const char *fmt, ... )
{
	char line[512];
	va_list args;
	int n;

	va_start( args, fmt );
	n = vsnprintf( line, sizeof( line ), fmt, args );
	va_end( args );

	if( n < 0 )
		return;
	if((size_t)n >= sizeof( line ))
		n = sizeof( line ) - 1;

	if( log_used + n < sizeof( log_ring ))
	{
		memcpy( log_ring + log_used, line, n );
		log_used += n;
	}

	/* the dcload/SCIF console, when something is listening */
	printf( "%s", line );
	fflush( stdout );
	dbglog( DBG_NOTICE, "%s", line );

}

void psp_log_memory( const char *when )
{
	struct mallinfo mi = mallinfo();
	psp_log( "memory %s: in use %d KB, free %d KB\n", when,
		mi.uordblks / 1024, mi.fordblks / 1024 );
}

/* the log, on the VMU, when the game stops on a fatal error */
static void dc_log_dump( void )
{
	FILE *f = fopen( "/vmu/a1/gdlog.txt", "wb" );

	if( !f )
		f = fopen( "/ram/gdlog.txt", "wb" );
	if( !f )
		return;
	fwrite( log_ring, 1, log_used, f );
	fclose( f );
}

/*
==============================================================================

Paths

The game names its files romfs:/ (read only, on the disc) and /3ds/gd3ds/
(config, saves). The disc is ISO9660, and the config lives on a RAM disk for
now: the VMU has no directories, and its 200 KB are better left for the
save file later.

==============================================================================
*/
static const char *DC_MapPath( const char *path, char *buf, size_t n )
{
	if( !path )
		return path;
	if( !strncmp( path, "romfs:/", 7 ))
	{
		snprintf( buf, n, "/cd/romfs/%s", path + 7 );
		return buf;
	}
	if( !strncmp( path, "/3ds/gd3ds/", 11 ))
	{
		/* the ramdisk is flat: keep the file's place in the name */
		char name[200];
		const char *r = path + 11;
		size_t i = 0;

		while( *r && i < sizeof( name ) - 1 )
		{
			name[i++] = ( *r == '/' ) ? '_' : *r;
			r++;
		}
		name[i] = 0;
		snprintf( buf, n, "/ram/gd3ds_%s", name );
		return buf;
	}
	if( !strncmp( path, "sdmc:/", 6 ))
	{
		snprintf( buf, n, "/ram/%s", path + 6 );
		return buf;
	}
	return path;
}

/*
==============================================================================

Config and saves

The game wants a writable /3ds/gd3ds/. KOS's ramdisk crashes when a file is
created on it, so these live in RAM here instead: fopen/open_memstream, stat,
remove and rename over a small table. They last the session (the VMU save is
still to be wired up).
*/
#define MEMFILE_MAX 32

typedef struct {
	char name[160];
	char *data;
	size_t size;
} MemFile;

static MemFile memfiles[MEMFILE_MAX];
static int memfile_count;

static bool DC_MemPath( const char *path )
{
	return !strncmp( path, "/3ds/gd3ds/", 11 ) || !strncmp( path, "sdmc:/", 6 );
}

static const char *DC_MemName( const char *path )
{
	return !strncmp( path, "/3ds/gd3ds/", 11 ) ? path + 11 : path + 6;
}

static MemFile *DC_MemFind( const char *name )
{
	int i;
	for( i = 0; i < memfile_count; i++ )
		if( !strcmp( memfiles[i].name, name ))
			return &memfiles[i];
	return NULL;
}

static MemFile *DC_MemAdd( const char *name )
{
	MemFile *f;

	if( memfile_count >= MEMFILE_MAX )
		return NULL;
	f = &memfiles[memfile_count++];
	snprintf( f->name, sizeof( f->name ), "%s", name );
	f->data = NULL;
	f->size = 0;
	return f;
}

static void DC_MemRemove( const char *name )
{
	MemFile *f = DC_MemFind( name );
	if( !f )
		return;
	free( f->data );
	*f = memfiles[--memfile_count];
}

FILE *__real_fopen( const char *path, const char *mode );
int __real_open( const char *path, int flags, ... );
int __real_stat( const char *path, struct stat *buf );
DIR *__real_opendir( const char *path );
int __real_access( const char *path, int mode );
int __real_remove( const char *path );
int __real_unlink( const char *path );
int __real_rename( const char *a, const char *b );
int __real_mkdir( const char *path, mode_t mode );

FILE *__wrap_fopen( const char *path, const char *mode )
{
	char buf[256];
	const char *name;
	bool write;
	MemFile *f;

	if( !strncmp( path, "romfs:/", 7 ))
		return dc_romfs_fopen( path + 7 );
	if( !DC_MemPath( path ))
		return __real_fopen( DC_MapPath( path, buf, sizeof( buf )), mode );

	name = DC_MemName( path );
	write = strchr( mode, 'w' ) || strchr( mode, 'a' ) || strchr( mode, '+' );
	f = DC_MemFind( name );

	if( !write )
	{
		if( !f || !f->data )
			return NULL;
		return fmemopen( f->data, f->size, "r" );
	}

	if( !f )
		f = DC_MemAdd( name );
	if( !f )
		return NULL;
	free( f->data );
	f->data = NULL;
	f->size = 0;
	return open_memstream( &f->data, &f->size );
}

int __wrap_open( const char *path, int flags, ... )
{
	char buf[256];
	va_list args;
	int mode = 0;

	if( flags & O_CREAT )
	{
		va_start( args, flags );
		mode = va_arg( args, int );
		va_end( args );
	}
	return __real_open( DC_MapPath( path, buf, sizeof( buf )), flags, mode );
}

int __wrap_stat( const char *path, struct stat *buf )
{
	char b[256];

	if( !strncmp( path, "romfs:/", 7 ))
		return dc_romfs_stat( path + 7, buf );
	if( DC_MemPath( path ))
	{
		MemFile *f = DC_MemFind( DC_MemName( path ));
		if( !f )
			return -1;
		memset( buf, 0, sizeof( *buf ));
		buf->st_mode = S_IFREG | 0644;
		buf->st_size = f->size;
		return 0;
	}
	return __real_stat( DC_MapPath( path, b, sizeof( b )), buf );
}

DIR *__wrap_opendir( const char *path )
{
	char buf[256];

	/* no custom levels or songs in this build */
	if( DC_MemPath( path ))
		return NULL;
	return __real_opendir( DC_MapPath( path, buf, sizeof( buf )));
}

int __wrap_access( const char *path, int mode )
{
	/* KOS has no access(): the table (or stat) says whether it is there */
	struct stat st;
	char buf[256];

	(void)mode;
	if( !strncmp( path, "romfs:/", 7 ))
		return dc_romfs_stat( path + 7, &st );
	if( DC_MemPath( path ))
		return DC_MemFind( DC_MemName( path )) ? 0 : -1;
	return __real_stat( DC_MapPath( path, buf, sizeof( buf )), &st );
}

int __wrap_remove( const char *path )
{
	char buf[256];
	if( DC_MemPath( path ))
	{
		DC_MemRemove( DC_MemName( path ));
		return 0;
	}
	return __real_remove( DC_MapPath( path, buf, sizeof( buf )));
}

int __wrap_unlink( const char *path )
{
	char buf[256];
	if( DC_MemPath( path ))
	{
		DC_MemRemove( DC_MemName( path ));
		return 0;
	}
	return __real_unlink( DC_MapPath( path, buf, sizeof( buf )));
}

int __wrap_rename( const char *a, const char *b )
{
	char ba[256], bb[256];
	MemFile *f;

	if( DC_MemPath( a ) && DC_MemPath( b ))
	{
		f = DC_MemFind( DC_MemName( a ));
		if( !f )
			return -1;
		snprintf( f->name, sizeof( f->name ), "%s", DC_MemName( b ));
		return 0;
	}
	return __real_rename( DC_MapPath( a, ba, sizeof( ba )), DC_MapPath( b, bb, sizeof( bb )));
}

int __wrap_mkdir( const char *path, mode_t mode )
{
	char buf[256];
	const char *mapped;

	/* the config and saves live on a flat ramdisk: no directories, and the
	   KOS VFS does not like a mkdir with no handler for it */
	if( !strncmp( path, "/3ds/", 5 ))
		return 0;
	mapped = DC_MapPath( path, buf, sizeof( buf ));
	if( !strncmp( mapped, "/ram/", 5 ))
		return 0;
	return __real_mkdir( mapped, mode );
}

/*
==============================================================================

Time

==============================================================================
*/
u64 svcGetSystemTick( void )
{
	/* microseconds to the 3DS's 268.111856 MHz ticks */
	u64 us = timer_us_gettime64();
	return us * 268 + us * 111856 / 1000000;
}

void svcSleepThread( s64 ns )
{
	if( ns <= 0 )
	{
		thd_pass();
		return;
	}
	thd_sleep( (uint32_t)( ns / 1000000 ));
}

unsigned int sceKernelGetSystemTimeLow( void )
{
	return (unsigned int)( timer_us_gettime64() / 1000 );
}

u64 osGetTime( void )
{
	return timer_ms_gettime64();
}

void svcBreak( u32 reason )
{
	psp_log( "svcBreak(%u): the game stopped on a fatal error\n", (unsigned)reason );
	dc_log_dump();
	thd_sleep( 2000 );
	arch_exit();
}

Result svcGetSystemInfo( s64 *out, u32 type, s32 param )
{
	(void)type; (void)param;
	*out = 0;
	return 0;
}

Result svcGetThreadPriority( s32 *out, Handle handle )
{
	(void)handle;
	*out = 0x30;
	return 0;
}

void osSetSpeedupEnable( bool enable ) { (void)enable; }
float osGet3DSliderState( void ) { return 0; }

size_t envGetHeapSize( void )
{
	return 16 * 1024 * 1024;
}

/*
==============================================================================

Memory

==============================================================================
*/
void *linearAlloc( size_t size ) { return memalign( 32, size ); }
void linearFree( void *mem ) { free( mem ); }

u32 linearSpaceFree( void )
{
	struct mallinfo mi = mallinfo();
	return (u32)mi.fordblks;
}

/*
==============================================================================

App, system

==============================================================================
*/
bool aptMainLoop( void )
{
	return true;
}

Result romfsInit( void )
{
	/* BOOT DEBUG: blue means the binary started */
	vid_set_mode( DM_640x480, PM_RGB565 );

	if( dc_romfs_init() < 0 )
		psp_log( "romfs: the disc is not ISO9660\n" );
	else
		psp_log( "romfs: ready\n" );


	/* mkdir( "/ram/gd3ds", 0777 ); */

	/* mkdir 2 */
	/* mkdir 3 */

	psp_log( "gd-dreamcast: started\n" );
	psp_log_memory( "at start" );
	return 0;
}

Result romfsExit( void ) { return 0; }

Result cfguInit( void ) { return 0; }
void cfguExit( void ) {}

Result CFGU_GetSystemModel( u8 *model )
{
	*model = CFG_MODEL_N3DS;
	return 0;
}

Result socInit( u32 *context_addr, u32 context_size ) { (void)context_addr; (void)context_size; return -1; }
Result socExit( void ) { return 0; }

void gfxInitDefault( void ) {}
void gfxExit( void ) {}
void gfxSet3D( bool enable ) { (void)enable; }
void gfxSetWide( bool enable ) { (void)enable; }
void gfxSwapBuffers( void ) {}
void gfxFlushBuffers( void ) {}
void gspWaitForVBlank( void ) { vid_waitvbl(); }

PrintConsole *consoleInit( gfxScreen_t screen, PrintConsole *console )
{
	(void)screen;
	return console;
}

Result HIDUSER_GetSoundVolume( u8 *volume )
{
	*volume = 63;
	return 0;
}

/*
==============================================================================

Threads

==============================================================================
*/
struct DcThread_s {
	pthread_t id;
	ThreadFunc func;
	void *arg;
	bool detached;
};

static void *DC_ThreadEntry( void *p )
{
	struct DcThread_s *t = p;
	t->func( t->arg );
	return NULL;
}

Thread threadCreate( ThreadFunc entrypoint, void *arg, size_t stack_size, int prio, int core_id, bool detached )
{
	struct DcThread_s *t = calloc( 1, sizeof( *t ));
	pthread_attr_t attr;
	int r;

	(void)prio; (void)core_id;
	if( !t )
		return NULL;

	pthread_attr_init( &attr );
	pthread_attr_setstacksize( &attr, stack_size < 0x4000 ? 0x4000 : stack_size );
	if( detached )
		pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );

	t->func = entrypoint;
	t->arg = arg;
	t->detached = detached;
	r = pthread_create( &t->id, &attr, DC_ThreadEntry, t );
	pthread_attr_destroy( &attr );
	if( r != 0 )
	{
		free( t );
		return NULL;
	}
	return t;
}

Result threadJoin( Thread thread, u64 timeout_ns )
{
	(void)timeout_ns;
	if( !thread )
		return -1;
	if( !thread->detached )
		pthread_join( thread->id, NULL );
	free( thread );
	return 0;
}

void threadFree( Thread thread ) { (void)thread; }

/* a zeroed LightLock is a valid unlocked one on the 3DS, so it's made on first use */
static pthread_mutex_t *DC_LockMake( LightLock *lock )
{
	if( !lock->mtx )
	{
		pthread_mutex_t *m = calloc( 1, sizeof( *m ));
		if( m )
			pthread_mutex_init( m, NULL );
		lock->mtx = m;
	}
	return lock->mtx;
}

void LightLock_Init( LightLock *lock )
{
	lock->mtx = NULL;
	DC_LockMake( lock );
}

void LightLock_Lock( LightLock *lock )
{
	pthread_mutex_lock( DC_LockMake( lock ));
}

void LightLock_Unlock( LightLock *lock )
{
	pthread_mutex_unlock( DC_LockMake( lock ));
}

typedef struct { pthread_mutex_t mtx; pthread_cond_t cond; int signaled; } DcEvent;

static DcEvent *DC_EventMake( LightEvent *event )
{
	if( !event->inited )
	{
		DcEvent *e = calloc( 1, sizeof( *e ));
		if( !e )
			return NULL;
		pthread_mutex_init( &e->mtx, NULL );
		pthread_cond_init( &e->cond, NULL );
		event->cond = e;
		event->mtx = NULL;
		event->inited = 1;
		event->signaled = 0;
	}
	return event->cond;
}

void LightEvent_Init( LightEvent *event, ResetType reset_type )
{
	DcEvent *e;

	memset( event, 0, sizeof( *event ));
	event->type = reset_type;
	e = DC_EventMake( event );
	(void)e;
}

void LightEvent_Signal( LightEvent *event )
{
	DcEvent *e = DC_EventMake( event );

	if( !e )
		return;
	pthread_mutex_lock( &e->mtx );
	e->signaled = 1;
	pthread_cond_signal( &e->cond );
	pthread_mutex_unlock( &e->mtx );
}

void LightEvent_Wait( LightEvent *event )
{
	DcEvent *e = DC_EventMake( event );

	if( !e )
		return;
	pthread_mutex_lock( &e->mtx );
	while( !e->signaled )
		pthread_cond_wait( &e->cond, &e->mtx );
	if( event->type == RESET_ONESHOT )
		e->signaled = 0;
	pthread_mutex_unlock( &e->mtx );
}

void LightEvent_Clear( LightEvent *event )
{
	DcEvent *e = DC_EventMake( event );

	if( !e )
		return;
	pthread_mutex_lock( &e->mtx );
	e->signaled = 0;
	pthread_mutex_unlock( &e->mtx );
}

/*
==============================================================================

Input

==============================================================================
*/
#define TOUCH_W 320
#define TOUCH_H 240

static u32 keys_held, keys_old;
static touchPosition touch_pos;
static circlePosition circle_pos;
static float cursor_x = TOUCH_W / 2, cursor_y = TOUCH_H / 2;
static bool cursor_used;

/* the HID shared memory, as utils/precise_input.c reads it */
static u32 hid_mem[0x400];
vu32 *hidSharedMem = hid_mem;

#define PAD_TICK_WORD    0
#define PAD_IDX_WORD     4
#define PAD_RING_WORD    ( 0x28 / 4 )
#define TOUCH_TICK_WORD  42
#define TOUCH_IDX_WORD   46
#define TOUCH_RING_WORD  (( 0xA8 + 0x20 ) / 4 )

static void DC_SetTick( int word, u64 tick )
{
	hid_mem[word] = (u32)tick;
	hid_mem[word + 1] = (u32)( tick >> 32 );
}

static u64 DC_GetTick( int word )
{
	return hid_mem[word] | ( (u64)hid_mem[word + 1] << 32 );
}

/* one sample per scan, the ring laps every 8 like the 3DS's */
static void DC_PushSample( int tick_word, int idx_word, int ring_word, int entry_words, const u32 *entry )
{
	u32 idx = ( hid_mem[idx_word] + 1 ) & 7;
	int i;

	if( idx == 0 )
	{
		DC_SetTick( tick_word + 2, DC_GetTick( tick_word ));
		DC_SetTick( tick_word, svcGetSystemTick());
	}
	for( i = 0; i < entry_words; i++ )
		hid_mem[ring_word + idx * entry_words + i] = entry[i];
	hid_mem[idx_word] = idx;
}

static void DC_InputInit( void )
{
	u64 now = svcGetSystemTick();
	u64 frame = (u64)( CPU_TICKS_PER_MSEC * 16.7 );

	DC_SetTick( PAD_TICK_WORD, now );
	DC_SetTick( PAD_TICK_WORD + 2, now - 8 * frame );
	DC_SetTick( TOUCH_TICK_WORD, now );
	DC_SetTick( TOUCH_TICK_WORD + 2, now - 8 * frame );
}

/* the cursor is out, A touches with it: in the menus, and while playing
   only in the pause menu and the level complete window (A jumps else;
   the practice buttons over the level go by their keys) */
static bool DC_TouchShown( void )
{
	if( game_state == STATE_GAME && !game_paused && !in_level_complete )
		return false;
	return psp_view == PSP_VIEW_BOTTOM || psp_view == PSP_VIEW_BOTH || psp_view == PSP_VIEW_MERGED;
}

/*
==============================================================================

D-pad navigation in the menus

ui_screen.c lists what can be touched while the menus update; the d-pad
moves the cursor to the nearest one that way and A touches it. A direction a
button already answers to (level select's arrows) stays a key, and on a
slider left/right move it.

==============================================================================
*/
static PspTarget nav[PSP_MAX_TARGETS];
static int nav_count;
static int snap_pending;
static bool cursor_by_dpad;

static bool DC_TargetToCursor( const PspTarget *t, float *cx, float *cy, float *hw, float *hh )
{
	float k = 1, dx = 0, dy = 0;

	if( t->in_list && ( t->x < t->cx0 || t->x > t->cx1 || t->y < t->cy0 || t->y > t->cy1 ))
		return false;

	if( psp_view == PSP_VIEW_MERGED )
	{
		if( t->bottom )
			psp_get_merge( &dx, &dy, &k );
	}
	else if( !t->bottom )
	{
		return false;
	}

	*cx = dx + t->x * k;
	*cy = dy + t->y * k;
	*hw = t->hw * k;
	*hh = t->hh * k;
	return true;
}

static const PspTarget *DC_TargetAt( float x, float y )
{
	const PspTarget *best = NULL;
	float best_area = 1e9f;
	int i;

	for( i = 0; i < nav_count; i++ )
	{
		float cx, cy, hw, hh;
		if( !DC_TargetToCursor( &nav[i], &cx, &cy, &hw, &hh ))
			continue;
		if( x < cx - hw || x >= cx + hw || y < cy - hh || y >= cy + hh )
			continue;
		if( hw * hh < best_area )
		{
			best_area = hw * hh;
			best = &nav[i];
		}
	}
	return best;
}

static bool DC_Navigate( int dx, int dy )
{
	float best_score = 1e9f, bx = 0, by = 0;
	bool found = false;
	int i;

	for( i = 0; i < nav_count; i++ )
	{
		float cx, cy, hw, hh, vx, vy, along, across, score;

		if( !DC_TargetToCursor( &nav[i], &cx, &cy, &hw, &hh ))
			continue;

		vx = cx - cursor_x;
		vy = cy - cursor_y;
		if( dx || dy )
		{
			along = vx * dx + vy * dy;
			across = fabsf( vx * dy ) + fabsf( vy * dx );
			if( along < 6 )
				continue;
			score = along + across * 2;
		}
		else
		{
			score = vx * vx + vy * vy;
		}

		if( score < best_score )
		{
			best_score = score;
			bx = cx;
			by = cy;
			found = true;
		}
	}

	if( found )
	{
		cursor_x = bx;
		cursor_y = by;
	}
	return found;
}

void hidScanInput( void )
{
	u32 buttons = 0;
	int ax8 = 0, ay8 = 0, ltrig = 0, rtrig = 0;
	u32 keys = 0;
	float ax, ay;
	static u32 dc_old;
	u32 entry[4] = { 0 };
	u32 tentry[2] = { 0 };
	static int last_state = -1;
	static bool last_merged, last_paused, last_complete;
	float cursor_w;

	{
		maple_device_t *dev = maple_enum_type( 0, MAPLE_FUNC_CONTROLLER );
		cont_state_t *st = dev ? (cont_state_t *)maple_dev_status( dev ) : NULL;
		if( st )
		{
			buttons = st->buttons;
			ax8 = st->joyx;
			ay8 = st->joyy;
			ltrig = st->ltrig;
			rtrig = st->rtrig;
		}
	}

	/* what the menus offered last frame; this frame's list starts empty */
	memcpy( nav, psp_targets, sizeof( PspTarget ) * psp_target_count );
	nav_count = psp_target_count;
	psp_target_count = 0;

	/* the view follows the game: a menu with a PSP layout is one screen, the
	   others show both; playing, the level fills the screen with the touch
	   screen's buttons over it (pause, practice), like the phone game */
	if( game_state != last_state || psp_menu_merged != last_merged || game_paused != last_paused
		|| in_level_complete != last_complete )
	{
		if( game_state == STATE_GAME )
			psp_view = psp_menu_merged ? PSP_VIEW_MERGED : PSP_VIEW_TOP;
		else
			psp_view = psp_menu_merged ? PSP_VIEW_MERGED : PSP_VIEW_BOTH;
		last_state = game_state;
		last_merged = psp_menu_merged;
		last_paused = game_paused;
		last_complete = in_level_complete;

		cursor_x = ( psp_view == PSP_VIEW_MERGED ? GSP_SCREEN_HEIGHT_TOP : TOUCH_W ) / 2;
		cursor_y = TOUCH_H / 2;
		snap_pending = 3;
	}

	/* A touches in the menus (A would also fire the buttons bound to A,
	   whichever is selected), jumps when playing */
	if(( buttons & CONT_A ) && !DC_TouchShown()) keys |= KEY_A;
	if( buttons & CONT_B )  keys |= KEY_B;
	if( buttons & CONT_Y )  keys |= KEY_Y;
	if( buttons & CONT_X )  keys |= KEY_X;
	if( buttons & CONT_START ) keys |= KEY_START;
	if( rtrig > 127 ) keys |= KEY_R;
	if( ltrig > 127 ) keys |= KEY_L;
	if( buttons & CONT_DPAD_UP )    keys |= KEY_DUP;
	if( buttons & CONT_DPAD_DOWN )  keys |= KEY_DDOWN;
	if( buttons & CONT_DPAD_LEFT )  keys |= KEY_DLEFT;
	if( buttons & CONT_DPAD_RIGHT ) keys |= KEY_DRIGHT;

	ax = ax8 / 128.0f;
	ay = ay8 / 128.0f;
	if( ax > -0.25f && ax < 0.25f ) ax = 0;
	if( ay > -0.25f && ay < 0.25f ) ay = 0;

	circle_pos.dx = circle_pos.dy = 0;
	if( DC_TouchShown())
	{
		cursor_w = psp_view == PSP_VIEW_MERGED ? GSP_SCREEN_HEIGHT_TOP : TOUCH_W;
		cursor_x += ax * fabsf( ax ) * 6.0f;
		cursor_y += ay * fabsf( ay ) * 6.0f;
		if( cursor_x < 0 ) cursor_x = 0;
		if( cursor_y < 0 ) cursor_y = 0;
		if( cursor_x > cursor_w - 1 ) cursor_x = cursor_w - 1;
		if( cursor_y > TOUCH_H - 1 ) cursor_y = TOUCH_H - 1;
		if( ax || ay )
			cursor_used = true;
		if( ax || ay )
			cursor_by_dpad = false;

		if( snap_pending && --snap_pending <= 0 )
		{
			if( nav_count )
			{
				DC_Navigate( 0, 0 );
				cursor_by_dpad = true;
			}
			else
			{
				snap_pending = 1;
			}
		}

		if( cursor_by_dpad && !snap_pending && nav_count && !DC_TargetAt( cursor_x, cursor_y ))
			DC_Navigate( 0, 0 );

		{
			static const struct { u32 dc, key; int dx, dy; } dirs[4] = {
				{ CONT_DPAD_UP, KEY_DUP, 0, -1 }, { CONT_DPAD_DOWN, KEY_DDOWN, 0, 1 },
				{ CONT_DPAD_LEFT, KEY_DLEFT, -1, 0 }, { CONT_DPAD_RIGHT, KEY_DRIGHT, 1, 0 },
			};
			static int repeat;
			const PspTarget *at = DC_TargetAt( cursor_x, cursor_y );
			int i, j;

			if( buttons & ( CONT_DPAD_UP | CONT_DPAD_DOWN | CONT_DPAD_LEFT | CONT_DPAD_RIGHT ))
				repeat++;
			else
				repeat = 0;

			for( i = 0; i < 4; i++ )
			{
				bool bound = false;
				bool edge = ( buttons & dirs[i].dc ) && !( dc_old & dirs[i].dc );

				if( !( buttons & dirs[i].dc ))
					continue;

				for( j = 0; j < nav_count; j++ )
					if( nav[j].keys & dirs[i].key )
						bound = true;
				if( bound )
					continue;

				if( at && at->in_list && dirs[i].dy )
				{
					if( edge )
					{
						DC_Navigate( dirs[i].dx, dirs[i].dy );
						cursor_by_dpad = true;
					}
					continue;
				}

				keys &= ~dirs[i].key;

				if( at && at->slider && dirs[i].dx )
				{
					if( edge || ( repeat > 20 && !( repeat % 3 )))
					{
						psp_slider_focus = at->elem;
						psp_slider_nudge = dirs[i].dx;
					}
				}
				else if( edge )
				{
					if( !at )
						DC_Navigate( 0, 0 );
					else
						DC_Navigate( dirs[i].dx, dirs[i].dy );
					cursor_by_dpad = true;
				}
			}
		}

		if( buttons & CONT_A )
		{
			float tx = cursor_x, ty = cursor_y;

			if( psp_view == PSP_VIEW_MERGED )
			{
				float dx, dy, k;
				psp_get_merge( &dx, &dy, &k );
				tx = ( cursor_x - dx ) / k;
				ty = ( cursor_y - dy ) / k;
			}

			keys |= KEY_TOUCH;
			touch_pos.px = (u16)(s16)(int)tx;
			touch_pos.py = (u16)(s16)(int)ty;
			if( !touch_pos.px && !touch_pos.py )
				touch_pos.px = 1;
		}
	}
	else
	{
		circle_pos.dx = (s16)( ax * 156 );
		circle_pos.dy = (s16)( -ay * 156 );
		if( ax > 0.5f )  keys |= KEY_CPAD_RIGHT;
		if( ax < -0.5f ) keys |= KEY_CPAD_LEFT;
		if( ay > 0.5f )  keys |= KEY_CPAD_DOWN;
		if( ay < -0.5f ) keys |= KEY_CPAD_UP;
	}

	if( !( keys & KEY_TOUCH ))
		touch_pos.px = touch_pos.py = 0;

	keys_old = keys_held;
	keys_held = keys;

	entry[0] = keys;
	DC_PushSample( PAD_TICK_WORD, PAD_IDX_WORD, PAD_RING_WORD, 4, entry );
	tentry[0] = touch_pos.px | ( (u32)touch_pos.py << 16 );
	tentry[1] = ( keys & KEY_TOUCH ) ? 1 : 0;
	DC_PushSample( TOUCH_TICK_WORD, TOUCH_IDX_WORD, TOUCH_RING_WORD, 2, tentry );

	dc_old = buttons;
}

u32 hidKeysDown( void ) { return keys_held & ~keys_old; }
u32 hidKeysHeld( void ) { return keys_held; }
u32 hidKeysUp( void ) { return ~keys_held & keys_old; }
void hidTouchRead( touchPosition *pos ) { *pos = touch_pos; }
void hidCircleRead( circlePosition *pos ) { *pos = circle_pos; }

void PSP_DrawOverlay( void )
{
	float x, y;

	int screen = psp_view == PSP_VIEW_MERGED ? GFX_TOP : GFX_BOTTOM;
	const PspTarget *at;

	if( !DC_TouchShown() || !PSP_ScreenToPsp( screen, cursor_x, cursor_y, &x, &y ))
		return;

	if(( at = DC_TargetAt( cursor_x, cursor_y )))
	{
		float cx, cy, hw, hh, x0, y0, x1, y1;
		if( DC_TargetToCursor( at, &cx, &cy, &hw, &hh )
			&& PSP_ScreenToPsp( screen, cx - hw, cy - hh, &x0, &y0 )
			&& PSP_ScreenToPsp( screen, cx + hw, cy + hh, &x1, &y1 ))
		{
			u32 c = 0xc000ffff; /* yellow */
			PSP_DrawRect2D( x0 - 2, y0 - 2, x1 + 2, y0, c );
			PSP_DrawRect2D( x0 - 2, y1, x1 + 2, y1 + 2, c );
			PSP_DrawRect2D( x0 - 2, y0, x0, y1, c );
			PSP_DrawRect2D( x1, y0, x1 + 2, y1, c );
		}
	}

	PSP_DrawRect2D( x - 6, y - 1, x + 7, y + 2, 0xff000000 );
	PSP_DrawRect2D( x - 1, y - 6, x + 2, y + 7, 0xff000000 );
	PSP_DrawRect2D( x - 5, y, x + 6, y + 1, ( keys_held & KEY_TOUCH ) ? 0xff00ffff : 0xffffffff );
	PSP_DrawRect2D( x, y - 5, x + 1, y + 6, ( keys_held & KEY_TOUCH ) ? 0xff00ffff : 0xffffffff );
	(void)cursor_used;
}

/*
==============================================================================

Software keyboard: none, every request is cancelled

==============================================================================
*/
void swkbdInit( SwkbdState *swkbd, SwkbdType type, int numButtons, int maxTextLength )
{
	memset( swkbd, 0, sizeof( *swkbd ));
	swkbd->type = type;
	swkbd->num_buttons = numButtons;
	swkbd->max_len = maxTextLength;
}

void swkbdSetFeatures( SwkbdState *swkbd, u32 features ) { swkbd->features = features; }
void swkbdSetHintText( SwkbdState *swkbd, const char *text ) { snprintf( swkbd->hint, sizeof( swkbd->hint ), "%s", text ); }
void swkbdSetInitialText( SwkbdState *swkbd, const char *text ) { snprintf( swkbd->initial, sizeof( swkbd->initial ), "%s", text ); }
void swkbdSetValidation( SwkbdState *swkbd, SwkbdValidInput validInput, u32 filterFlags, u32 maxDigits )
{
	(void)swkbd; (void)validInput; (void)filterFlags; (void)maxDigits;
}
void swkbdSetButton( SwkbdState *swkbd, SwkbdButton button, const char *text, bool submit )
{
	(void)swkbd; (void)button; (void)text; (void)submit;
}
SwkbdButton swkbdInputText( SwkbdState *swkbd, char *buf, size_t bufsize )
{
	(void)swkbd;
	if( buf && bufsize )
		buf[0] = 0;
	return SWKBD_BUTTON_NONE;
}

/* the input ring has to be ready before the game polls it */
void __attribute__(( constructor )) dc_ctru_ctor( void )
{
	DC_InputInit();
}
