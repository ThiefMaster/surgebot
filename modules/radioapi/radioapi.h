#ifndef RADIOAPI_H
#define RADIOAPI_H

#include <json/json.h>

typedef void (radioapi_func)(const char *api, json_object *payload);
void radioapi_call_cb(const char *api, const char *payload, radioapi_func *cb);
#define radioapi_call(API, PAYLOAD)	radioapi_call_cb((API), (PAYLOAD), NULL)

#endif
