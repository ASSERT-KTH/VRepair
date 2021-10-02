
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

/* 	FriBidi python binding:
    
    Install:
	python setup.py install
    
 */

#include <Python.h>		/* must be first */
#include <fribidi.h>
#undef _POSIX_C_SOURCE

#include "pyfribidi.h"

#define MAX_STR_LEN 65000

static PyObject *
_pyfribidi_log2vis (PyObject * self, PyObject * args, PyObject * kw)
{
	PyObject *logical = NULL;	/* input unicode or string object */
	FriBidiParType base = FRIBIDI_TYPE_RTL;	/* optional direction */
	const char *encoding = "utf-8";	/* optional input string encoding */
	int clean = 0; /* optional flag to clean the string */
	int reordernsm = 1; /* optional flag to allow reordering of non spacing marks*/

	static char *kwargs[] =
	        { "logical", "base_direction", "encoding", "clean", "reordernsm", NULL };

        if (!PyArg_ParseTupleAndKeywords (args, kw, "O|isii", kwargs,
					  &logical, &base, &encoding, &clean, &reordernsm))
		return NULL;

	/* Validate base */

	if (!(base == FRIBIDI_TYPE_RTL ||
	      base == FRIBIDI_TYPE_LTR || base == FRIBIDI_TYPE_ON))
		return PyErr_Format (PyExc_ValueError,
				     "invalid value %d: use either RTL, LTR or ON",
				     base);

	/* Check object type and delegate to one of the log2vis functions */

	if (PyUnicode_Check (logical))
	        return log2vis_unicode (logical, base, clean, reordernsm);
	else if (PyString_Check (logical))
	        return log2vis_encoded_string (logical, encoding, base, clean, reordernsm);
	else
		return PyErr_Format (PyExc_TypeError,
				     "expected unicode or str, not %s",
				     logical->ob_type->tp_name);
}

/*
  log2vis_unicode - reorder unicode string visually

  Return value: new reference

  Return Python unicode object ordered visually or NULL if an exception
  was raised.

  Since Python and fribidi don't now know each other unicode format,
  encode input string as utf-8 and invoke log2vis_utf8.
  
  Arguments:
  
  - unicode: Python unicode object
  - base_direction: input string base direction, e.g right to left  
*/

static PyObject *
log2vis_unicode (PyObject * unicode, FriBidiParType base_direction, int clean, int reordernsm)
{
	PyObject *logical = NULL;	/* input string encoded in utf-8 */
	PyObject *visual = NULL;	/* output string encoded in utf-8 */
	PyObject *result = NULL;	/* unicode output string */

	int length = PyUnicode_GET_SIZE (unicode);

	logical = PyUnicode_AsUTF8String (unicode);
	if (logical == NULL)
		goto cleanup;

	visual = log2vis_utf8 (logical, length, base_direction, clean, reordernsm);
	if (visual == NULL)
		goto cleanup;

	result = PyUnicode_DecodeUTF8 (PyString_AS_STRING (visual),
				       PyString_GET_SIZE (visual), "strict");

      cleanup:
	Py_XDECREF (logical);
	Py_XDECREF (visual);

	return result;
}

/*
  log2vis_encoded_string - reorder encoded string visually

  Return value: new reference

  Return Python string object ordered visually or NULL if an exception
  was raised. The returned string use the same encoding.

  Invoke either log2vis_utf8 or log2vis_unicode.

  - string: Python string object using encoding
  - encoding: string encoding, any encoding name known to Python
  - base_direction: input string base direction, e.g right to left
 */

static PyObject *
log2vis_encoded_string (PyObject * string, const char *encoding,
			FriBidiParType base_direction, int clean, int reordernsm)
{
	PyObject *logical = NULL;	/* logical unicode object */
	PyObject *result = NULL;	/* output string object */

	/* Always needed for the string length */
	logical = PyUnicode_Decode (PyString_AS_STRING (string),
				    PyString_GET_SIZE (string),
				    encoding, "strict");
	if (logical == NULL)
		return NULL;

	if (strcmp (encoding, "utf-8") == 0)
		/* Shortcut for utf8 strings (little faster) */
		result = log2vis_utf8 (string,
				       PyUnicode_GET_SIZE (logical),
				       base_direction, clean, reordernsm);
	else
	{
		/* Invoke log2vis_unicode and encode back to encoding */

		PyObject *visual = log2vis_unicode (logical, base_direction, clean, reordernsm);

		if (visual)
		{
			result = PyUnicode_Encode (PyUnicode_AS_UNICODE
						   (visual),
						   PyUnicode_GET_SIZE (visual),
						   encoding, "strict");
			Py_DECREF (visual);
		}
	}

	Py_DECREF (logical);

	return result;
}

