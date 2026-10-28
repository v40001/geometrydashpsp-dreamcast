/*
wav_dc.c - the game's sound effects, on the AICA (snd_sfx)

The PSP mixed them in software; the Dreamcast has 64 hardware channels and a
sample player. The .wav files are 16-bit PCM, which snd_sfx converts to AICA
ADPCM in sound RAM, so all of them fit in the AICA's 2 MB.
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <3ds.h>
#include <dc/sound/sfxmgr.h>
#include "dc_audio.h"
#include "../../source/wav_player.h"

bool load_wav( const char *path, SFX *sfx )
{
	FILE *f;
	char *buf;
	long n;

	memset( sfx, 0, sizeof( *sfx ));
	sfx->handle = -1;

	if( !( f = fopen( path, "rb" )))
		return false;
	fseek( f, 0, SEEK_END );
	n = ftell( f );
	fseek( f, 0, SEEK_SET );
	buf = n > 0 ? malloc( n ) : NULL;
	if( !buf || fread( buf, 1, n, f ) != (size_t)n )
	{
		free( buf );
		fclose( f );
		return false;
	}
	fclose( f );
	sfx->handle = snd_sfx_load_buf( buf );
	free( buf );
	if( sfx->handle < 0 )
	{
		sfx->handle = -1;
		return false;
	}
	return true;
}

void play_sfx( SFX *sfx, int channel )
{
	int vol;

	(void)channel;
	if( !sfx || sfx->handle < 0 )
		return;

	vol = (int)( dc_sfx_gain() * 255.0f );
	if( vol < 0 ) vol = 0;
	if( vol > 255 ) vol = 255;

	snd_sfx_play( sfx->handle, vol, 128 );
}
