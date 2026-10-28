/*
mp3_dc.c - the game's music, from the PS2 port's 4-bit IMA ADPCM .wav files

The 3DS (and the PSP port) decoded MP3. The Dreamcast has neither the CPU nor
the RAM to spare for that, and the PS2 port already ships every song as IMA
ADPCM at 32 kHz: 4 bits a sample, a quarter of PCM, which is what the
Dreamcast's CD can stream without a hitch. This decodes it into the AICA
stream (snd_stream), in the poll thread.

The rest of the interface is mp3_player.h's: play/stop/pause/seek, and the
amplitude the player's trail and the menu's equalizer follow.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>

#include <kos.h>
#include <3ds.h>
#include <dc/sound/stream.h>

#include "mp3_player.h"
#include "math_helpers.h"
#include "main.h"
#include "menus/settings.h"
#include "state.h"
#include "dc_audio.h"
#include "dc_port.h"

#define MUSIC_BUF_SIZE   ( 16 * 1024 )   /* per channel, ~0.5 s at 32 kHz */
#define MUSIC_PCM_MAX    ( 64 * 1024 )   /* interleaved bytes, one poll */
#define MUSIC_BLOCK_MAX  4096            /* frames in an IMA block */

static const int ima_step[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
	11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767
};
static const int ima_index[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8
};

volatile float amplitude = 0;

static snd_stream_hnd_t stream = SND_STREAM_INVALID;
static pthread_t poll_thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool quit_thread;
static bool thread_running;

static FILE *song;
static int music_ch = 2;
static int music_rate = 32000;
static long data_start, data_size;
static int block_align, samples_per_block;
static long total_blocks;
static volatile bool playing, paused, looping;

static int64_t next_block;
static int16_t block[MUSIC_BLOCK_MAX * 2];
static int block_frames, block_pos;
static int64_t block_first_sample;

static int16_t pcm[MUSIC_PCM_MAX / 2];

static int16_t rd16( const u8 *p ) { return (int16_t)( p[0] | ( p[1] << 8 )); }
static u32 rd32( const u8 *p ) { return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ((u32)p[3] << 24 ); }

/* ---- the game's amplitude (as in the 3DS/PSP player) ----------------- */

static float calculate_amplitude( float power )
{
	if( state.practice_mode && !settingsState.practiceMusicSync ) return 0.0f;
	if( game_state == STATE_MAIN_MENU ) return 0.5f;

	{
		static float prev, pulse, avg_delta, prev_power;

		float delta = power - prev;
		float abs_delta = fabsf( delta );
		float rms_delta = power - prev_power;
		float abs_rms_delta = fabsf( rms_delta );
		float rms_thresh;

		avg_delta = avg_delta + (( abs_delta - avg_delta ) * 0.1f );
		rms_thresh = avg_delta * POWER_THRESH_MULTIPLIER;

		if( abs_rms_delta > rms_thresh && rms_delta > 0.0f )
			pulse = AMP_MAX;

		pulse *= AMP_I_DECAY;
		if( pulse > AMP_MAX ) pulse = AMP_MAX;
		if( pulse < AMP_MIN ) pulse = AMP_MIN;

		prev = power;
		prev_power = power;
		return pulse;
	}
}

static float calculate_power( const int16_t *samples, size_t frames, int ch )
{
	uint64_t sum = 0;
	size_t i;

	for( i = 0; i < frames; i++ )
	{
		int16_t mono = ch == 2 ? (int16_t)(( samples[i * 2] + samples[i * 2 + 1] ) / 2 ) : samples[i];
		sum += (int32_t)mono * mono;
	}
	if( !frames )
		return 0.0f;
	return ((float)sum / frames ) / ( 32768.0f * 32768.0f );
}

/* ---- IMA ADPCM ------------------------------------------------------- */

