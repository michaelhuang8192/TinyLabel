#include <stdio.h>
#include <windows.h>
#include <gdiplus.h>
#include "resource.h"
#include <Python.h>

typedef struct _PageDataArg {
	char file[MAX_PATH];
	char tmpl[MAX_PATH];
	int flag;
} PageDataArg, *pPageDataArg;
static PageDataArg g_data_arg;

typedef struct _PageReq {
	struct _PageReq *prev;
	struct _PageReq *next;
	int done;
	int page_nb;
	HDC hdc;
	HBITMAP bitmap;
	Gdiplus::Graphics *gfx;
	pPageDataArg data_arg;
} PageReq, *pPageReq;

typedef struct _PageReqMgr {
	CRITICAL_SECTION list_lock;
	HANDLE event_ready;
	PageReq head;
} PageReqMgr, *pPageReqMgr;

typedef struct _PageCtx
{
	unsigned int in_req;
	int max_page;
	HWND mw_hwnd;
	HDC mw_hdc;
	pPageReq *page_list;
	int page_list_sz;
	PageReq pr;

	HINSTANCE hInst;

	HDC pdc;
	HANDLE prn;
	HANDLE pth;
	HWND prn_hwnd;
	float prn_rx;
	float prn_ry;
	float prn_off_pt_x;
	float prn_off_pt_y;

	int cur_tmpl_id;

	PageReq cache_pr_head;
	int cache_pr_sz;

	int page_inuse_sidx;
	int page_inuse_eidx;
	int page_inreq_sidx;
	int page_inreq_eidx;

} PageCtx, *pPageCtx;
static PageCtx g_pctx;


#define PAGE_WIDTH_INCH 8.5
#define PAGE_HEIGHT_INCH 11
#define PAGE_WIDTH_POINT 612 //8.5 * 72
#define PAGE_HEIGHT_POINT 792 //11 * 72

#define PAGE_MARGIN_PX 10
#define PAGE_MARGIN_PY 10

#define IN_REQ_PRINTER (1<<30)

#define WM_USR_DRAW_PAGE_DONE (WM_USER + 1)
#define WM_USR_LOAD_DATA_DONE (WM_USER + 2)
#define WM_USR_PREPARING (WM_USER + 8)
#define WM_USR_PRINTING_DONE (WM_USER + 16)

#define PAGE_READ_AHEAD 1
#define PAGE_MAX_CACHE_SZ 5

static int g_inch_px;
static int g_inch_py;
static int g_page_px;
static int g_page_py;
static int g_page_total_px;
static int g_page_total_py;

static int scr_curposx;
static int scr_maxposx;
static int scr_newposx;
static int scr_client_px;

static int scr_curposy;
static int scr_maxposy;
static int scr_newposy;
static int scr_client_py;

static int scr_pg_sidx;
static int scr_pg_eidx;
static int scr_pg_new_sidx;
static int scr_pg_new_eidx;

static int scr_resizing;

static volatile int g_worker_stop;
static PageReqMgr g_prm;
static volatile int g_printer_stop;

void set_cur_dir_path();
LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
BOOL CALLBACK PrintingProc(HWND, UINT, WPARAM, LPARAM);
DWORD WINAPI worker_thread(LPVOID);
DWORD WINAPI printer_thread(LPVOID);
int add_tmpl_menu(HWND hwnd);


static int pr_list_in_list(pPageReq pr)
{
	return (pr->prev && pr->next && pr->prev != pr->next);
}

static int pr_list_empty(pPageReq head)
{
	return (head == head->next);
}

static void pr_list_init_item(pPageReq pr)
{
	pr->prev = pr->next = 0;
}

static void pr_list_init_head(pPageReq head)
{
	head->prev = head->next = head;
}

static pPageReq __pr_list_pop_back(pPageReq head)
{
	pPageReq pr = 0;

	if(!pr_list_empty(head)) {
		pr = head->prev;
		head->prev = pr->prev;
		pr->prev->next = head;
	}

	return pr;
}

static pPageReq pr_list_pop_back(pPageReq head)
{
	pPageReq pr = __pr_list_pop_back(head);
	if(pr) pr_list_init_item(pr);
	return pr;
}

static pPageReq __pr_list_remove(pPageReq pr)
{
	pr->prev->next = pr->next;
	pr->next->prev = pr->prev;
	return pr;
}

static pPageReq pr_list_remove(pPageReq pr)
{
	__pr_list_remove(pr);
	pr_list_init_item(pr);
	return pr;
}

