/*
jsonc_dc.c - objects, strings, numbers and booleans for the game's config

The game's config is written and read by itself: a tree of objects with a few
leaf values. This is that, and a parser/serializer, without json-c's arrays,
references or 30 years of compatibility.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <json-c/json.h>

enum { JT_OBJECT, JT_STRING, JT_INT, JT_DOUBLE, JT_BOOL };

typedef struct json_pair {
	char *key;
	struct json_object *val;
} json_pair;

struct json_object {
	int type;
	char *str;
	double num;
	int boolean;
	json_pair *items;
	int count, cap;
	char *ser;
};

static struct json_object *jo_new( int type )
{
	struct json_object *o = calloc( 1, sizeof( *o ));
	if( o )
		o->type = type;
	return o;
}

struct json_object *json_object_new_object( void ) { return jo_new( JT_OBJECT ); }

struct json_object *json_object_new_string( const char *s )
{
	struct json_object *o = jo_new( JT_STRING );
	if( o )
		o->str = strdup( s ? s : "" );
	return o;
}

struct json_object *json_object_new_int( int v )
{
	struct json_object *o = jo_new( JT_INT );
	if( o ) o->num = v;
	return o;
}

struct json_object *json_object_new_double( double v )
{
	struct json_object *o = jo_new( JT_DOUBLE );
	if( o ) o->num = v;
	return o;
}

struct json_object *json_object_new_boolean( int v )
{
	struct json_object *o = jo_new( JT_BOOL );
	if( o ) o->boolean = v != 0;
	return o;
}

void json_object_put( struct json_object *o )
{
	int i;

	if( !o )
		return;
	for( i = 0; i < o->count; i++ )
	{
		free( o->items[i].key );
		json_object_put( o->items[i].val );
	}
	free( o->items );
	free( o->str );
	free( o->ser );
	free( o );
}

void json_object_object_add( struct json_object *obj, const char *key, struct json_object *val )
{
	int i;

	if( !obj || obj->type != JT_OBJECT || !key )
		return;

	for( i = 0; i < obj->count; i++ )
		if( !strcmp( obj->items[i].key, key ))
		{
			json_object_put( obj->items[i].val );
			obj->items[i].val = val;
			free( obj->ser );
			obj->ser = NULL;
			return;
		}

	if( obj->count == obj->cap )
	{
		int cap = obj->cap ? obj->cap * 2 : 8;
		json_pair *items = realloc( obj->items, cap * sizeof( *items ));
		if( !items )
			return;
		obj->items = items;
		obj->cap = cap;
	}
	obj->items[obj->count].key = strdup( key );
	obj->items[obj->count].val = val;
	obj->count++;
	free( obj->ser );
	obj->ser = NULL;
}

int json_object_object_get_ex( const struct json_object *obj, const char *key, struct json_object **out )
{
	int i;

	if( out ) *out = NULL;
	if( !obj || obj->type != JT_OBJECT || !key )
		return 0;
	for( i = 0; i < obj->count; i++ )
		if( !strcmp( obj->items[i].key, key ))
		{
			if( out ) *out = obj->items[i].val;
			return 1;
		}
	return 0;
}

const char *json_object_get_string( struct json_object *o )
{
	if( !o )
		return "";
	if( o->type == JT_STRING )
		return o->str ? o->str : "";
	return "";
}

int json_object_get_int( struct json_object *o )
{
	if( !o ) return 0;
	if( o->type == JT_BOOL ) return o->boolean;
	return (int)o->num;
}

double json_object_get_double( struct json_object *o )
{
	if( !o ) return 0.0;
	if( o->type == JT_BOOL ) return o->boolean;
	return o->num;
}

int json_object_get_boolean( struct json_object *o )
{
	if( !o ) return 0;
	if( o->type == JT_BOOL ) return o->boolean;
	return o->num != 0.0;
}

/* ---- writing --------------------------------------------------------- */

static void jo_escape( char **out, const char *s )
{
	while( *s )
	{
		char c = *s++;
		if( c == '"' || c == '\\' )
		{
			*(*out)++ = '\\';
			*(*out)++ = c;
		}
		else if( c == '\n' )
		{
			*(*out)++ = '\\';
			*(*out)++ = 'n';
		}
		else if( c == '\t' )
		{
			*(*out)++ = '\\';
			*(*out)++ = 't';
		}
		else
			*(*out)++ = c;
	}
}

static void jo_indent( char **out, int depth )
{
	int i;
	for( i = 0; i < depth; i++ )
	{
		*(*out)++ = ' ';
		*(*out)++ = ' ';
	}
}

static void jo_write( char **out, struct json_object *o, int depth, int pretty )
{
	int i;

	switch( o->type )
	{
	case JT_OBJECT:
		*(*out)++ = '{';
		if( o->count )
		{
			for( i = 0; i < o->count; i++ )
			{
				if( i ) *(*out)++ = ',';
				if( pretty )
				{
					*(*out)++ = '\n';
					jo_indent( out, depth + 1 );
				}
				*(*out)++ = '"';
				jo_escape( out, o->items[i].key );
				*(*out)++ = '"';
				*(*out)++ = ':';
				if( pretty ) *(*out)++ = ' ';
				jo_write( out, o->items[i].val, depth + 1, pretty );
			}
			if( pretty )
			{
				*(*out)++ = '\n';
				jo_indent( out, depth );
			}
		}
		*(*out)++ = '}';
		break;
	case JT_STRING:
		*(*out)++ = '"';
		jo_escape( out, o->str ? o->str : "" );
		*(*out)++ = '"';
		break;
	case JT_INT:
		*out += sprintf( *out, "%d", (int)o->num );
		break;
	case JT_DOUBLE:
		*out += sprintf( *out, "%.6g", o->num );
		break;
	case JT_BOOL:
		*out += sprintf( *out, o->boolean ? "true" : "false" );
		break;
	}
}

