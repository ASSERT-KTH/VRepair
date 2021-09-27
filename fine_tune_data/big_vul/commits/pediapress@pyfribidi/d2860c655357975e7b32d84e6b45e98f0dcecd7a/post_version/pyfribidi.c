
/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/* Copyright (C) 2005,2006,2010 Yaacov Zamir, Nir Soffer */


#include <Python.h>
#include <fribidi.h>


static PyObject *
unicode_log2vis (PyUnicodeObject* string,
                 FriBidiParType base_direction, int clean, int reordernsm)
{
    int i;
    int length = string->length;
    FriBidiChar *logical = NULL; /* input fribidi unicode buffer */
    FriBidiChar *visual = NULL;      /* output fribidi unicode buffer */
    FriBidiStrIndex new_len = 0; /* length of the UTF-8 buffer */
    PyUnicodeObject *result = NULL;

    /* Allocate fribidi unicode buffers
       TODO - Don't copy strings if sizeof(FriBidiChar) == sizeof(Py_UNICODE)
    */

    logical = PyMem_New (FriBidiChar, length + 1);
    if (logical == NULL) {
        PyErr_NoMemory();
        goto cleanup;
    }

    visual = PyMem_New (FriBidiChar, length + 1);
    if (visual == NULL) {
        PyErr_NoMemory();
        goto cleanup;
    }

    for (i=0; i<length; ++i) {
        logical[i] = string->str[i];
    }

    /* Convert to unicode and order visually */
    fribidi_set_reorder_nsm(reordernsm);

    if (!fribidi_log2vis (logical, length, &base_direction, visual,
                          NULL, NULL, NULL)) {

        PyErr_SetString (PyExc_RuntimeError,
                         "fribidi failed to order string");
        goto cleanup;
    }

    /* Cleanup the string if requested */
    if (clean) {
        length = fribidi_remove_bidi_marks (visual, length, NULL, NULL, NULL);
    }

    result = (PyUnicodeObject*) PyUnicode_FromUnicode(NULL, length);
    if (result == NULL) {
        goto cleanup;
    }

    for (i=0; i<length; ++i) {
        result->str[i] = visual[i];
    }

  cleanup:
    /* Delete unicode buffers */
    PyMem_Del (logical);
    PyMem_Del (visual);

    return (PyObject *)result;
}

static PyObject *
_pyfribidi_log2vis (PyObject * self, PyObject * args, PyObject * kw)
{
    PyUnicodeObject *logical = NULL;	/* input unicode or string object */
    FriBidiParType base = FRIBIDI_TYPE_RTL;	/* optional direction */
    int clean = 0; /* optional flag to clean the string */
    int reordernsm = 1; /* optional flag to allow reordering of non spacing marks*/

    static char *kwargs[] =
        { "logical", "base_direction", "clean", "reordernsm", NULL };

    if (!PyArg_ParseTupleAndKeywords (args, kw, "U|iii", kwargs,
                                      &logical, &base, &clean, &reordernsm)) {
        return NULL;
    }

    /* Validate base */

    if (!(base == FRIBIDI_TYPE_RTL
          || base == FRIBIDI_TYPE_LTR
          || base == FRIBIDI_TYPE_ON)) {
        return PyErr_Format (PyExc_ValueError,
                             "invalid value %d: use either RTL, LTR or ON",
                             base);
    }

    return unicode_log2vis (logical, base, clean, reordernsm);
}


static PyMethodDef PyfribidiMethods[] = {
        {"log2vis", (PyCFunction) _pyfribidi_log2vis, METH_VARARGS | METH_KEYWORDS, NULL},
	{NULL, NULL, 0, NULL}
};


PyMODINIT_FUNC
init_pyfribidi (void)
{
        PyObject *module = Py_InitModule ("_pyfribidi", PyfribidiMethods);
	PyModule_AddIntConstant (module, "RTL", (long) FRIBIDI_TYPE_RTL);
	PyModule_AddIntConstant (module, "LTR", (long) FRIBIDI_TYPE_LTR);
	PyModule_AddIntConstant (module, "ON", (long) FRIBIDI_TYPE_ON);
}