static void pr_list_push(pPageReq head, pPageReq pr)
{
	pr->next = head->next;
	pr->next->prev = pr;
	pr->prev = head;
	head->next = pr;
}

static void pr_list_append(pPageReq head, pPageReq pr)
{
	pr->prev = head->prev;
	pr->prev->next = pr;
	pr->next = head;
	head->prev = pr;
}

static pPageReq alloc_pr()
{
	pPageReq pr;
	pr = (pPageReq)malloc( sizeof(PageReq) );
	memset(pr, 0x00, sizeof(PageReq));
	return pr;
}

static void free_pr(pPageReq pr)
{
	if(!pr) return;
	if(pr->gfx) delete pr->gfx;
	if(pr->hdc) DeleteDC(pr->hdc);
	if(pr->bitmap) DeleteObject(pr->bitmap);
	free(pr);
}

static void free_pr_list(pPageReq pr_head)
{
	pPageReq cur;
	pPageReq next = pr_head->next;
	while(pr_head != next) {
		cur = next;
		next = cur->next;
		free_pr(cur);
	}
}

static pPageReq get_pagereq()
{
	pPageReq pr = NULL;
	EnterCriticalSection(&g_prm.list_lock);
	pr = __pr_list_pop_back(&g_prm.head);
	LeaveCriticalSection(&g_prm.list_lock);
	if(pr) pr_list_init_item(pr);
	return pr;
}

static void init_pr(pPageReq pr, int page_nb=0)
{
	pr->hdc = CreateCompatibleDC(g_pctx.mw_hdc);
	pr->bitmap = CreateCompatibleBitmap(g_pctx.mw_hdc, g_page_px, g_page_py);
	SelectObject(pr->hdc, pr->bitmap);
	pr->gfx = new Gdiplus::Graphics(pr->hdc);
	pr->page_nb = page_nb;
	pr_list_init_item(pr);
}

static void set_pagereq(pPageReq pr)
{
	EnterCriticalSection(&g_prm.list_lock);
	pr_list_append(&g_prm.head, pr);
	LeaveCriticalSection(&g_prm.list_lock);
}

extern void lpy_init();
extern void lpy_final();
extern int lpy_load_data(const char* fnz, const char *tmpl, unsigned int flag);
extern int lpy_draw_page(Gdiplus::Graphics* gfx, int page_nb, int screen_printing, int pt_x, int pt_y, float prn_rx, float prn_ry, float prn_off_pt_x, float prn_off_pt_y);



HINSTANCE gDllHinst;
BOOL WINAPI DllMain(
  _In_ HINSTANCE hinstDLL,
  _In_ DWORD     fdwReason,
  _In_ LPVOID    lpvReserved
)
{
	if(fdwReason == DLL_PROCESS_ATTACH) {
		gDllHinst = hinstDLL;
	}

	return TRUE;
}


//int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
int MainLoop(HINSTANCE hInstance, int nCmdShow)
{
	HWND hwnd;
	MSG msg;
	WNDCLASSEX wcx;
	HANDLE wt;

	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	ULONG_PTR gdiplusToken;
	Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

	//AllocConsole();
	//freopen("CONOUT$", "w", stdout);
	//freopen("CONOUT$", "w", stderr);
	
	//set_cur_dir_path();

	lpy_init();

	memset(&g_pctx, 0x00, sizeof(g_pctx));
	g_pctx.hInst = hInstance;
	pr_list_init_head(&g_pctx.cache_pr_head);
	g_printer_stop = 1;
	g_data_arg.file[0] = 0;
	g_data_arg.tmpl[0] = 0;
	scr_resizing = 1;
	scr_client_px = scr_newposx = scr_curposx = scr_maxposx = 0;
	scr_client_py = scr_newposy = scr_curposy = scr_maxposy = 0;
	g_pctx.page_inuse_sidx = g_pctx.page_inreq_sidx = 0;
	g_pctx.page_inuse_eidx = g_pctx.page_inreq_eidx = -1;

	g_worker_stop = 0;
	InitializeCriticalSection(&g_prm.list_lock);
	g_prm.event_ready = CreateEvent(0, 0, 0, 0);
	pr_list_init_head(&g_prm.head);
	wt = CreateThread(0, 0, &worker_thread, 0, 0, 0);

	memset(&wcx, 0x00, sizeof(wcx));
	wcx.cbSize = sizeof(wcx);
	wcx.style = CS_HREDRAW | CS_VREDRAW;
	wcx.lpfnWndProc = MainWndProc;
	wcx.cbClsExtra = 0;
	wcx.cbWndExtra = 0;
	wcx.hInstance = hInstance;
	wcx.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
	wcx.lpszMenuName =  "LabelMenu";
	wcx.lpszClassName = "LabelWClass";
    RegisterClassEx(&wcx); 

	hwnd = CreateWindow("LabelWClass", "TinyLabel", WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		NULL, LoadMenu(hInstance, MAKEINTRESOURCE(IDR_MENU1)), hInstance, NULL);
	
	add_tmpl_menu(hwnd);

	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);
	while(GetMessage(&msg, 0, 0, 0) > 0)
	{ 
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	
	if(g_pctx.pdc) {
		DeleteDC(g_pctx.pdc);
		ClosePrinter(g_pctx.prn);
		g_pctx.pdc = 0;
		g_pctx.prn = 0;
	}

	g_worker_stop = 1;
	SetEvent(g_prm.event_ready);
	WaitForSingleObject(wt, -1);
	CloseHandle(wt);
	DeleteCriticalSection(&g_prm.list_lock);
	CloseHandle(g_prm.event_ready);
	free_pr_list(&g_prm.head);

	if(g_pctx.page_list) {
		for(int i = 0; i < g_pctx.max_page; i++) {
			if(g_pctx.page_list[i]) free_pr(g_pctx.page_list[i]);
		}
		free(g_pctx.page_list);
	}
	g_pctx.max_page = 0;
	g_pctx.page_list_sz = 0;
	ReleaseDC(hwnd, g_pctx.mw_hdc);

	lpy_final();
	Gdiplus::GdiplusShutdown(gdiplusToken);

	return 0;
}