static int decode_block( int64_t index )
{
	u8 raw[2048];
	int pred[2] = { 0, 0 }, step_index[2] = { 0, 0 };
	int n = 0, ch, i, j;
	const u8 *p;
	int frames;

	if( !song || index >= total_blocks )
		return 0;
	if( fseek( song, data_start + (long)index * block_align, SEEK_SET ) != 0 )
		return 0;
	if( fread( raw, 1, block_align, song ) != (size_t)block_align )
		return 0;

	p = raw;
	for( ch = 0; ch < music_ch; ch++ )
	{
		pred[ch] = rd16( p );
		step_index[ch] = p[2];
		if( step_index[ch] > 88 ) step_index[ch] = 88;
		p += 4;
	}

	frames = samples_per_block;
	if( frames > MUSIC_BLOCK_MAX )
		frames = MUSIC_BLOCK_MAX;

	for( ch = 0; ch < music_ch; ch++ )
		block[ch] = (int16_t)pred[ch];
	n = 1;

	while( n < frames && p + 4 * music_ch <= raw + block_align )
	{
		int base = n;
		for( ch = 0; ch < music_ch; ch++ )
		{
			n = base;
			for( j = 0; j < 8 && n < frames; j++ )
			{
				int nib = ( j & 1 ) ? ( p[j >> 1] >> 4 ) : ( p[j >> 1] & 15 );
				int step = ima_step[step_index[ch]];
				int diff = step >> 3;

				if( nib & 1 ) diff += step >> 2;
				if( nib & 2 ) diff += step >> 1;
				if( nib & 4 ) diff += step;
				if( nib & 8 ) diff = -diff;

				pred[ch] += diff;
				if( pred[ch] > 32767 ) pred[ch] = 32767;
				if( pred[ch] < -32768 ) pred[ch] = -32768;
				step_index[ch] += ima_index[nib & 7];
				if( step_index[ch] < 0 ) step_index[ch] = 0;
				if( step_index[ch] > 88 ) step_index[ch] = 88;

				block[n * music_ch + ch] = (int16_t)pred[ch];
				n++;
			}
			p += 4;
		}
	}

	block_frames = n;
	block_pos = 0;
	block_first_sample = index * samples_per_block;
	return n;
}

static void music_seek_locked( float time )
{
	int64_t sample = (int64_t)( time * music_rate );
	int64_t blk = sample / samples_per_block;
	int skip = (int)( sample % samples_per_block );

	if( blk < 0 ) blk = 0;
	if( blk >= total_blocks ) blk = 0;

	next_block = blk;
	block_frames = block_pos = 0;
	decode_block( next_block++ );
	block_pos = skip < block_frames ? skip : block_frames;
}

static void *music_cb( snd_stream_hnd_t hnd, int bytes_req, int *bytes_recv )
{
	int want, done = 0;

	(void)hnd;
	want = bytes_req / ( 2 * music_ch );
	if( want > MUSIC_PCM_MAX / ( 2 * music_ch ))
		want = MUSIC_PCM_MAX / ( 2 * music_ch );

	while( done < want )
	{
		int avail, n;

		if( block_pos >= block_frames )
		{
			if( next_block >= total_blocks )
			{
				if( !looping )
					break;
				next_block = 0;
				block_frames = block_pos = 0;
			}
			if( !decode_block( next_block++ ))
				break;
		}

		avail = block_frames - block_pos;
		n = want - done < avail ? want - done : avail;
		memcpy( pcm + done * music_ch, block + block_pos * music_ch, n * music_ch * 2 );
		block_pos += n;
		done += n;
	}

	*bytes_recv = done * music_ch * 2;

	if( done > 0 )
		amplitude = calculate_amplitude( calculate_power( pcm, done, music_ch ));
	return pcm;
}

/* ---- the player ------------------------------------------------------ */

static void *poll_entry( void *arg )
{
	(void)arg;
	while( !quit_thread )
	{
		if( playing && !paused && stream != SND_STREAM_INVALID )
		{
			pthread_mutex_lock( &lock );
			snd_stream_volume( stream, (int)( dc_music_gain() * 255.0f ));
			snd_stream_poll( stream );
			pthread_mutex_unlock( &lock );
		}
		thd_sleep( 8 );
	}
	return NULL;
}

void audio_init( void )
{
	if( stream != SND_STREAM_INVALID )
		return;

	stream = snd_stream_alloc( music_cb, MUSIC_BUF_SIZE );
	if( stream == SND_STREAM_INVALID )
	{
		psp_log( "snd_stream_alloc failed\n" );
		return;
	}

	quit_thread = false;
	if( pthread_create( &poll_thread, NULL, poll_entry, NULL ) == 0 )
		thread_running = true;
}

void audio_exit( void )
{
	if( thread_running )
	{
		quit_thread = true;
		pthread_join( poll_thread, NULL );
		thread_running = false;
	}
	if( stream != SND_STREAM_INVALID )
	{
		snd_stream_destroy( stream );
		stream = SND_STREAM_INVALID;
	}
}

