#include <Python.h>
#include <windows.h>
#include <GdiPlus.h>


static PyObject *_mmod;
static PyObject *_load_data_func;
static PyObject *_draw_page_func;
static PyObject *_empty_arg_tp;

typedef struct _PageGfxCtx {
	Gdiplus::Graphics* gfx;
	int page_nb;
	int doc_pt_x;
	int doc_pt_y;
	int screen_printing;
	float prn_rx;
	float prn_ry;
	float prn_off_pt_x;
	float prn_off_pt_y;
} PageGfxCtx, pPageGfxCtx;
static PageGfxCtx ctx;


static PyObject* labelx_get_doc_info(PyObject *self, PyObject *args)
{
	return Py_BuildValue("{s:i,s:i,s:i,s:f,s:f,s:f,s:f,s:i}",
		"page_nb", ctx.page_nb,
		"pt_x", ctx.doc_pt_x,
		"pt_y", ctx.doc_pt_y,
		"prn_rx", ctx.prn_rx,
		"prn_ry", ctx.prn_ry,
		"prn_off_pt_x", ctx.prn_off_pt_x,
		"prn_off_pt_y", ctx.prn_off_pt_y,
		"screen_printing", ctx.screen_printing);
}

static PyObject* labelx_draw_text(PyObject *self, PyObject *args, PyObject *kws)
{
	using namespace Gdiplus;
	wchar_t *str;
	float size;
	float x, y, w, h;
	unsigned int color = 0x00000000;
	int style = FontStyleRegular;
	wchar_t *family = L"Times New Roman";
	unsigned int format_flag = 0, format_align = 0, format_line_align = 0;
	char *kw_list[] = {"str", "size", "rect", "color", "style", "family", "format", 0};

	if( !PyArg_ParseTupleAndKeywords(args, kws, "uf(ffff)|Iiu(III)", kw_list,
		&str,
		&size,
		&x, &y, &w, &h,
		&color,
		&style,
		&family,
		&format_flag, &format_align, &format_line_align) ) return 0;

	Font font(family, size, style);
	SolidBrush brush(Color((color>>16) & 0xFF, (color>>8) & 0xFF, (color>>0) & 0xFF));
	if(format_flag) {
		StringFormat format(format_flag);
		format.SetAlignment((StringAlignment)format_align);
		format.SetLineAlignment((StringAlignment)format_line_align);
		
		if(w && h) {
			RectF rect(x, y, w, h);
			ctx.gfx->DrawString(str, -1, &font, rect, &format, &brush);
		} else {
			PointF point(x, y);
			ctx.gfx->DrawString(str, -1, &font, point, &format, &brush);
		}

	} else {
		PointF point(x, y);
		ctx.gfx->DrawString(str, -1, &font, point, &brush);
	}

	Py_RETURN_NONE;
}

static PyObject* label_draw_image(PyObject *self, PyObject *args)
{
	using namespace Gdiplus;
	wchar_t *nz;
	float x, y, w, h;
	if( !PyArg_ParseTuple(args, "u(ffff)", &nz, &x, &y, &w, &h) ) return 0;

	Image img(nz);
	float rt1 = float(img.GetHeight()) / float(img.GetWidth());
	float rt2 = h / w;

	float nw, nh;
	if(rt1 <= rt2) {
		nw = w;
		nh = nw * rt1;
	} else {
		nh = h;
		nw = nh / rt1;
	}

	float xo = (w - nw) / 2.0f + x;
	float yo = (h - nh) / 2.0f + y;

	ctx.gfx->DrawImage(&img, xo, yo, nw, nh);

	Py_RETURN_NONE;
}

static PyObject* labelx_fill_rect(PyObject *self, PyObject *args)
{
	using namespace Gdiplus;
	float x, y, w, h;
	unsigned int color = 0;
	if( !PyArg_ParseTuple(args, "(ffff)|I", &x, &y, &w, &h, &color) ) return 0;

	SolidBrush brush(Color((color>>16) & 0xFF, (color>>8) & 0xFF, (color>>0) & 0xFF));

	ctx.gfx->FillRectangle(&brush, x, y, w, h);

	Py_RETURN_NONE;
}

static PyObject* labelx_draw_rect(PyObject *self, PyObject *args)
{
	using namespace Gdiplus;
	float x, y, w, h;
	unsigned int color = 0;
	float width = 1;
	if( !PyArg_ParseTuple(args, "(ffff)If", &x, &y, &w, &h, &color, &width) ) return 0;

	Pen pen(Color((color>>16) & 0xFF, (color>>8) & 0xFF, (color>>0) & 0xFF), width);

	ctx.gfx->DrawRectangle(&pen, x, y, w, h);

	Py_RETURN_NONE;
}

