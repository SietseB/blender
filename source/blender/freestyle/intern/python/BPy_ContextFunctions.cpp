/* SPDX-FileCopyrightText: 2009-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_ContextFunctions.h"
#include "BPy_Convert.h"

#include "../stroke/ContextFunctions.h"

#include "BLI_sys_types.h"

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

//------------------------ MODULE FUNCTIONS ----------------------------------

PyDoc_STRVAR(
    /* Wrap. */
    ContextFunctions_get_time_stamp___doc__,
    ".. function:: get_time_stamp()\n"
    "\n"
    "   Returns the system time stamp.\n"
    "\n"
    "   :return: The system time stamp.\n"
    "   :rtype: int\n");

static PyObject *ContextFunctions_get_time_stamp(PyObject * /*self*/)
{
  return PyLong_FromLong(ContextFunctions::GetTimeStampCF());
}

PyDoc_STRVAR(
    /* Wrap. */
    ContextFunctions_get_canvas_width___doc__,
    ".. method:: get_canvas_width()\n"
    "\n"
    "   Returns the canvas width.\n"
    "\n"
    "   :return: The canvas width.\n"
    "   :rtype: int\n");

static PyObject *ContextFunctions_get_canvas_width(PyObject * /*self*/)
{
  return PyLong_FromLong(ContextFunctions::GetCanvasWidthCF());
}

PyDoc_STRVAR(
    /* Wrap. */
    ContextFunctions_get_canvas_height___doc__,
    ".. method:: get_canvas_height()\n"
    "\n"
    "   Returns the canvas height.\n"
    "\n"
    "   :return: The canvas height.\n"
    "   :rtype: int\n");

static PyObject *ContextFunctions_get_canvas_height(PyObject * /*self*/)
{
  return PyLong_FromLong(ContextFunctions::GetCanvasHeightCF());
}

PyDoc_STRVAR(
    /* Wrap. */
    ContextFunctions_get_border___doc__,
    ".. method:: get_border()\n"
    "\n"
    "   Returns the border.\n"
    "\n"
    "   :return: A tuple of 4 numbers (xmin, ymin, xmax, ymax).\n"
    "   :rtype: tuple[int, int, int, int]\n");

static PyObject *ContextFunctions_get_border(PyObject * /*self*/)
{
  BBox<Vec2i> border(ContextFunctions::GetBorderCF());
  PyObject *v = PyTuple_New(4);
  PyTuple_SET_ITEMS(v,
                    PyLong_FromLong(border.getMin().x()),
                    PyLong_FromLong(border.getMin().y()),
                    PyLong_FromLong(border.getMax().x()),
                    PyLong_FromLong(border.getMax().y()));
  return v;
}

PyDoc_STRVAR(
    /* Wrap. */
    ContextFunctions_load_map___doc__,
    ".. function:: load_map(file_name, map_name, num_levels=4, sigma=1.0)\n"
    "\n"
    "   Loads an image map for further reading.\n"
    "\n"
    "   :arg file_name: The name of the image file.\n"
    "   :type file_name: str\n"
    "   :arg map_name: The name that will be used to access this image.\n"
    "   :type map_name: str\n"
    "   :arg num_levels: The number of levels in the map pyramid\n"
    "      (default = 4). If num_levels == 0, the complete pyramid is\n"
    "      built.\n"
    "   :type num_levels: int\n"
    "   :arg sigma: The sigma value of the gaussian function.\n"
    "   :type sigma: float\n");

