#include "global.h"
#include "module.h"
#include "conf.h"
#include "ptrlist.h"
#include "modules/scripting/scripting.h"

#define USE_GC_ZEAL
#ifdef USE_GC_ZEAL
	#define DEBUG
	#define JS_NO_JSVAL_JSID_STRUCT_TYPES
#endif

#define XP_UNIX
#include <js/jsapi.h>

MODULE_DEPENDS("scripting", NULL);

static void scripting_js_error(JSContext *cx, const char *message, JSErrorReport *report);
static JSBool scripting_js_print(JSContext *cx, uintN argc, jsval *vp);
static JSBool scripting_js_call(JSContext *cx, uintN argc, jsval *vp);
static JSBool scripting_js_register(JSContext *cx, uintN argc, jsval *vp);
static JSBool scripting_js_unregister(JSContext *cx, uintN argc, jsval *vp);
static struct dict *js_caller(JSObject *jsfunc, struct dict *args, struct module *module);
static void js_freeer(JSObject *jsfunc, JSObject **funcp);
static JSObject *js_taker(JSObject *jsfunc, JSObject **funcp);
static struct dict *args_from_js(JSObject *jsargs);
static struct scripting_arg *arg_from_js(jsval *value);
static jsval args_to_js(struct dict *args);
static jsval arg_to_js(struct scripting_arg *arg);
static JSBool scripting_js_call_callable(JSContext *cx, uintN argc, jsval *vp);
static jsval callable_arg_to_js(struct scripting_arg *arg);

static struct module *this;
static struct ptrlist *script_roots;
static JSRuntime *rt;
static JSContext *cx;
static JSObject *global, *js_surgebot;

static JSClass global_class = { "global", JSCLASS_GLOBAL_FLAGS, JS_PropertyStub, JS_PropertyStub,
	JS_PropertyStub, JS_StrictPropertyStub, JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub,
	JS_FinalizeStub, JSCLASS_NO_OPTIONAL_MEMBERS };

static JSClass resource_class = { "Resource", JSCLASS_HAS_PRIVATE, JS_PropertyStub, JS_PropertyStub,
	JS_PropertyStub, JS_StrictPropertyStub, JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub,
	JS_FinalizeStub, JSCLASS_NO_OPTIONAL_MEMBERS };

static JSFunctionSpec js_surgebot_functions[] = {
    JS_FS("call", scripting_js_call, 2, JSPROP_READONLY | JSPROP_PERMANENT),
    JS_FS("register", scripting_js_register, 2, JSPROP_READONLY | JSPROP_PERMANENT),
    JS_FS("unregister", scripting_js_unregister, 1, JSPROP_READONLY | JSPROP_PERMANENT),
    JS_FS_END
};


MODULE_INIT
{
	this = self;

	rt = JS_NewRuntime(8L * 1024L * 1024L);
	cx = JS_NewContext(rt, 8192);
	JS_SetOptions(cx, JSOPTION_STRICT | JSOPTION_VAROBJFIX);
	JS_SetVersion(cx, JSVERSION_LATEST);
	JS_SetErrorReporter(cx, scripting_js_error);
	#ifdef USE_GC_ZEAL
		JS_SetGCZeal(cx, 2);
	#endif
	JS_BeginRequest(cx);

	global = JS_NewCompartmentAndGlobalObject(cx, &global_class, NULL);
	JS_InitStandardClasses(cx, global);

	JS_DefineFunction(cx, global, "print", scripting_js_print, 1, JSPROP_READONLY | JSPROP_PERMANENT);
	js_surgebot = JS_NewObject(cx, NULL, NULL, NULL);
	JS_DefineFunctions(cx, js_surgebot, js_surgebot_functions);
	JS_DefineProperty(cx, global, "surgebot", OBJECT_TO_JSVAL(js_surgebot), NULL, NULL, JSPROP_READONLY | JSPROP_ENUMERATE | JSPROP_PERMANENT);


	struct stringlist *scripts = conf_get("scripting/javascript/scripts", DB_STRINGLIST);
	script_roots = ptrlist_create();
	if(scripts) {
		for(unsigned int i = 0; i < scripts->count; i++) {
			JSObject *script = JS_CompileFile(cx, global, scripts->data[i]);
			if(!script) {
				continue;
			}
			JSObject **scriptp = malloc(sizeof(JSObject *));
			*scriptp = script;
			JS_AddObjectRoot(cx, scriptp);
			ptrlist_add(script_roots, 0, scriptp);
			JS_ExecuteScript(cx, global, script, NULL);
		}
		JS_MaybeGC(cx);
	}
}