/* .mp3 in the game's paths, .wav in the PS2 port's romfs */
static void song_path( const char *in, char *out, size_t n )
{
	size_t len;

	snprintf( out, n, "%s", in );
	len = strlen( out );
	if( len > 4 && !strcasecmp( out + len - 4, ".mp3" ))
		strcpy( out + len - 4, ".wav" );
}

static int parse_wav( FILE *f )
{
	u8 hdr[12], chunk[8];
	u8 fmt[40];
	bool have_fmt = false;

	if( fread( hdr, 1, 12, f ) != 12 || memcmp( hdr, "RIFF", 4 ) || memcmp( hdr + 8, "WAVE", 4 ))
		return 0;

	while( fread( chunk, 1, 8, f ) == 8 )
	{
		u32 size = rd32( chunk + 4 );

		if( !memcmp( chunk, "fmt ", 4 ))
		{
			if( size > sizeof( fmt ) || fread( fmt, 1, size, f ) != size )
				return 0;
			if( rd16( fmt ) != 0x11 ) /* IMA ADPCM */
				return 0;
			music_ch = rd16( fmt + 2 );
			music_rate = rd32( fmt + 4 );
			block_align = rd16( fmt + 12 );
			if( size >= 20 )
				samples_per_block = rd16( fmt + 18 );
			else
				samples_per_block = ( block_align - 4 * music_ch ) * 2 / music_ch + 1;
			if( music_ch < 1 || music_ch > 2 || block_align <= 4 * music_ch || samples_per_block < 2 )
				return 0;
			have_fmt = true;
		}
		else if( !memcmp( chunk, "data", 4 ))
		{
			if( !have_fmt )
				return 0;
			data_start = ftell( f );
			data_size = size;
			total_blocks = size / block_align;
			return total_blocks > 0;
		}
		else
			fseek( f, size + ( size & 1 ), SEEK_CUR );
	}
	return 0;
}

int play_mp3( char *path, bool loop, float seek )
{
	char mapped[256];
	FILE *f;

	song_path( path, mapped, sizeof( mapped ));
	if( access( mapped, F_OK ) != 0 )
		return 0;

	stop_mp3();
	audio_init();
	if( stream == SND_STREAM_INVALID )
		return 0;

	f = fopen( mapped, "rb" );
	if( !f )
	{
		psp_log( "music: can't open %s\n", mapped );
		return 0;
	}
	if( !parse_wav( f ))
	{
		psp_log( "music: %s is not IMA ADPCM\n", mapped );
		fclose( f );
		return 0;
	}

	pthread_mutex_lock( &lock );
	song = f;
	looping = loop;
	next_block = 0;
	block_frames = block_pos = 0;
	if( seek > 0 )
		music_seek_locked( seek );

	playing = true;
	paused = false;
	amplitude = 0.5f;

	snd_stream_start( stream, music_rate, music_ch == 2 );
	pthread_mutex_unlock( &lock );
	return 1;
}

int play_mp3_buf( void *buf, size_t sz, bool loop, float seek )
{
	(void)buf; (void)sz; (void)loop; (void)seek;
	return 0;
}

void seek_mp3( float time )
{
	if( !playing || !song )
		return;

	pthread_mutex_lock( &lock );
	if( time < 0 ) time = 0;
	music_seek_locked( time );
	snd_stream_stop( stream );
	snd_stream_start( stream, music_rate, music_ch == 2 );
	pthread_mutex_unlock( &lock );
}

void stop_mp3( void )
{
	pthread_mutex_lock( &lock );
	if( playing )
	{
		playing = false;
		paused = false;
		if( stream != SND_STREAM_INVALID )
			snd_stream_stop( stream );
	}
	if( song )
	{
		fclose( song );
		song = NULL;
	}
	amplitude = 0.5f;
	pthread_mutex_unlock( &lock );
}

/* the AICA keeps looping its buffer unless the channel is stopped */
static void set_paused( bool p )
{
	pthread_mutex_lock( &lock );
	if( playing && stream != SND_STREAM_INVALID && p != paused )
	{
		if( p )
			snd_stream_stop( stream );
		else
			snd_stream_start( stream, music_rate, music_ch == 2 );
	}
	paused = p;
	pthread_mutex_unlock( &lock );
}

void toggle_playback_mp3( void ) { set_paused( !paused ); }
void pause_playback_mp3( void ) { set_paused( true ); }
void unpause_playback_mp3( void ) { set_paused( false ); }

void fade_to_amplitude( float new_amplitude )
{
	amplitude = calculate_amplitude( new_amplitude );
}
