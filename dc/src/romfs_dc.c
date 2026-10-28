/*
romfs_dc.c - the disc's files, without KallistiOS's ISO9660 driver

KOS's fs_iso9660 hangs on the first file open in this setup (its per-disc
init waits on the drive), while raw sector reads work. The game only needs to
read files by name from the disc, so this reads the ISO9660 directory once at
startup (Joliet names, the ones mkisofs -J writes) into a table, and opens
files through funopen() over cdrom_read_sectors_ex().
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include <kos.h>
#include <dc/cdrom.h>

#include "romfs_dc.h"
#include "dc_port.h"

#define ROMFS_MAX_FILES 700
#define ROMFS_PATH_MAX  192

typedef struct {
	char path[ROMFS_PATH_MAX];
	uint32_t extent;
	uint32_t size;
} RomFile;

static RomFile files[ROMFS_MAX_FILES];
static int file_count;

static uint8_t sector[2048] __attribute__((aligned(32)));
static uint8_t readbuf[32 * 2048] __attribute__((aligned(32)));

static uint32_t rd32( const uint8_t *p )
{
	return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ((uint32_t)p[3] << 24 );
}

/* extents on the disc are absolute LBAs; FAD = LBA + 150 */
static const uint32_t session_base = 150;
/* where the volume starts: LBA 0 on a CD image, 45000 on a GD-ROM (GDI) */
static uint32_t volume_lba;

static int read_sectors( uint32_t lba, uint32_t count, void *out )
{
	if( cdrom_read_sectors( out, lba + session_base, count ) < 0 )
		return -1;
	return 0;
}

/* one ISO9660 directory record -> name (Joliet UCS-2, ASCII here) */
static int dirent_name( const uint8_t *de, char *out, size_t n, int joliet )
{
	int len = de[32];
	int i, o = 0;

	if( joliet )
	{
		for( i = 0; i + 1 < len && o < (int)n - 1; i += 2 )
		{
			uint16_t c = ( de[33 + i] << 8 ) | de[33 + i + 1];
			out[o++] = ( c < 128 ) ? (char)c : '?';
		}
	}
	else
	{
		for( i = 0; i < len && o < (int)n - 1; i++ )
		{
			if( de[33 + i] == ';' )
				break;
			out[o++] = (char)de[33 + i];
		}
	}
	out[o] = 0;
	return o;
}

static void walk_dir( uint32_t extent, uint32_t size, const char *prefix, int joliet, int depth )
{
	uint32_t pos = 0;

	if( depth > 6 )
		return;

	while( pos < size && file_count < ROMFS_MAX_FILES )
	{
		uint32_t lba = extent + pos / 2048;
		uint32_t off = pos % 2048;
		const uint8_t *de;
		char name[128], path[ROMFS_PATH_MAX];
		uint32_t dsize, dirsize, reclen;
		int isdir;

		if( read_sectors( lba, 1, sector ) < 0 )
			return;
		de = sector + off;
		if( de[0] == 0 )
		{
			/* records never cross a sector: skip to the next one */
			pos = ( pos / 2048 + 1 ) * 2048;
			continue;
		}
		if( de[0] < 34 || off + de[0] > 2048 )
			return;

		dsize = rd32( de + 2 );
		reclen = de[0];
		dirsize = rd32( de + 10 );
		isdir = de[25] & 2;
		dirent_name( de, name, sizeof( name ), joliet );

		if( name[0] && strcmp( name, "." ) && strcmp( name, ".." ))
		{
			snprintf( path, sizeof( path ), "%s/%s", prefix, name );
			if( isdir )
				walk_dir( dsize, dirsize, path, joliet, depth + 1 );
			else
			{
				snprintf( files[file_count].path, sizeof( files[file_count].path ), "%s", path );
				files[file_count].extent = dsize;
				files[file_count].size = dirsize;
				file_count++;
			}
		}

		pos += reclen;
	}
}