static void set_cur_dir_path()
{
	char buf[MAX_PATH];
	char *tmp;

	if( !GetModuleFileName(0, buf, sizeof(buf)) ) return;

	tmp = strrchr((char*)buf, '\\');
	if(!tmp) return;

	tmp[0] = 0x00;
	SetCurrentDirectory(buf);
}

static int add_tmpl_menu(HWND hwnd)
{
	char *chr;
	WIN32_FIND_DATA fd;
	HMENU popup_menu;
	HMENU menu = GetMenu(hwnd);
	UINT pm_id = ID_TEMPLATE;

	if(!menu) return 0;
	popup_menu = CreatePopupMenu();
	AppendMenu(menu, MF_STRING | MF_POPUP, (UINT_PTR)popup_menu, "Template");

	HANDLE fh = FindFirstFile("tmpl\\tmpl_*", &fd);
	if(fh == INVALID_HANDLE_VALUE) return 0;
	
	do {
		chr = strrchr(fd.cFileName, '.');
		if(!chr) continue;
		chr[0] = 0x00;
		chr = strchr(fd.cFileName, '_');
		if(!chr) continue;

		if(!g_pctx.cur_tmpl_id) {
			g_pctx.cur_tmpl_id = pm_id;
			strcpy(g_data_arg.tmpl, chr+1);
		}
		AppendMenu(popup_menu, MF_STRING, pm_id++, ++chr);

	} while(FindNextFile(fh, &fd) && pm_id <= ID_TEMPLATE_MAX);
	FindClose(fh);

	return 1;
}

static void _init_page(Gdiplus::Graphics *gfx)
{
	gfx->SetPageUnit(Gdiplus::UnitPoint);
	gfx->Clear(Gdiplus::Color(255, 0xFF, 0xFF, 0xFF));
	gfx->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
	gfx->SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
}

