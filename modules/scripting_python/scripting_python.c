#include <Python.h>
#include "global.h"
#include "module.h"
#include "modules/scripting/scripting.h"

MODULE_DEPENDS("scripting", NULL);

PyMODINIT_FUNC create_module();
static PyObject* scripting_python_call(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject* scripting_python_register(PyObject *self, PyObject *args);
static PyObject* scripting_python_unregister(PyObject *self, PyObject *args);
static void python_caller(struct scripting_func *func);
static void python_freeer(struct scripting_func *func);

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
	Py_Initialize();
	create_module();
	PyRun_SimpleString("import surgebot\n\
def test(**args):\n\
	print args\n\
surgebot.register('test', test)\n\
surgebot.call('test')");
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
	scripting_call_function(func);
	Py_RETURN_NONE;
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
	func->caller = python_caller;
	func->freeer = python_freeer;
	Py_XINCREF(pyfunc);
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

static void python_caller(struct scripting_func *func)
{
	PyObject *pyfunc = func->extra;
	PyObject_CallObject(pyfunc, NULL);
}

static void python_freeer(struct scripting_func *func)
{
	PyObject *pyfunc = func->extra;
	Py_XDECREF(pyfunc);
}