MODULE_FINI
{
	for(unsigned int i = 0; i < script_roots->count; i++) {
		JS_RemoveObjectRoot(cx, script_roots->data[i]->ptr);
	}
	ptrlist_free(script_roots);
	JS_EndRequest(cx);
	JS_DestroyContext(cx);
	JS_DestroyRuntime(rt);
	JS_ShutDown();
}

static void scripting_js_error(JSContext *cx, const char *message, JSErrorReport *report)
{
	log_append(LOG_WARNING, "JS error in %s:%u: %s", report->filename ? report->filename : "<no filename>", report->lineno, message);
}

static JSBool scripting_js_print(JSContext *cx, uintN argc, jsval *vp)
{
	jsval *argv;
	JSString *str;
	char *bytes;

	argv = JS_ARGV(cx, vp);
	for(uintN i = 0; i < argc; i++) {
		if(JSVAL_IS_OBJECT(argv[i]) && !JS_InstanceOf(cx, JSVAL_TO_OBJECT(argv[i]), &resource_class, NULL)) {
			str = JS_ValueToSource(cx, argv[i]);
		}
		else {
			str = JS_ValueToString(cx, argv[i]);
		}
	        if(!str) {
			return JS_FALSE;
		}
		bytes = JS_EncodeString(cx, str);
		if(!bytes) {
			return JS_FALSE;
		}
		printf("%s%s", i ? " " : "", bytes);
		JS_free(cx, bytes);
	}

	putc('\n', stdout);

	JS_SET_RVAL(cx, vp, JSVAL_VOID);
	return JS_TRUE;
}

static JSBool scripting_js_call(JSContext *cx, uintN argc, jsval *vp)
{
	JSString *funcname_js;
	char *funcname;
	JSObject *args = NULL;

	JS_EnterLocalRootScope(cx);

	if(!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "S/o", &funcname_js, &args)){
		return JS_FALSE;
	}
	if(!(funcname = JS_EncodeString(cx, funcname_js))) {
		return JS_FALSE;
	}

	struct scripting_func *func = scripting_find_function(funcname);
	if(!func) {
		JS_ReportError(cx, "Function %s is not registered", funcname);
		JS_free(cx, funcname);
		return JS_FALSE;
	}
	struct dict *funcargs = args ? args_from_js(args) : NULL;
	if(args && !funcargs) {
		JS_ReportError(cx, "Unsupported argument type");
		JS_free(cx, funcname);
		return JS_FALSE;
	}

	struct dict *ret = scripting_call_function(func, funcargs, this);

	if(scripting_get_error()) {
		assert_return(!ret, JS_FALSE);
		JS_ReportError(cx, "%s", scripting_get_error());
		JS_free(cx, funcname);
		return JS_FALSE;
	}

	JS_free(cx, funcname);
	JS_SET_RVAL(cx, vp, ret ? arg_to_js(dict_find(ret, "result")) : JSVAL_VOID);
	JS_LeaveLocalRootScope(cx);
	return JS_TRUE;
}

static JSBool scripting_js_register(JSContext *cx, uintN argc, jsval *vp)
{
	JSString *funcname_js;
	char *funcname;
	JSObject *jsfunc;

	if(!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "So", &funcname_js, &jsfunc)){
		return JS_FALSE;
	}
	if(!JS_ObjectIsFunction(cx, jsfunc)) {
		JS_ReportError(cx, "func must be callable");
		return JS_FALSE;
	}
	if(!(funcname = JS_EncodeString(cx, funcname_js))) {
		return JS_FALSE;
	}
	struct scripting_func *func = scripting_register_function(this, funcname);
	if(!func) {
		JS_ReportError(cx, "Function %s is already registered", funcname);
		JS_free(cx, funcname);
		return JS_FALSE;
	}
	func->caller = (scripting_func_caller*)js_caller;
	func->freeer = (scripting_func_freeer*)js_freeer;
	func->extra = jsfunc;
	JS_AddObjectRoot(cx, (JSObject **)&func->extra);
	JS_free(cx, funcname);
	JS_SET_RVAL(cx, vp, JSVAL_VOID);
	return JS_TRUE;
}

static JSBool scripting_js_unregister(JSContext *cx, uintN argc, jsval *vp)
{
	JSString *funcname_js;
	char *funcname;

	if(!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "S", &funcname_js)){
		return JS_FALSE;
	}

	if(!(funcname = JS_EncodeString(cx, funcname_js))) {
		return JS_FALSE;
	}
	uint8_t rc = scripting_unregister_function(this, funcname);
	if(rc) {
		JS_ReportError(cx, "Function %s is not registered by this module", funcname);
		JS_free(cx, funcname);
		return JS_FALSE;
	}
	JS_free(cx, funcname);
	JS_SET_RVAL(cx, vp, JSVAL_VOID);
	return JS_TRUE;
}

