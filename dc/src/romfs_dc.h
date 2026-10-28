/*
romfs_dc.h - the disc's files, read without KOS's ISO9660 driver
*/
#pragma once

#include <stdio.h>
#include <sys/stat.h>

int dc_romfs_init( void );
FILE *dc_romfs_fopen( const char *path );
int dc_romfs_stat( const char *path, struct stat *st );