static PyObject *ContextFunctions_load_map(PyObject * /*self*/, PyObject *args, PyObject *kwds)
{
  static const char *kwlist[] = {"file_name", "map_name", "num_levels", "sigma", nullptr};
  char *fileName, *mapName;
  uint nbLevels = 4;
  float sigma = 1.0;

  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "ss|If", (char **)kwlist, &fileName, &mapName, &nbLevels, &sigma))
  {
    return nullptr;
  }
  ContextFunctions::LoadMapCF(fileName, mapName, nbLevels, sigma);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    ContextFunctions_read_map_pixel___doc__,
    ".. function:: read_map_pixel(map_name, level, x, y)\n"
    "\n"
    "   Reads a pixel in a user-defined map.\n"
    "\n"
    "   :arg map_name: The name of the map.\n"
    "   :type map_name: str\n"
    "   :arg level: The level of the pyramid in which we wish to read the\n"
    "      pixel.\n"
    "   :type level: int\n"
    "   :arg x: The x coordinate of the pixel we wish to read. The origin\n"
    "      is in the lower-left corner.\n"
    "   :type x: int\n"
    "   :arg y: The y coordinate of the pixel we wish to read. The origin\n"
    "      is in the lower-left corner.\n"
    "   :type y: int\n"
    "   :return: The floating-point value stored for that pixel.\n"
    "   :rtype: float\n");

static PyObject *ContextFunctions_read_map_pixel(PyObject * /*self*/,
                                                 PyObject *args,
                                                 PyObject *kwds)
{
  static const char *kwlist[] = {"map_name", "level", "x", "y", nullptr};
  char *mapName;
  int level;
  uint x, y;

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "siII", (char **)kwlist, &mapName, &level, &x, &y))
  {
    return nullptr;
  }
  return PyFloat_FromDouble(ContextFunctions::ReadMapPixelCF(mapName, level, x, y));
}

PyDoc_STRVAR(
    /* Wrap. */
    ContextFunctions_read_complete_view_map_pixel___doc__,
    ".. function:: read_complete_view_map_pixel(level, x, y)\n"
    "\n"
    "   Reads a pixel in the complete view map.\n"
    "\n"
    "   :arg level: The level of the pyramid in which we wish to read the\n"
    "      pixel.\n"
    "   :type level: int\n"
    "   :arg x: The x coordinate of the pixel we wish to read. The origin\n"
    "      is in the lower-left corner.\n"
    "   :type x: int\n"
    "   :arg y: The y coordinate of the pixel we wish to read. The origin\n"
    "      is in the lower-left corner.\n"
    "   :type y: int\n"
    "   :return: The floating-point value stored for that pixel.\n"
    "   :rtype: float\n");

static PyObject *ContextFunctions_read_complete_view_map_pixel(PyObject * /*self*/,
                                                               PyObject *args,
                                                               PyObject *kwds)
{
  static const char *kwlist[] = {"level", "x", "y", nullptr};
  int level;
  uint x, y;

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "iII", (char **)kwlist, &level, &x, &y)) {
    return nullptr;
  }
  return PyFloat_FromDouble(ContextFunctions::ReadCompleteViewMapPixelCF(level, x, y));
}

PyDoc_STRVAR(
    /* Wrap. */
    ContextFunctions_read_directional_view_map_pixel___doc__,
    ".. function:: read_directional_view_map_pixel(orientation, level, x, y)\n"
    "\n"
    "   Reads a pixel in one of the oriented view map images.\n"
    "\n"
    "   :arg orientation: The number telling which orientation we want to\n"
    "      check.\n"
    "   :type orientation: int\n"
    "   :arg level: The level of the pyramid in which we wish to read the\n"
    "      pixel.\n"
    "   :type level: int\n"
    "   :arg x: The x coordinate of the pixel we wish to read. The origin\n"
    "      is in the lower-left corner.\n"
    "   :type x: int\n"
    "   :arg y: The y coordinate of the pixel we wish to read. The origin\n"
    "      is in the lower-left corner.\n"
    "   :type y: int\n"
    "   :return: The floating-point value stored for that pixel.\n"
    "   :rtype: float\n");

static PyObject *ContextFunctions_read_directional_view_map_pixel(PyObject * /*self*/,
                                                                  PyObject *args,
                                                                  PyObject *kwds)
{
  static const char *kwlist[] = {"orientation", "level", "x", "y", nullptr};
  int orientation, level;
  uint x, y;

  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "iiII", (char **)kwlist, &orientation, &level, &x, &y))
  {
    return nullptr;
  }
  return PyFloat_FromDouble(
      ContextFunctions::ReadDirectionalViewMapPixelCF(orientation, level, x, y));
}