const char *json_object_to_json_string_ext( struct json_object *o, int flags )
{
	char *buf, *p;
	size_t size = 256;

	if( !o )
		return "{}";
	if( o->ser )
		return o->ser;

	/* a generous bound: every node and key gets room */
	{
		struct json_object *stack[64];
		int n = 0;
		stack[n++] = o;
		while( n )
		{
			struct json_object *cur = stack[--n];
			size += 256;
			if( cur->type == JT_OBJECT )
			{
				int i;
				for( i = 0; i < cur->count && n < 63; i++ )
					stack[n++] = cur->items[i].val;
			}
		}
	}

	buf = malloc( size + 64 );
	if( !buf )
		return "{}";
	p = buf;
	jo_write( &p, o, 0, flags & JSON_C_TO_STRING_PRETTY );
	*p = 0;
	o->ser = buf;
	return o->ser;
}

/* ---- parsing --------------------------------------------------------- */

typedef struct { const char *p; int ok; } jparse;

static void jp_ws( jparse *j )
{
	while( *j->p && isspace( (unsigned char)*j->p ))
		j->p++;
}

static struct json_object *jp_value( jparse *j );

static char *jp_string( jparse *j )
{
	char *out, *o;
	int n = 0;

	if( *j->p != '"' )
	{
		j->ok = 0;
		return NULL;
	}
	j->p++;
	out = malloc( strlen( j->p ) + 1 );
	if( !out )
	{
		j->ok = 0;
		return NULL;
	}
	o = out;
	while( *j->p && *j->p != '"' )
	{
		char c = *j->p++;
		if( c == '\\' )
		{
			c = *j->p++;
			switch( c )
			{
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'u':
				/* the config is ASCII; keep the code point's low byte */
				if( j->p[0] && j->p[1] && j->p[2] && j->p[3] )
				{
					char hex[5] = { j->p[0], j->p[1], j->p[2], j->p[3], 0 };
					c = (char)strtol( hex, NULL, 16 );
					j->p += 4;
				}
				break;
			default: break;
			}
		}
		o[n++] = c;
	}
	if( *j->p != '"' )
	{
		free( out );
		j->ok = 0;
		return NULL;
	}
	j->p++;
	o[n] = 0;
	return out;
}

static struct json_object *jp_value( jparse *j )
{
	jp_ws( j );

	if( *j->p == '{' )
	{
		struct json_object *obj = json_object_new_object();
		j->p++;
		jp_ws( j );
		if( *j->p == '}' )
		{
			j->p++;
			return obj;
		}
		while( *j->p && j->ok )
		{
			char *key = jp_string( j );
			struct json_object *val;

			if( !key )
				break;
			jp_ws( j );
			if( *j->p != ':' )
			{
				free( key );
				j->ok = 0;
				break;
			}
			j->p++;
			val = jp_value( j );
			if( !val )
			{
				free( key );
				break;
			}
			json_object_object_add( obj, key, val );
			free( key );
			jp_ws( j );
			if( *j->p == ',' )
			{
				j->p++;
				jp_ws( j );
				continue;
			}
			break;
		}
		jp_ws( j );
		if( *j->p != '}' )
			j->ok = 0;
		else
			j->p++;
		return obj;
	}
	if( *j->p == '[' )
	{
		/* arrays are accepted and dropped: the config has none */
		int depth = 0;
		while( *j->p )
		{
			if( *j->p == '[' ) depth++;
			else if( *j->p == ']' && --depth == 0 ) { j->p++; break; }
			j->p++;
		}
		return json_object_new_object();
	}
	if( *j->p == '"' )
	{
		char *s = jp_string( j );
		struct json_object *o = s ? json_object_new_string( s ) : NULL;
		free( s );
		return o;
	}
	if( !strncmp( j->p, "true", 4 ))
	{
		j->p += 4;
		return json_object_new_boolean( 1 );
	}
	if( !strncmp( j->p, "false", 5 ))
	{
		j->p += 5;
		return json_object_new_boolean( 0 );
	}
	if( !strncmp( j->p, "null", 4 ))
	{
		j->p += 4;
		return json_object_new_int( 0 );
	}
	{
		const char *start = j->p;
		char *end;
		double v = strtod( j->p, &end );
		int is_float = 0;
		const char *q;

		if( end == j->p )
		{
			j->ok = 0;
			return NULL;
		}
		for( q = start; q < end; q++ )
			if( *q == '.' || *q == 'e' || *q == 'E' )
				is_float = 1;
		j->p = end;
		return is_float ? json_object_new_double( v ) : json_object_new_int( (int)v );
	}
}

struct json_object *json_tokener_parse( const char *text )
{
	jparse j = { text, 1 };
	struct json_object *o;

	if( !text )
		return NULL;
	o = jp_value( &j );
	if( !j.ok )
	{
		json_object_put( o );
		return NULL;
	}
	return o;
}