int dc_romfs_init( void )
{
	uint32_t root_extent, root_size;
	int joliet = 0;

	file_count = 0;
	{
		cd_toc_t toc;
		uint32_t base;
		if( cdrom_read_toc( &toc, true ) == 0 && ( base = cdrom_locate_data_track( &toc )))
			volume_lba = base - 150;
		else if( cdrom_read_toc( &toc, false ) == 0 && ( base = cdrom_locate_data_track( &toc )))
			volume_lba = base - 150;
	}

	/* primary volume descriptor at LBA 16 */
	if( read_sectors( volume_lba + 16, 1, sector ) < 0 || memcmp( sector, "\x01" "CD001", 6 ))
		return -1;

	/* a Joliet one after it, if mkisofs wrote one */
	{
		uint32_t lba;
		for( lba = volume_lba + 17; lba < volume_lba + 20; lba++ )
		{
			if( read_sectors( lba, 1, sector ) < 0 )
				break;
			if( !memcmp( sector, "\x02" "CD001", 6 ))
			{
				joliet = 1;
				break;
			}
		}
	}

	root_extent = rd32( sector + 156 + 2 );
	root_size = rd32( sector + 156 + 10 );
	if( !root_extent || !root_size )
		return -1;

	psp_log( "romfs: joliet %d, root %u/%u\n", joliet, root_extent, root_size );
	walk_dir( root_extent, root_size, "", joliet, 0 );
	{
		int i;
		psp_log( "romfs: %d files\n", file_count );
		for( i = 0; i < file_count && i < 8; i++ )
			psp_log( "romfs:   %s\n", files[i].path );
	}
	return 0;
}

static RomFile *find_file( const char *path )
{
	char full[ROMFS_PATH_MAX];
	int i;

	/* the game says gfx/ui.dcs; the table has /romfs/gfx/ui.dcs */
	if( path[0] == '/' )
		snprintf( full, sizeof( full ), "%s", path );
	else
		snprintf( full, sizeof( full ), "/romfs/%s", path );

	for( i = 0; i < file_count; i++ )
		if( !strcmp( files[i].path, full ))
			return &files[i];
	return NULL;
}

/* ---- stdio over the disc -------------------------------------------- */

typedef struct { uint32_t extent, size, pos; } RomCookie;

static mutex_t rom_mtx = MUTEX_INITIALIZER;

static int rom_read( void *cookie, char *buf, int len )
{
	RomCookie *c = cookie;
	int done = 0;

	mutex_lock_scoped( &rom_mtx );

	while( done < len && c->pos < c->size )
	{
		uint32_t off = c->pos % 2048;
		uint32_t lba = c->extent + c->pos / 2048;
		uint32_t want = (uint32_t)( len - done );
		uint32_t nsec = ( off + want + 2047 ) / 2048;
		uint32_t n;

		if( nsec > 8 )
			nsec = 8;
		if( read_sectors( lba, nsec, readbuf ) < 0 )
			break;

		n = nsec * 2048 - off;
		if( n > want ) n = want;
		if( c->pos + n > c->size ) n = c->size - c->pos;
		memcpy( buf + done, readbuf + off, n );
		done += n;
		c->pos += n;
		thd_pass();
	}
	return done;
}

static fpos_t rom_seek( void *cookie, fpos_t off, int whence )
{
	RomCookie *c = cookie;
	int64_t pos = (int64_t)off;

	if( whence == SEEK_CUR ) pos += c->pos;
	else if( whence == SEEK_END ) pos += c->size;
	if( pos < 0 ) pos = 0;
	if( pos > c->size ) pos = c->size;
	c->pos = (uint32_t)pos;
	return (fpos_t)c->pos;
}

static int rom_close( void *cookie )
{
	free( cookie );
	return 0;
}

FILE *dc_romfs_fopen( const char *path )
{
	RomFile *f = find_file( path );
	RomCookie *c;

	if( !f )
		return NULL;
	c = calloc( 1, sizeof( *c ));
	if( !c )
		return NULL;
	c->extent = f->extent;
	c->size = f->size;
	return funopen( c, rom_read, NULL, rom_seek, rom_close );
}

int dc_romfs_stat( const char *path, struct stat *st )
{
	RomFile *f = find_file( path );

	if( !f )
		return -1;
	memset( st, 0, sizeof( *st ));
	st->st_mode = S_IFREG | 0644;
	st->st_size = f->size;
	return 0;
}