/*
  log2vis_utf8 - reorder string visually

  Return value: new reference

  Return Python string object ordered visually or NULL if an exception
  was raised.
  
  Arguments:
  
  - string: Python string object using utf-8 encoding
  - unicode_length: number of characters in string. This is not the
    number of bytes in the string, which may be much bigger than the
    number of characters, because utf-8 uses 1-4 bytes per character.
  - base_direction: input string base direction, e.g right to left
*/

static PyObject *
log2vis_utf8 (PyObject * string, int unicode_length,
	      FriBidiParType base_direction, int clean, int reordernsm)
{
	FriBidiChar *logical = NULL; /* input fribidi unicode buffer */
	FriBidiChar *visual = NULL;	 /* output fribidi unicode buffer */
	char *visual_utf8 = NULL;    /* output fribidi UTF-8 buffer */
	FriBidiStrIndex new_len = 0; /* length of the UTF-8 buffer */
	PyObject *result = NULL;	 /* failure */

	/* Allocate fribidi unicode buffers */

	logical = PyMem_New (FriBidiChar, unicode_length + 1);
	if (logical == NULL)
	{
		PyErr_SetString (PyExc_MemoryError,
				 "failed to allocate unicode buffer");
		goto cleanup;
	}

	visual = PyMem_New (FriBidiChar, unicode_length + 1);
	if (visual == NULL)
	{
		PyErr_SetString (PyExc_MemoryError,
				 "failed to allocate unicode buffer");
		goto cleanup;
	}

	/* Convert to unicode and order visually */
	fribidi_set_reorder_nsm(reordernsm);
	fribidi_utf8_to_unicode (PyString_AS_STRING (string),
				 PyString_GET_SIZE (string), logical);

	if (!fribidi_log2vis (logical, unicode_length, &base_direction, visual,
			      NULL, NULL, NULL))
	{
		PyErr_SetString (PyExc_RuntimeError,
				 "fribidi failed to order string");
		goto cleanup;
	}

	/* Cleanup the string if requested */
	if (clean)
		fribidi_remove_bidi_marks (visual, unicode_length, NULL, NULL, NULL);

	/* Allocate fribidi UTF-8 buffer */

	visual_utf8 = PyMem_New(char, (unicode_length * 4)+1);
	if (visual_utf8 == NULL)
	{
		PyErr_SetString (PyExc_MemoryError,
				"failed to allocate UTF-8 buffer");
		goto cleanup;
	}

	/* Encode the reordered string  and create result string */

	new_len = fribidi_unicode_to_utf8 (visual, unicode_length, visual_utf8);

	result = PyString_FromStringAndSize (visual_utf8, new_len);
	if (result == NULL)
		/* XXX does it raise any error? */
		goto cleanup;

      cleanup:
	/* Delete unicode buffers */
	PyMem_Del (logical);
	PyMem_Del (visual);
	PyMem_Del (visual_utf8);

	return result;
}

static PyMethodDef PyfribidiMethods[] = {
	{"log2vis", (PyCFunction) _pyfribidi_log2vis,
	 METH_VARARGS | METH_KEYWORDS,
	 _pyfribidi_log2vis__doc__},
	{NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC
initpyfribidi (void)
{
	PyObject *module;

	/* XXX What should be done if we fail here? */

	module = Py_InitModule3 ("pyfribidi", PyfribidiMethods,
				 _pyfribidi__doc__);

	PyModule_AddIntConstant (module, "RTL", (long) FRIBIDI_TYPE_RTL);
	PyModule_AddIntConstant (module, "LTR", (long) FRIBIDI_TYPE_LTR);
	PyModule_AddIntConstant (module, "ON", (long) FRIBIDI_TYPE_ON);

	PyModule_AddStringConstant (module, "__author__",
				    "Yaacov Zamir and Nir Soffer");
}