static DWORD WINAPI worker_thread(LPVOID lpParameter)
{
	pPageReq pr;

	while(!g_worker_stop) {
		WaitForSingleObject(g_prm.event_ready, INFINITE);
		if(g_worker_stop) break;

		while(!g_worker_stop) {
			pr = get_pagereq();
			if(!pr) break;

			if(pr->page_nb) {
				_init_page(pr->gfx);
				Gdiplus::Font font(L"Arial", 12, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
				Gdiplus::RectF rect(0.0f, 0.0f, float(PAGE_WIDTH_POINT), 14.0f);
				Gdiplus::SolidBrush brush(Gdiplus::Color(255, 0, 0, 0));
				Gdiplus::StringFormat strformat;
				strformat.SetAlignment(Gdiplus::StringAlignmentCenter);

				wchar_t buf[256];
				int len = swprintf(buf, L"[ Page %d ]", pr->page_nb);
				pr->gfx->DrawString(buf, len, &font, rect, &strformat, &brush);

				lpy_draw_page(pr->gfx, pr->page_nb, 1, PAGE_WIDTH_POINT, PAGE_HEIGHT_POINT,
					g_pctx.prn_rx, g_pctx.prn_ry, g_pctx.prn_off_pt_x, g_pctx.prn_off_pt_y);
				PostMessage(g_pctx.mw_hwnd, WM_USR_DRAW_PAGE_DONE, pr->page_nb, (LPARAM)pr);
			} else {
				int max_page = lpy_load_data(pr->data_arg->file, pr->data_arg->tmpl, pr->data_arg->flag);
				PostMessage(g_pctx.mw_hwnd, WM_USR_LOAD_DATA_DONE, max_page, (LPARAM)pr);
			}

		}
	}

	return 0;
}

static int open_data()
{
	char buf[MAX_PATH];
	OPENFILENAME ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = g_pctx.mw_hwnd;
	ofn.lpstrFile = buf;
	ofn.lpstrFile[0] = 0x00;
	ofn.nMaxFile = sizeof(buf);
	ofn.lpstrFilter = "CSV\0*.CSV\0";
	ofn.nFilterIndex = 0;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = NULL;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

	if(!GetOpenFileName(&ofn) || !strlen(buf)) return 0;
	strcpy(g_data_arg.file, buf);

	return 1;
}

static int open_printer()
{
	PRINTDLG pd;
	DEVNAMES *pnz_o;
	char* pnz;

	ZeroMemory(&pd, sizeof(pd));
	pd.lStructSize = sizeof(pd);
	pd.hwndOwner = g_pctx.mw_hwnd;
	pd.Flags = PD_RETURNDC;
	if(!PrintDlg(&pd)) return 0;

	if(g_pctx.pdc) {
		DeleteDC(g_pctx.pdc);
		ClosePrinter(g_pctx.prn);
	}

	pnz_o = *(DEVNAMES**)pd.hDevNames;
	pnz = (char*)pnz_o + pnz_o->wDeviceOffset;
	OpenPrinter(pnz, &g_pctx.prn, NULL);
	g_pctx.pdc = pd.hDC;

	if(pd.hDevMode) GlobalFree(pd.hDevMode);
	if(pd.hDevNames) GlobalFree(pd.hDevNames);

	int lpx = GetDeviceCaps(g_pctx.pdc, LOGPIXELSX);
	int lpy = GetDeviceCaps(g_pctx.pdc, LOGPIXELSY);
	g_pctx.prn_off_pt_x = GetDeviceCaps(g_pctx.pdc, PHYSICALOFFSETX) * 72.0f / lpx;
	g_pctx.prn_off_pt_y = GetDeviceCaps(g_pctx.pdc, PHYSICALOFFSETY) * 72.0f / lpy;
	g_pctx.prn_rx = float(PAGE_WIDTH_INCH) * lpx / GetDeviceCaps(g_pctx.pdc, HORZRES);
	g_pctx.prn_ry = float(PAGE_HEIGHT_INCH) * lpy / GetDeviceCaps(g_pctx.pdc, VERTRES);

	printf("%d \n", GetDeviceCaps(g_pctx.pdc, VERTRES));

	return 1;
}

static int do_printing()
{
	//if(!g_pctx.pdc && !open_printer()) return 0;

	g_pctx.in_req += IN_REQ_PRINTER;
	DialogBox(g_pctx.hInst, MAKEINTRESOURCE(IDD_PRINTING_DIALOG), g_pctx.mw_hwnd, &PrintingProc);
	g_pctx.in_req -= IN_REQ_PRINTER;
	return 1;
}

static DWORD WINAPI printer_thread(LPVOID lpParameter)
{
	char buf[64];
	DOCINFO docinfo;
	int i;
	Gdiplus::Graphics *gfx;
	gfx = new Gdiplus::Graphics(g_pctx.pdc, g_pctx.prn);

	docinfo.cbSize = sizeof(docinfo);
	docinfo.lpszDocName = "labelx";

	StartDoc(g_pctx.pdc, &docinfo);
	for(i = 1; i <= g_pctx.max_page && !g_printer_stop; i++) {
		sprintf(buf, "Printing Page %d of %d", i, g_pctx.max_page);
		SetDlgItemText(g_pctx.prn_hwnd, ID_PRN_DLG_MSG, buf);
		StartPage(g_pctx.pdc);
		_init_page(gfx);
		lpy_draw_page(gfx, i, 0, PAGE_WIDTH_POINT, PAGE_HEIGHT_POINT,
			g_pctx.prn_rx, g_pctx.prn_ry, g_pctx.prn_off_pt_x, g_pctx.prn_off_pt_y);
		EndPage(g_pctx.pdc);
	}
	EndDoc(g_pctx.pdc);

	delete gfx;

	PostMessage(g_pctx.prn_hwnd, WM_USR_PRINTING_DONE, 0, 0);

	return 0;
}

static void limit_page_cache()
{
	pPageReq pr;
	int cur_pr_sz = g_pctx.cache_pr_sz;

	
	if(cur_pr_sz <= PAGE_MAX_CACHE_SZ + PAGE_READ_AHEAD * 2) return;
	while(cur_pr_sz > PAGE_MAX_CACHE_SZ + PAGE_READ_AHEAD * 2) {
		pr = pr_list_pop_back(&g_pctx.cache_pr_head);
		cur_pr_sz--;

		g_pctx.page_list[ pr->page_nb - 1 ] = 0;
		//printf("count: %d - delete: %d\n", cur_pr_sz, pr->page_nb);
		free_pr(pr);
	}
	g_pctx.cache_pr_sz = cur_pr_sz;
}

static int check_page(int start_idx, int end_idx)
{
	int ready = 1;
	int new_req = 0;
	pPageReq pr;
	int i;

	if(!g_pctx.max_page) return ready;

	if(g_pctx.page_inreq_sidx != start_idx || g_pctx.page_inreq_eidx != end_idx) {

		for(i = g_pctx.page_inreq_sidx; i <= g_pctx.page_inreq_eidx; i++) {
			if(i >= start_idx && i <= end_idx) continue;
			if(i >= g_pctx.page_inuse_sidx && i <= g_pctx.page_inuse_eidx) continue;
			if(g_pctx.page_list[i] && g_pctx.page_list[i]->done) {
				pr_list_push(&g_pctx.cache_pr_head, g_pctx.page_list[i]);
				g_pctx.cache_pr_sz++;
				//printf("Remove0: %d\n", i + 1);
			}
		}

		g_pctx.page_inreq_sidx = start_idx;
		g_pctx.page_inreq_eidx = end_idx;
	}

	for(i = start_idx; i <= end_idx; i++) {
		if(g_pctx.page_list[i] && g_pctx.page_list[i]->done) {
			if( pr_list_in_list(g_pctx.page_list[i]) ) {
				pr_list_remove(g_pctx.page_list[i]);
				g_pctx.cache_pr_sz--;
				//printf("in_cache: %d\n", i + 1);
			}
			continue;
		}

		ready = 0;
		if(!g_pctx.page_list[i]) {
			new_req++;
			g_pctx.in_req++;
			g_pctx.page_list[i] = pr = alloc_pr();
			init_pr(pr, i + 1);
			set_pagereq(pr);
			//printf("Req: %d\n", i + 1);
		}
	}

	if(new_req) SetEvent(g_prm.event_ready);

	if(ready) {
		if(g_pctx.page_inreq_sidx != g_pctx.page_inuse_sidx || g_pctx.page_inreq_eidx != g_pctx.page_inuse_eidx) {
			for(i = g_pctx.page_inuse_sidx; i <= g_pctx.page_inuse_eidx; i++) {
				if(i >= g_pctx.page_inreq_sidx && i <= g_pctx.page_inreq_eidx) continue;
				pr_list_push(&g_pctx.cache_pr_head, g_pctx.page_list[i]);
				g_pctx.cache_pr_sz++;
				//printf("Remove1: %d\n", i + 1);
			}

			g_pctx.page_inuse_sidx = g_pctx.page_inreq_sidx;
			g_pctx.page_inuse_eidx = g_pctx.page_inreq_eidx;
		}
	}

	limit_page_cache();

	return ready;
}

static void get_cur_page_idx(int cur_pos, int *sidx, int *eidx)
{
	int s, e;
	s = cur_pos / g_page_total_py;
	e = (cur_pos + scr_client_py - 1) / g_page_total_py;
	e = min(g_pctx.max_page - 1, e);

	*sidx = s;
	*eidx = e;
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam)
{
	SCROLLINFO si;
	HDC hdc;
	PAINTSTRUCT ps;
	RECT rect;
	int usr_loop = 0;
	int done = 0;
	pPageReq pr;

	while(!done) {
		done = 1;
		switch(umsg)
		{
	
		case WM_USR_DRAW_PAGE_DONE:
			{
				int page_nb = (int)wparam;
				int page_idx = page_nb - 1;
				pr = g_pctx.page_list[page_idx];
				pr->done = 1;
				g_pctx.in_req--;

				if(page_idx < g_pctx.page_inreq_sidx || page_idx > g_pctx.page_inreq_eidx) {
					pr_list_push(&g_pctx.cache_pr_head, pr);
					g_pctx.cache_pr_sz++;
					limit_page_cache();
					break;
				}
			}
		case WM_USR_PREPARING:
			{
				if(scr_resizing) {
					if( check_page(scr_pg_sidx, scr_pg_eidx) ) {
						scr_resizing = 0;
						InvalidateRect(hwnd, NULL, 1);
					}

				} else if(scr_curposx != scr_newposx || scr_curposy != scr_newposy) {

					if( check_page(scr_pg_new_sidx, scr_pg_new_eidx) ) {
						int dx = scr_curposx - scr_newposx;
						int dy = scr_curposy - scr_newposy;

						if(scr_curposx != scr_newposx) {
							scr_curposx = scr_newposx;
							si.cbSize = sizeof(si);
							si.fMask  = SIF_POS;
							si.nPos   = scr_curposx;
							SetScrollInfo(hwnd, SB_HORZ, &si, TRUE);
						}

						if(scr_curposy != scr_newposy) {
							scr_curposy = scr_newposy;
							scr_pg_sidx = scr_pg_new_sidx;
							scr_pg_eidx = scr_pg_new_eidx;
							si.cbSize = sizeof(si);
							si.fMask  = SIF_POS;
							si.nPos   = scr_curposy;
							SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
						}

						ScrollWindowEx(hwnd, dx, dy, NULL, NULL, NULL, NULL, SW_INVALIDATE | SW_ERASE);
						UpdateWindow(hwnd);
					}
				}

			}
			break;

		case WM_USR_LOAD_DATA_DONE:
			{
				g_pctx.max_page = (int)wparam;

				if(g_pctx.max_page > g_pctx.page_list_sz) {
					if(g_pctx.page_list_sz)
						g_pctx.page_list = (pPageReq*)realloc(g_pctx.page_list, sizeof(pPageReq) * g_pctx.max_page);
					else
						g_pctx.page_list = (pPageReq*)malloc(sizeof(pPageReq) * g_pctx.max_page);
					g_pctx.page_list_sz = g_pctx.max_page;
				
				}
				memset(g_pctx.page_list, 0x00, sizeof(pPageReq) * g_pctx.page_list_sz);

				g_pctx.in_req--;
				usr_loop = 1;
				umsg = WM_SIZE;
				done = 0;
				//printf("WM_USR_LOAD_DATA_DONE: %d\n", g_pctx.page_list_sz);
			}
			break;

		case WM_COMMAND:
			{
				if( !HIWORD(wparam) ) {
					int idm = LOWORD(wparam);
					int flag = 0;
					if(g_pctx.in_req) break;

					if(idm >= ID_TEMPLATE && idm <= ID_TEMPLATE_MAX) {
						g_pctx.cur_tmpl_id = idm;
						GetMenuString(GetMenu(g_pctx.mw_hwnd), idm, g_data_arg.tmpl, sizeof(g_data_arg.file), 0);
						idm = ID_RELOADDATA; flag = 0x02;

					} else if(idm == ID_SELECTPRINTER) {
						if(open_printer()) idm = ID_RELOADDATA;

					} else if(idm == ID_PRINT) {
						if(g_pctx.max_page) {
							if(!g_pctx.pdc && open_printer()) idm = ID_RELOADDATA;
							if(g_pctx.pdc) do_printing();
						}

					} else if(idm == ID_RELOADDATA) {
						flag = 0x01;

					} else if(idm == ID_LOADDATA) {
						flag = 0x03;

					}
					
					if(idm == ID_LOADDATA || idm == ID_RELOADDATA) {
						if(!g_pctx.cur_tmpl_id || idm == ID_LOADDATA && !open_data() || !strlen(g_data_arg.file)) break;

						if(g_pctx.max_page) {
							pr_list_init_head(&g_pctx.cache_pr_head);
							g_pctx.cache_pr_sz = 0;
							for(int i = 0; i < g_pctx.max_page; i++)
								if(g_pctx.page_list[i]) free_pr(g_pctx.page_list[i]);
						}

						scr_resizing = 1;
						scr_client_px = scr_newposx = scr_curposx = scr_maxposx = 0;
						scr_client_py = scr_newposy = scr_curposy = scr_maxposy = 0;
						g_pctx.page_inuse_sidx = g_pctx.page_inreq_sidx = 0;
						g_pctx.page_inuse_eidx = g_pctx.page_inreq_eidx = -1;

						g_pctx.max_page = 0;
						g_pctx.in_req = 1;
						g_data_arg.flag = flag;
						g_pctx.pr.data_arg = &g_data_arg;
						set_pagereq(&g_pctx.pr);
						SetEvent(g_prm.event_ready);

					}

				}

			}
			break;

		case WM_PAINT:
			{
				if(scr_resizing || !g_pctx.max_page) return DefWindowProc(hwnd, umsg, wparam, lparam);
				if( GetUpdateRect(hwnd, &rect, 1) ) {
					hdc = BeginPaint(hwnd, &ps);
					int s_l, s_r, u_l, u_r, d_l;
					s_l = scr_maxposx ? PAGE_MARGIN_PX : (scr_client_px - g_page_total_px) / 2 + PAGE_MARGIN_PX;
					s_l -= scr_curposx;
					s_r = s_l + g_page_px;
					u_l = ps.rcPaint.left;
					u_r = ps.rcPaint.right;
					d_l = 0;

					if(u_r > s_l && u_l < s_r) {
						if(s_l < u_l) {
							d_l = u_l - s_l;
							s_l = u_l;
						}
						if(s_r > u_r) s_r = u_r;

						int s_t, s_b, u_t, u_b, d_t;
						u_t = ps.rcPaint.top;
						u_b = ps.rcPaint.bottom;
						//printf("%d - %d\n", s_l, s_r);
						for(int i = scr_pg_sidx; i <= scr_pg_eidx; i++) {
							if(g_pctx.page_list[i] && g_pctx.page_list[i]->done) {
								s_t = i * g_page_total_py + PAGE_MARGIN_PY - scr_curposy;
								s_b = s_t + g_page_py;
								d_t = 0;

								//printf("%d - %d - %d : %d - %d\n", i, u_b, s_t, u_t, s_b);

								if(u_b <= s_t) break;
								if(u_t >= s_b) continue;

								if(s_t < u_t) {
									d_t = u_t - s_t;
									s_t = u_t;
								}
								if(s_b > u_b) s_b = u_b;

								BitBlt(ps.hdc, s_l, s_t, s_r - s_l, s_b - s_t, g_pctx.page_list[i]->hdc, d_l, d_t, SRCCOPY);
								//printf("+++++\n");
							}
						}
					}
					EndPaint(hwnd, &ps);
					//printf("paint: %d - %d - %d - %d - %d\n", ps.fErase, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
				}
			}
			break;
		
		case WM_CREATE:
			{
				hdc = GetDC(hwnd);
				g_pctx.mw_hwnd = hwnd;
				g_pctx.mw_hdc = hdc;
				g_inch_px = GetDeviceCaps(hdc, LOGPIXELSX);
				g_inch_py = GetDeviceCaps(hdc, LOGPIXELSY);
				g_page_px = int(PAGE_WIDTH_INCH * g_inch_px);
				g_page_py = int(PAGE_HEIGHT_INCH * g_inch_py);
				g_page_total_px = g_page_px + PAGE_MARGIN_PX * 2;
				g_page_total_py = g_page_py + PAGE_MARGIN_PY * 2;

			};
			break;

		case WM_SIZE:
			{
				int x = LOWORD(lparam);
				int y = HIWORD(lparam);
		
				if(usr_loop || !x && ! y) {
					GetClientRect(hwnd, &rect);
					x = rect.right - rect.left;
					y = rect.bottom - rect.top;
				}

				scr_client_px = max(1, x);
				scr_maxposx = max(g_page_total_px - scr_client_px, 0);
				scr_curposx = min(scr_curposx, scr_maxposx);
				si.cbSize = sizeof(si);
				si.fMask = SIF_DISABLENOSCROLL | SIF_RANGE | SIF_PAGE | SIF_POS;
				si.nMin = 0;
				si.nMax = g_page_total_px;
				si.nPage = scr_client_px;
				si.nPos = scr_curposx;
				scr_newposx = scr_curposx = SetScrollInfo(hwnd, SB_HORZ, &si, TRUE);

				scr_client_py = max(1, y);
				scr_maxposy = max(g_page_total_py * g_pctx.max_page - scr_client_py, 0);
				scr_curposy = min(scr_curposy, scr_maxposy);
				si.cbSize = sizeof(si);
				si.fMask = SIF_DISABLENOSCROLL | SIF_RANGE | SIF_PAGE | SIF_POS;
				si.nMin = 0;
				si.nMax = g_page_total_py * g_pctx.max_page;
				si.nPage = scr_client_py;
				si.nPos = scr_curposy;
				scr_newposy = scr_curposy = SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

				get_cur_page_idx(scr_curposy, &scr_pg_new_sidx, &scr_pg_new_eidx);
				scr_pg_sidx = scr_pg_new_sidx;
				scr_pg_eidx = scr_pg_new_eidx;

				scr_resizing = 1;
				umsg = WM_USR_PREPARING;
				done = 0;

				//printf("WM_SIZE\n");
			}
			break;

		case WM_HSCROLL:
			{
				if(scr_resizing || !g_pctx.max_page) break;
				switch(LOWORD(wparam))
				{
				case SB_PAGEUP: scr_newposx = scr_curposx - g_page_total_px; break;
				case SB_PAGEDOWN: scr_newposx = scr_curposx + g_page_total_px; break;
				case SB_LINEUP: scr_newposx = scr_curposx - g_inch_px; break;
				case SB_LINEDOWN: scr_newposx = scr_curposx + g_inch_px; break;
				case SB_THUMBPOSITION: scr_newposx = HIWORD(wparam); break;
				default: return 0;
				}

				scr_newposx = max(0, scr_newposx);
				scr_newposx = min(scr_maxposx, scr_newposx);

				umsg = WM_USR_PREPARING;
				done = 0;
			}
			break;

		case WM_MOUSEWHEEL:
			{
				wparam = scr_curposy - GET_WHEEL_DELTA_WPARAM(wparam);
				usr_loop = 1;
				umsg = WM_VSCROLL;
				done = 0;
			}
			break;
		case WM_VSCROLL:
			{
				if(scr_resizing || !g_pctx.max_page) break;
				if(usr_loop)
					scr_newposy = wparam;
				else {
					switch(LOWORD(wparam))
					{
					case SB_PAGEUP: scr_newposy = scr_curposy - g_page_total_py; break;
					case SB_PAGEDOWN: scr_newposy = scr_curposy + g_page_total_py; break;
					case SB_LINEUP: scr_newposy = scr_curposy - g_inch_py; break;
					case SB_LINEDOWN: scr_newposy = scr_curposy + g_inch_py; break;
					case SB_THUMBPOSITION:
						{
							si.cbSize = sizeof(si);
							si.fMask = SIF_TRACKPOS;
							GetScrollInfo(hwnd, SB_VERT, &si);
							scr_newposy = si.nTrackPos;
						}
						break;
					default: return 0;
					}
				}
				scr_newposy = max(0, scr_newposy);
				scr_newposy = min(scr_maxposy, scr_newposy);
				get_cur_page_idx(scr_newposy, &scr_pg_new_sidx, &scr_pg_new_eidx);
				umsg = WM_USR_PREPARING;
				done = 0;
			}
			break;

		case WM_CLOSE:
			PostQuitMessage(0);

		default:
			return DefWindowProc(hwnd, umsg, wparam, lparam);
		}

	}

	return 0;
}

static BOOL CALLBACK PrintingProc(HWND dlghwnd, UINT umsg, WPARAM wparam, LPARAM lparam)
{
	switch (umsg) {
	case WM_INITDIALOG:
		{
			g_pctx.prn_hwnd = dlghwnd;
			g_printer_stop = 0;
			g_pctx.pth = CreateThread(0, 0, printer_thread, &g_pctx, 0, 0);
		}
		break;

	case WM_COMMAND:
		{
			if(LOWORD(wparam) == ID_PRN_DLG_CANCEL)
				g_printer_stop = 1;
		}
		break;

	case WM_USR_PRINTING_DONE:
		{
			WaitForSingleObject(g_pctx.pth, INFINITE);
			CloseHandle(g_pctx.pth);
			g_pctx.pth = 0;
			g_pctx.prn_hwnd = 0;
			g_printer_stop = 1;

			EndDialog(dlghwnd, wparam);
			return 1;
		}
		break;

	default:
		break;
	}

	return 0;
}



static PyObject* TinyLabel_MainLoop(PyObject *self, PyObject *args)
{
	MainLoop(gDllHinst, 1);
	Py_RETURN_NONE;
}
static PyMethodDef TinyLabelMethods[] = {
	{"MainLoop", TinyLabel_MainLoop, METH_NOARGS},
    {NULL, NULL, 0}
};

PyMODINIT_FUNC
initTinyLabel(void)
{
	Py_InitModule("TinyLabel", TinyLabelMethods);
}