static struct dict *js_caller(JSObject *jsfunc, struct dict *args, struct module *module)
{
	jsval rval;
	JSBool success;
	if(!args) {
		success = JS_CallFunctionValue(cx, NULL, OBJECT_TO_JSVAL(jsfunc), 0, NULL, &rval);
	}
	else {
		jsval argv[1];
		argv[0] = args_to_js(args);
		success = JS_CallFunctionValue(cx, NULL, OBJECT_TO_JSVAL(jsfunc), 1, argv, &rval);
	}
	if(!success) {
		JS_ReportPendingException(cx);
		return NULL;
	}

	struct scripting_arg *retarg = arg_from_js(&rval);
	if(!retarg) {
		return NULL;
	}
	struct dict *dict = scripting_args_create_dict();
	dict_insert(dict, strdup("result"), retarg);
	return dict;
}

static void js_freeer(JSObject *jsfunc, JSObject **funcp)
{
	JS_RemoveObjectRoot(cx, funcp);
}

static JSObject *js_taker(JSObject *jsfunc, JSObject **funcp)
{
	JS_AddObjectRoot(cx, funcp);
	return jsfunc;
}

static struct dict *args_from_js(JSObject *jsargs)
{
	struct dict *args = scripting_args_create_dict();
	JSObject *iter = JS_NewPropertyIterator(cx, jsargs);
	assert_return(iter, NULL);

	jsid id;
	while(JS_NextProperty(cx, iter, &id) && id != JSID_VOID) {
		jsval js_key, js_val;
		char *key;
		if(!JS_IdToValue(cx, id, &js_key)) {
			continue;
		}
		if(!JS_GetPropertyById(cx, jsargs, id, &js_val)) {
			continue;
		}
		if(!(key = JS_EncodeString(cx, JS_ValueToString(cx, js_key)))) {
			continue;
		}
		struct scripting_arg *arg = arg_from_js(&js_val);
		if(!arg) {
			dict_free(args);
			return NULL;
		}
		dict_insert(args, strdup(key), arg);
		JS_free(cx, key);
	}
	return args;

}

static struct scripting_arg *arg_from_js(jsval *valuep)
{
	jsval value = *valuep;
	struct scripting_arg *arg = malloc(sizeof(struct scripting_arg));
	memset(arg, 0, sizeof(struct scripting_arg));
	if(JSVAL_IS_NULL(value) || JSVAL_IS_VOID(value)) {
		arg->type = SCRIPTING_ARG_TYPE_NULL;
	}
	else if(JSVAL_IS_BOOLEAN(value)) {
		arg->type = SCRIPTING_ARG_TYPE_BOOL;
		arg->data.integer = malloc(sizeof(long));
		*arg->data.integer =  JSVAL_TO_BOOLEAN(value);
	}
	else if(JSVAL_IS_INT(value)) {
		arg->type = SCRIPTING_ARG_TYPE_INT;
		arg->data.integer = malloc(sizeof(long));
		*arg->data.integer = JSVAL_TO_INT(value);
	}
	else if(JSVAL_IS_DOUBLE(value)) {
		arg->type = SCRIPTING_ARG_TYPE_DOUBLE;
		arg->data.dbl = malloc(sizeof(double));
		*arg->data.dbl = JSVAL_TO_DOUBLE(value);
	}
	else if(JSVAL_IS_STRING(value)) {
		arg->type = SCRIPTING_ARG_TYPE_STRING;
		JSString *jsstr = JSVAL_TO_STRING(value);
		char *str = JS_EncodeString(cx, jsstr);
		if(!str) {
			scripting_arg_free(arg);
			return NULL;
		}
		arg->data.string = strdup(str);
		JS_free(cx, str);
	}
	else if(JSVAL_IS_OBJECT(value) && JS_IsArrayObject(cx, JSVAL_TO_OBJECT(value))) {
		JSObject *obj = JSVAL_TO_OBJECT(value);
		arg->type = SCRIPTING_ARG_TYPE_LIST;
		arg->data.list = scripting_args_create_list();
		jsuint len = 0;
		JS_GetArrayLength(cx, obj, &len);
		for(jsuint i = 0; i < len; i++) {
			jsval subval;
			JS_GetElement(cx, obj, i, &subval);
			struct scripting_arg *subarg = arg_from_js(&subval);
			if(!subarg) {
				scripting_arg_free(arg);
				return NULL;
			}
			ptrlist_add(arg->data.list, 0, subarg);
		}
	}
	else if(JSVAL_IS_OBJECT(value) && JS_ObjectIsFunction(cx, JSVAL_TO_OBJECT(value))) {
		arg->type = SCRIPTING_ARG_TYPE_CALLABLE;
		arg->callable = JSVAL_TO_OBJECT(value);
		arg->callable_module = this;
		arg->callable_caller = (scripting_func_caller*)js_caller;
		arg->callable_freeer = (scripting_func_freeer*)js_freeer;
		arg->callable_taker = (scripting_func_taker*)js_taker;
	}
	else if(JSVAL_IS_OBJECT(value) && JS_InstanceOf(cx, JSVAL_TO_OBJECT(value), &resource_class, NULL) == JS_TRUE) {
		arg->type = SCRIPTING_ARG_TYPE_RESOURCE;
		arg->resource = JS_GetPrivate(cx, JSVAL_TO_OBJECT(value));
	}
	else if(JSVAL_IS_OBJECT(value)) {
		arg->type = SCRIPTING_ARG_TYPE_DICT;
		arg->data.dict = args_from_js(JSVAL_TO_OBJECT(value));
	}
	else {
		scripting_arg_free(arg);
		return NULL;
	}
	return arg;

}

