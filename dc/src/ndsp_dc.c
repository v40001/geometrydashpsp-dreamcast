/*
ndsp_dc.c - the part of libctru's DSP the game uses, on the Dreamcast's AICA

The game's own mixer was only used for two things: the music (a stream, see
mp3_dc.c) and the sound effects (dc/src/wav_dc.c, snd_sfx). What is left here
is the channel gain table those two read, and stubs for the calls the
replaced players used to make.
*/
#include <string.h>
#include <3ds.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>
#include "dc_audio.h"

static float chan_gain[NDSP_NUM_CHANNELS];

Result ndspInit( void )
{
	int i;

	for( i = 0; i < NDSP_NUM_CHANNELS; i++ )
		chan_gain[i] = 1.0f;

	if( snd_stream_init() < 0 )
		return -1;
	return 0;
}

void ndspExit( void )
{
	snd_stream_shutdown();
	snd_shutdown();
}

void ndspSetOutputMode( ndspOutputMode mode ) { (void)mode; }
void ndspSetCallback( ndspCallback callback, void *data ) { (void)callback; (void)data; }

void ndspChnReset( int id ) { (void)id; }
void ndspChnSetInterp( int id, ndspInterpType type ) { (void)id; (void)type; }
void ndspChnSetRate( int id, float rate ) { (void)id; (void)rate; }
void ndspChnSetFormat( int id, u16 format ) { (void)id; (void)format; }
void ndspChnSetPaused( int id, bool paused ) { (void)id; (void)paused; }
bool ndspChnIsPaused( int id ) { (void)id; return false; }
bool ndspChnIsPlaying( int id ) { (void)id; return false; }
void ndspChnWaveBufAdd( int id, ndspWaveBuf *buf ) { (void)id; (void)buf; }
void ndspChnWaveBufClear( int id ) { (void)id; }

void ndspChnSetMix( int id, float mix[12] )
{
	if( id < 0 || id >= NDSP_NUM_CHANNELS )
		return;
	/* the game puts the left and right gains in the first two slots */
	chan_gain[id] = ( mix[0] + mix[1] ) * 0.5f;
}

float dc_sfx_gain( void )
{
	int i;
	float g = 0;

	/* the effects are on channels 1..7, all with the same gain */
	for( i = 1; i <= 7 && i < NDSP_NUM_CHANNELS; i++ )
		g = chan_gain[i];
	return g;
}

float dc_music_gain( void )
{
	return chan_gain[0];
}
