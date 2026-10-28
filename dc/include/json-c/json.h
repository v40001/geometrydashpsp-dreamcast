/*
json-c/json.h - the small part of json-c the game uses

The PSP/3DS builds link the real json-c. The Dreamcast has no port of it in
this tree, and the game only needs objects, strings, numbers and booleans for
its config file, so dc/src/jsonc_dc.c has just those.
*/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

#define JSON_C_TO_STRING_PLAIN  0
#define JSON_C_TO_STRING_SPACED 2
#define JSON_C_TO_STRING_PRETTY 4

struct json_object *json_object_new_object(void);
struct json_object *json_object_new_string(const char *s);
struct json_object *json_object_new_int(int v);
struct json_object *json_object_new_double(double v);
struct json_object *json_object_new_boolean(int v);

void json_object_object_add(struct json_object *obj, const char *key, struct json_object *val);
int json_object_object_get_ex(const struct json_object *obj, const char *key, struct json_object **out);

const char *json_object_get_string(struct json_object *obj);
int json_object_get_int(struct json_object *obj);
double json_object_get_double(struct json_object *obj);
int json_object_get_boolean(struct json_object *obj);

struct json_object *json_tokener_parse(const char *text);
const char *json_object_to_json_string_ext(struct json_object *obj, int flags);
void json_object_put(struct json_object *obj);

#ifdef __cplusplus
}
#endif