PyDoc_STRVAR(
    /* Wrap. */
    ContextFunctions_get_selected_fedge___doc__,
    ".. function:: get_selected_fedge()\n"
    "\n"
    "   Returns the selected FEdge.\n"
    "\n"
    "   :return: The selected FEdge.\n"
    "   :rtype: :class:`FEdge`\n");

static PyObject *ContextFunctions_get_selected_fedge(PyObject * /*self*/)
{
  FEdge *fe = ContextFunctions::GetSelectedFEdgeCF();
  if (fe) {
    return Any_BPy_FEdge_from_FEdge(*fe);
  }
  Py_RETURN_NONE;
}

/*-----------------------ContextFunctions module docstring-------------------------------*/

PyDoc_STRVAR(
    /* Wrap. */
    module_docstring,
    "The Blender Freestyle.ContextFunctions submodule\n"
    "\n");

/*-----------------------ContextFunctions module functions definitions-------------------*/

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wcast-function-type"
#  else
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-function-type"
#  endif
#endif

static PyMethodDef module_functions[] = {
    {"get_time_stamp",
     (PyCFunction)ContextFunctions_get_time_stamp,
     METH_NOARGS,
     ContextFunctions_get_time_stamp___doc__},
    {"get_canvas_width",
     (PyCFunction)ContextFunctions_get_canvas_width,
     METH_NOARGS,
     ContextFunctions_get_canvas_width___doc__},
    {"get_canvas_height",
     (PyCFunction)ContextFunctions_get_canvas_height,
     METH_NOARGS,
     ContextFunctions_get_canvas_height___doc__},
    {"get_border",
     (PyCFunction)ContextFunctions_get_border,
     METH_NOARGS,
     ContextFunctions_get_border___doc__},
    {"load_map",
     (PyCFunction)ContextFunctions_load_map,
     METH_VARARGS | METH_KEYWORDS,
     ContextFunctions_load_map___doc__},
    {"read_map_pixel",
     (PyCFunction)ContextFunctions_read_map_pixel,
     METH_VARARGS | METH_KEYWORDS,
     ContextFunctions_read_map_pixel___doc__},
    {"read_complete_view_map_pixel",
     (PyCFunction)ContextFunctions_read_complete_view_map_pixel,
     METH_VARARGS | METH_KEYWORDS,
     ContextFunctions_read_complete_view_map_pixel___doc__},
    {"read_directional_view_map_pixel",
     (PyCFunction)ContextFunctions_read_directional_view_map_pixel,
     METH_VARARGS | METH_KEYWORDS,
     ContextFunctions_read_directional_view_map_pixel___doc__},
    {"get_selected_fedge",
     (PyCFunction)ContextFunctions_get_selected_fedge,
     METH_NOARGS,
     ContextFunctions_get_selected_fedge___doc__},
    {nullptr, nullptr, 0, nullptr},
};

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic pop
#  else
#    pragma GCC diagnostic pop
#  endif
#endif

/*-----------------------ContextFunctions module definition--------------------------------*/

static PyModuleDef module_definition = {
    /*m_base*/ PyModuleDef_HEAD_INIT,
    /*m_name*/ "Freestyle.ContextFunctions",
    /*m_doc*/ module_docstring,
    /*m_size*/ -1,
    /*m_methods*/ module_functions,
    /*m_slots*/ nullptr,
    /*m_traverse*/ nullptr,
    /*m_clear*/ nullptr,
    /*m_free*/ nullptr,
};

//------------------- MODULE INITIALIZATION --------------------------------

int ContextFunctions_Init(PyObject *module)
{
  PyObject *m;

  if (module == nullptr) {
    return -1;
  }

  m = PyModule_Create(&module_definition);
  if (m == nullptr) {
    return -1;
  }
  PyModule_AddObjectRef(module, "ContextFunctions", m);

  return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////