static jsval args_to_js(struct dict *args)
{
	JSObject *obj = JS_NewObject(cx, NULL, NULL, NULL);
	dict_iter(node, args) {
		jsval val = arg_to_js(node->data);
		JS_SetProperty(cx, obj, node->key, &val);
	}
	return OBJECT_TO_JSVAL(obj);
}

static jsval arg_to_js(struct scripting_arg *arg)
{
	JSObject *list, *res;

	switch(arg->type) {
		case SCRIPTING_ARG_TYPE_NULL:
			return JSVAL_NULL;
		case SCRIPTING_ARG_TYPE_BOOL:
			return BOOLEAN_TO_JSVAL(*arg->data.integer);
		case SCRIPTING_ARG_TYPE_INT:
			return INT_TO_JSVAL(*arg->data.integer);
		case SCRIPTING_ARG_TYPE_DOUBLE:
			return DOUBLE_TO_JSVAL(*arg->data.dbl);
		case SCRIPTING_ARG_TYPE_STRING:
			return STRING_TO_JSVAL(JS_NewStringCopyZ(cx, arg->data.string));
		case SCRIPTING_ARG_TYPE_LIST:
			list = JS_NewArrayObject(cx, arg->data.list->count, NULL);
			for(unsigned int i = 0; i < arg->data.list->count; i++) {
				jsval val = arg_to_js(arg->data.list->data[i]->ptr);
				JS_SetElement(cx, list, i, &val);
			}
			return OBJECT_TO_JSVAL(list);
		case SCRIPTING_ARG_TYPE_DICT:
			return args_to_js(arg->data.dict);
		case SCRIPTING_ARG_TYPE_CALLABLE:
			if(arg->callable_module == this) {
				return OBJECT_TO_JSVAL(arg->callable);
			}
			else {
				return callable_arg_to_js(arg);
			}
		case SCRIPTING_ARG_TYPE_RESOURCE:
			res = JS_NewObject(cx, &resource_class, NULL, NULL);
			JS_SetPrivate(cx, res, arg->resource);
			return OBJECT_TO_JSVAL(res);
	}

	assert_return(0 && "shouldn't happen at all", JSVAL_VOID);
}

static JSBool scripting_js_call_callable(JSContext *cx, uintN argc, jsval *vp)
{
	JSObject *args = NULL;

	JS_EnterLocalRootScope(cx);

	if(!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "/o", &args)){
		return JS_FALSE;
	}

	struct dict *funcargs = args ? args_from_js(args) : NULL;
	if(args && !funcargs) {
		JS_ReportError(cx, "Unsupported argument type");
		return JS_FALSE;
	}

	jsval jsarg;
	JSObject *func = JSVAL_TO_OBJECT(JS_CALLEE(cx, vp));
	JS_GetReservedSlot(cx, func, 0, &jsarg);
	struct scripting_arg *arg = JSVAL_TO_PRIVATE(jsarg);
	struct dict *ret = arg->callable_caller(arg->callable, funcargs, this);

	if(scripting_get_error()) {
		assert_return(!ret, JS_FALSE);
		JS_ReportError(cx, "%s", scripting_get_error());
		return JS_FALSE;
	}

	JS_SET_RVAL(cx, vp, ret ? arg_to_js(dict_find(ret, "result")) : JSVAL_VOID);
	JS_LeaveLocalRootScope(cx);
	return JS_TRUE;
}

static jsval callable_arg_to_js(struct scripting_arg *arg)
{
	JSObject *func = JS_GetFunctionObject(JS_NewFunction(cx, scripting_js_call_callable, 0, JSPROP_READONLY | JSPROP_PERMANENT, NULL, NULL));
	JS_SetReservedSlot(cx, func, 0, PRIVATE_TO_JSVAL(arg));
	return OBJECT_TO_JSVAL(func);
}
