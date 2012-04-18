#include <Python.h>
#include "global.h"
#include "module.h"
#include "ptrlist.h"
#include "modules/scripting/scripting.h"

MODULE_DEPENDS("scripting", NULL);

PyMODINIT_FUNC create_module();
static PyObject* scripting_python_call(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject* scripting_python_register(PyObject *self, PyObject *args);
static PyObject* scripting_python_unregister(PyObject *self, PyObject *args);
static struct dict *python_caller(PyObject *pyfunc, struct dict *args);
static void python_freeer(PyObject *pyfunc);
static PyObject *python_taker(PyObject *pyfunc);
static struct dict *args_from_python(PyObject *pyargs);
static struct scripting_arg *arg_from_python(PyObject *value);
static PyObject *args_to_python(struct dict *args);
static PyObject *arg_to_python(struct scripting_arg *arg);

static struct module *this;

static PyMethodDef SurgebotMethods[] = {
    {"call", (PyCFunction)scripting_python_call, METH_VARARGS | METH_KEYWORDS, "Execute a function."},
    {"register", scripting_python_register, METH_VARARGS, "Register a function."},
    {"unregister", scripting_python_unregister, METH_VARARGS, "Unregister a function."},
    {NULL, NULL, 0, NULL}
};


MODULE_INIT
{
	this = self;
	Py_InitializeEx(0);
	create_module();
/*
	PyRun_SimpleString("import surgebot\n\
def cb(**args):\n\
	print 'cb called: %r' % args\n\
	return 1337\n\
def test(callable, **args):\n\
	print 'test func called: %r' % args\n\
	return callable(), 1*2*3, 4*5*6, 'xyz', {'foo': 'bar'}\n\
surgebot.register('test', test)\n\
print surgebot.call('test', foo='bar', num=1337, abc=['x',1,2,3], xxx=dict(a='b', c='d'), callable=cb)");
*/
	PyRun_SimpleString("import surgebot\n\
cnt = 0\n\
def notice(src, args):\n\
	global cnt\n\
	print 'notice: %r' % args\n\
	cnt += 1\n\
	if src:\n\
		surgebot.call('irc_send', msg='PRIVMSG #ircops :hello [%s]' % args[-1], raw=True)\n\
	if cnt == 10:\n\
		surgebot.call('unreg_irc_handler', cmd='NOTICE', func=notice)\n\
surgebot.call('reg_irc_handler', cmd='NOTICE', func=notice)");
}

MODULE_FINI
{
	Py_Finalize();
}

PyMODINIT_FUNC create_module()
{
	Py_InitModule("surgebot", SurgebotMethods);
}

static PyObject* scripting_python_call(PyObject *self, PyObject *args, PyObject *kwargs)
{
	const char *funcname = NULL;
	if(!PyArg_ParseTuple(args, "s", &funcname)) {
		return NULL;
	}

	struct scripting_func *func = scripting_find_function(funcname);
	if(!func) {
		return PyErr_Format(PyExc_NotImplementedError, "Function %s is not registered", funcname);
	}

	struct dict *funcargs = args_from_python(kwargs);
	if(!funcargs) {
		return PyErr_Format(PyExc_ValueError, "Unsupported argument type");
	}
	struct dict *ret = scripting_call_function(func, funcargs);
	if(scripting_get_error()) {
		assert_return(!ret, NULL);
		return PyErr_Format(PyExc_RuntimeError, "%s", scripting_get_error());
	}
	if(!ret) {
		Py_RETURN_NONE;
	}
	return args_to_python(ret);
}

static PyObject* scripting_python_register(PyObject *self, PyObject *args)
{
	const char *funcname = NULL;
	PyObject *pyfunc = NULL;
	if(!PyArg_ParseTuple(args, "sO", &funcname, &pyfunc)) {
		return NULL;
	}

	if(!PyCallable_Check(pyfunc)) {
		PyErr_SetString(PyExc_TypeError, "func must be callable");
		return NULL;
	}

	struct scripting_func *func = scripting_register_function(this, funcname);
	if(!func) {
		return PyErr_Format(PyExc_ValueError, "Function %s is already registered", funcname);
	}
	func->caller = (scripting_func_caller*)python_caller;
	func->freeer = (scripting_func_freeer*)python_freeer;
	Py_INCREF(pyfunc);
	func->extra = pyfunc;
	Py_RETURN_NONE;
}

static PyObject* scripting_python_unregister(PyObject *self, PyObject *args)
{
	const char *funcname = NULL;
	if(!PyArg_ParseTuple(args, "s", &funcname)) {
		return NULL;
	}

	uint8_t rc = scripting_unregister_function(this, funcname);
	if(rc) {
		return PyErr_Format(PyExc_ValueError, "Function %s is not registered by this module", funcname);
	}
	Py_RETURN_NONE;
}

static struct dict *python_caller(PyObject *pyfunc, struct dict *args)
{
	PyObject *ret = NULL;
	if(!args) {
		ret = PyObject_CallObject(pyfunc, NULL);
	}
	else {
		PyObject *posargs = PyTuple_New(0);
		PyObject *kwargs = args_to_python(args);
		ret = PyObject_Call(pyfunc, posargs, kwargs);
		Py_DECREF(posargs);
		Py_DECREF(kwargs);
	}
	if(PyErr_Occurred()) {
		PyErr_Print();
		Py_XDECREF(ret);
		return NULL;
	}
	if(!ret) {
		return NULL;
	}

	// Our top-level struct is always a dict, so we wrap return values in a dict
	struct scripting_arg *retarg = arg_from_python(ret);
	Py_DECREF(ret);
	if(!retarg) {
		return NULL;
	}
	struct dict *dict = scripting_args_create_dict();
	dict_insert(dict, strdup("result"), retarg);
	return dict;
}

static void python_freeer(PyObject *pyfunc)
{
	Py_DECREF(pyfunc);
}

static PyObject *python_taker(PyObject *pyfunc)
{
	Py_INCREF(pyfunc);
	return pyfunc;
}

static struct dict *args_from_python(PyObject *pyargs)
{
	struct dict *args = scripting_args_create_dict();
	PyObject *key, *value;
	Py_ssize_t pos = 0;

	while(PyDict_Next(pyargs, &pos, &key, &value)) {
		struct scripting_arg *arg = arg_from_python(value);
		if(!arg) {
			dict_free(args);
			return NULL;
		}
		dict_insert(args, strdup(PyString_AsString(key)), arg);
	}
	return args;
}

static struct scripting_arg *arg_from_python(PyObject *value)
{
	struct scripting_arg *arg = malloc(sizeof(struct scripting_arg));
	memset(arg, 0, sizeof(struct scripting_arg));
	if(value == Py_None) {
		arg->type = SCRIPTING_ARG_TYPE_NULL;
	}
	else if(PyBool_Check(value)) {
		arg->type = SCRIPTING_ARG_TYPE_BOOL;
		arg->data.integer = malloc(sizeof(long));
		*arg->data.integer = value == Py_True;
	}
	else if(PyInt_Check(value)) {
		arg->type = SCRIPTING_ARG_TYPE_INT;
		arg->data.integer = malloc(sizeof(long));
		*arg->data.integer = PyInt_AS_LONG(value);
	}
	else if(PyFloat_Check(value)) {
		arg->type = SCRIPTING_ARG_TYPE_DOUBLE;
		arg->data.dbl = malloc(sizeof(double));
		*arg->data.dbl = PyFloat_AS_DOUBLE(value);
	}
	else if(PyString_Check(value)) {
		arg->type = SCRIPTING_ARG_TYPE_STRING;
		arg->data.string = strdup(PyString_AsString(value));
	}
	else if(PyList_Check(value) || PyTuple_Check(value)) {
		arg->type = SCRIPTING_ARG_TYPE_LIST;
		arg->data.list = scripting_args_create_list();
		for(Py_ssize_t i = 0; i < PySequence_Size(value); i++) {
			struct scripting_arg *subarg = arg_from_python(PySequence_Fast_GET_ITEM(value, i));
			if(!subarg) {
				scripting_arg_free(arg);
				return NULL;
			}
			ptrlist_add(arg->data.list, 0, subarg);
		}
	}
	else if(PyDict_Check(value)) {
		arg->type = SCRIPTING_ARG_TYPE_DICT;
		arg->data.dict = args_from_python(value);
	}
	else if(PyCallable_Check(value)) {
		arg->type = SCRIPTING_ARG_TYPE_CALLABLE;
		Py_INCREF(value);
		arg->callable = value;
		arg->callable_module = this;
		arg->callable_freeer = (scripting_func_freeer*)python_freeer;
		arg->callable_taker = (scripting_func_taker*)python_taker;
		arg->callable_caller = (scripting_func_caller*)python_caller;
	}
	else {
		scripting_arg_free(arg);
		return NULL;
	}
	return arg;
}

static PyObject *args_to_python(struct dict *args)
{
	PyObject *kwargs = PyDict_New();
	dict_iter(node, args) {
		PyObject *tmp = arg_to_python(node->data);
		PyDict_SetItemString(kwargs, node->key, tmp);
		Py_DECREF(tmp);
	}
	return kwargs;
}

static PyObject *arg_to_python(struct scripting_arg *arg)
{
	PyObject *list;

	switch(arg->type) {
		case SCRIPTING_ARG_TYPE_NULL:
			Py_INCREF(Py_None);
			return Py_None;
		case SCRIPTING_ARG_TYPE_BOOL:
			return PyBool_FromLong(*arg->data.integer);
		case SCRIPTING_ARG_TYPE_INT:
			return PyInt_FromLong(*arg->data.integer);
		case SCRIPTING_ARG_TYPE_DOUBLE:
			return PyFloat_FromDouble(*arg->data.dbl);
		case SCRIPTING_ARG_TYPE_STRING:
			return PyString_FromString(arg->data.string);
		case SCRIPTING_ARG_TYPE_LIST:
			list = PyList_New(arg->data.list->count);
			for(unsigned int i = 0; i < arg->data.list->count; i++) {
				PyList_SET_ITEM(list, i, arg_to_python(arg->data.list->data[i]->ptr));
			}
			return list;
		case SCRIPTING_ARG_TYPE_DICT:
			return args_to_python(arg->data.dict);
		case SCRIPTING_ARG_TYPE_CALLABLE:
			Py_INCREF(arg->callable);
			return arg->callable;
	}

	assert_return(0 && "shouldn't happen at all", NULL);
}