static PyObject* labelx_draw_line(PyObject *self, PyObject *args)
{
	using namespace Gdiplus;
	float x, y, x1, y1;
	unsigned int color = 0;
	float width = 1;
	if( !PyArg_ParseTuple(args, "(ff)(ff)If", &x, &y, &x1, &y1, &color, &width) ) return 0;

	Pen pen(Color((color>>16) & 0xFF, (color>>8) & 0xFF, (color>>0) & 0xFF), width);

	ctx.gfx->DrawLine(&pen, x, y, x1, y1);

	Py_RETURN_NONE;
}

static PyMethodDef LabelMethods[] = {
	{"get_doc_info", labelx_get_doc_info, METH_NOARGS},
	{"draw_text", (PyCFunction)labelx_draw_text, METH_VARARGS|METH_KEYWORDS},
	{"fill_rect", labelx_fill_rect, METH_VARARGS},
	{"draw_rect", labelx_draw_rect, METH_VARARGS},
	{"draw_line", labelx_draw_line, METH_VARARGS},
	{"draw_image", label_draw_image, METH_VARARGS},
    {NULL, NULL, 0}
};

void lpy_init()
{
	/*
	PyObject *fpo;

	Py_DontWriteBytecodeFlag++;
	Py_NoSiteFlag++;
	Py_Initialize();
	PySys_SetPath(".;lib");

	fpo = PyFile_FromString("CONOUT$", "wb");
	PyFile_SetBufSize(fpo, 0);
	PySys_SetObject("stdout", fpo);
	Py_DECREF(fpo);
	fpo = PyFile_FromString("CONOUT$", "wb");
	PyFile_SetBufSize(fpo, 0);
	PySys_SetObject("stderr", fpo);
	Py_DECREF(fpo);
	*/

	Py_InitModule("_Label", LabelMethods);

	_empty_arg_tp = PyTuple_New(0);

	_mmod = PyImport_ImportModule("Label");
	if(!_mmod) goto _import_error;
	
	_load_data_func = PyObject_GetAttrString(_mmod, "load_data");
	if(!_load_data_func) goto _import_error;
	if(!PyCallable_Check(_load_data_func)) {
		Py_DECREF(_load_data_func);
		_load_data_func = 0;
		return;
	}

	_draw_page_func = PyObject_GetAttrString(_mmod, "draw_page");
	if(!_draw_page_func) goto _import_error;
	if(!PyCallable_Check(_draw_page_func)) {
		Py_DECREF(_draw_page_func);
		_draw_page_func = 0;
		return;
	}

	return;

_import_error:
	PyErr_Print();
}

void lpy_final()
{
	Py_XDECREF(_empty_arg_tp);
	Py_XDECREF(_draw_page_func);
	Py_XDECREF(_load_data_func);
	Py_XDECREF(_mmod);

	//Py_Finalize();
}

int lpy_load_data(const char *fnz, const char *tmpl, unsigned int flag)
{
	int max_page = 0;
	if(!_load_data_func) return 0;

	PyObject *tp = PyTuple_New(3);
	PyTuple_SetItem(tp, 0, PyString_FromString(fnz));
	PyTuple_SetItem(tp, 1, PyString_FromString(tmpl));
	PyTuple_SetItem(tp, 2, PyLong_FromUnsignedLong(flag));
	PyObject *ret = PyObject_CallObject(_load_data_func, tp);
	Py_DECREF(tp);

	if(ret) {
		max_page = PyInt_AsLong(ret);
		Py_DECREF(ret);
	} else PyErr_Print();

	return max_page;
}

int lpy_draw_page(Gdiplus::Graphics* gfx, int page_nb, int screen_printing, int pt_x, int pt_y, float prn_rx, float prn_ry, float prn_off_pt_x, float prn_off_pt_y)
{
	ctx.gfx = gfx;
	ctx.page_nb = page_nb;
	ctx.doc_pt_x = pt_x;
	ctx.doc_pt_y = pt_y;
	ctx.prn_rx = prn_rx;
	ctx.prn_ry = prn_ry;
	ctx.prn_off_pt_x = prn_off_pt_x;
	ctx.prn_off_pt_y = prn_off_pt_y;
	ctx.screen_printing = screen_printing;

	if(!_draw_page_func) return 0;
	PyObject *tp = PyTuple_New(2);
	PyTuple_SetItem(tp, 0, PyInt_FromLong(page_nb));
	PyTuple_SetItem(tp, 1, PyInt_FromLong(screen_printing));
	PyObject *ret = PyObject_CallObject(_draw_page_func, tp);
	Py_DECREF(tp);

	if(ret) Py_DECREF(ret);
	else PyErr_Print();

	return 0;
}

