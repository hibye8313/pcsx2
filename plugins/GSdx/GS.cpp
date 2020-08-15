/*
 *	Copyright (C) 2007-2009 Gabest
 *	http://www.gabest.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include <sstream>
#include <memory>

#include "stdafx.h"
#include "GSdx.h"
#include "GSUtil.h"
#include "Renderers/SW/GSRendererSW.h"
#include "Renderers/Null/GSRendererNull.h"
#include "Renderers/Null/GSDeviceNull.h"
#include "Renderers/OpenGL/GSDeviceOGL.h"
#include "Renderers/OpenGL/GSRendererOGL.h"
#include "Renderers/OpenCL/GSRendererCL.h"
#include "GSLzma.h"

#ifdef _WIN32

#include "Renderers/DX11/GSRendererDX11.h"
#include "Renderers/DX11/GSDevice11.h"
#include "Window/GSWndDX.h"
#include "Window/GSWndWGL.h"
#include "Window/GSSettingsDlg.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <tchar.h>

#include "MyDebug.h"

static HRESULT s_hr = E_FAIL;

#else

#include "Window/GSWndEGL.h"

extern bool RunLinuxDialog();

#endif

#define PS2E_LT_GS 0x01
#define PS2E_GS_VERSION 0x0006
#define PS2E_X86 0x01   // 32 bit
#define PS2E_X86_64 0x02   // 64 bit

static GSRenderer* s_gs = NULL;
static void (*s_irq)() = NULL;
static uint8* s_basemem = NULL;
static int s_vsync = 0;
static bool s_exclusive = true;
static const char *s_renderer_name = "";
static const char *s_renderer_type = "";
bool gsopen_done = false; // crash guard for GSgetTitleInfo2 and GSKeyEvent (replace with lock?)

EXPORT_C_(uint32) PS2EgetLibType()
{
	return PS2E_LT_GS;
}

EXPORT_C_(const char*) PS2EgetLibName()
{
	return GSUtil::GetLibName();
}

EXPORT_C_(uint32) PS2EgetLibVersion2(uint32 type)
{
	const uint32 revision = 1;
	const uint32 build = 2;

	return (build << 0) | (revision << 8) | (PS2E_GS_VERSION << 16) | (PLUGIN_VERSION << 24);
}

EXPORT_C_(uint32) PS2EgetCpuPlatform()
{
#ifdef _M_AMD64

	return PS2E_X86_64;

#else

	return PS2E_X86;

#endif
}

EXPORT_C GSsetBaseMem(uint8* mem)
{
	s_basemem = mem;

	if(s_gs)
	{
		s_gs->SetRegsMem(s_basemem);
	}
}

EXPORT_C GSsetSettingsDir(const char* dir)
{
	theApp.SetConfigDir(dir);
}

EXPORT_C_(int) GSinit()
{
	if(!GSUtil::CheckSSE())
	{
		return -1;
	}

	// Vector instructions must be avoided when initialising GSdx since PCSX2
	// can crash if the CPU does not support the instruction set.
	// Initialise it here instead - it's not ideal since we have to strip the
	// const type qualifier from all the affected variables.
	theApp.Init();

	GSUtil::Init();
	GSBlock::InitVectors();
	GSClut::InitVectors();
#ifdef ENABLE_OPENCL
	GSRendererCL::InitVectors();
#endif
	GSRendererSW::InitVectors();
	GSVector4i::InitVectors();
	GSVector4::InitVectors();
#if _M_SSE >= 0x500
	GSVector8::InitVectors();
#endif
#if _M_SSE >= 0x501
	GSVector8i::InitVectors();
#endif
	GSVertexTrace::InitVectors();

	if (g_const == nullptr)
		return -1;
	else
		g_const->Init();

#ifdef _WIN32
	s_hr = ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
#endif

	return 0;
}

EXPORT_C GSshutdown()
{
	gsopen_done = false;

	delete s_gs;
	s_gs = nullptr;

	theApp.SetCurrentRendererType(GSRendererType::Undefined);

#ifdef _WIN32
	if(SUCCEEDED(s_hr))
	{
		::CoUninitialize();

		s_hr = E_FAIL;
	}
#endif
}

EXPORT_C GSclose()
{
	gsopen_done = false;

	if(s_gs == NULL) return;

	s_gs->ResetDevice();

	// Opengl requirement: It must be done before the Detach() of
	// the context
	delete s_gs->m_dev;

	s_gs->m_dev = NULL;

	if (s_gs->m_wnd)
	{
		s_gs->m_wnd->Detach();
	}
}

static int _GSopen(void** dsp, const char* title, GSRendererType renderer, int threads = -1)
{
	GSDevice* dev = NULL;
	bool old_api = *dsp == NULL;

	// Fresh start up or config file changed
	if(renderer == GSRendererType::Undefined)
	{
		renderer = static_cast<GSRendererType>(theApp.GetConfigI("Renderer"));
#ifdef _WIN32
		if (renderer == GSRendererType::Default)
			renderer = GSUtil::GetBestRenderer();
#endif
	}

	if(threads == -1)
	{
		threads = theApp.GetConfigI("extrathreads");
	}

	try
	{
		if (theApp.GetCurrentRendererType() != renderer)
		{
			// Emulator has made a render change request, which requires a completely
			// new s_gs -- if the emu doesn't save/restore the GS state across this
			// GSopen call then they'll get corrupted graphics, but that's not my problem.

			delete s_gs;

			s_gs = NULL;

			theApp.SetCurrentRendererType(renderer);
		}

		std::shared_ptr<GSWnd> window;
		{
			// Select the window first to detect the GL requirement
			std::vector<std::shared_ptr<GSWnd>> wnds;
			switch (renderer)
			{
				case GSRendererType::OGL_HW:
				case GSRendererType::OGL_SW:
#ifdef ENABLE_OPENCL
				case GSRendererType::OGL_OpenCL:
#endif
#if defined(__unix__)
					// Note: EGL code use GLX otherwise maybe it could be also compatible with Windows
					// Yes OpenGL code isn't complicated enough !
					switch (GSWndEGL::SelectPlatform()) {
#if GS_EGL_X11
						case EGL_PLATFORM_X11_KHR:
							wnds.push_back(std::make_shared<GSWndEGL_X11>());
							break;
#endif
#if GS_EGL_WL
						case EGL_PLATFORM_WAYLAND_KHR:
							wnds.push_back(std::make_shared<GSWndEGL_WL>());
							break;
#endif
						default:
							break;
					}
#else
					wnds.push_back(std::make_shared<GSWndWGL>());
#endif
					break;
				default:
#ifdef _WIN32
					wnds.push_back(std::make_shared<GSWndDX>());
#else
					wnds.push_back(std::make_shared<GSWndEGL_X11>());
#endif
					break;
			}

			int w = theApp.GetConfigI("ModeWidth");
			int h = theApp.GetConfigI("ModeHeight");
#if defined(__unix__)
			void *win_handle = (void*)((uptr*)(dsp)+1);
#else
			void *win_handle = *dsp;
#endif

			for(auto& wnd : wnds)
			{
				try
				{
					if (old_api)
					{
						// old-style API expects us to create and manage our own window:
						wnd->Create(title, w, h);

						wnd->Show();

						*dsp = wnd->GetDisplay();
					}
					else
					{
						wnd->Attach(win_handle, false);
					}

					window = wnd; // Previous code will throw if window isn't supported

					break;
				}
				catch (GSDXRecoverableError)
				{
					wnd->Detach();
				}
			}

			if(!window)
			{
				GSclose();

				return -1;
			}
		}

		const char* renderer_fullname = "";
		const char* renderer_mode = "";

		switch (renderer)
		{
		case GSRendererType::DX1011_SW:
		case GSRendererType::OGL_SW:
			renderer_mode = "(Software renderer)";
			break;
		case GSRendererType::Null:
			renderer_mode = "(Null renderer)";
			break;
#ifdef ENABLE_OPENCL
		case GSRendererType::DX1011_OpenCL:
		case GSRendererType::OGL_OpenCL:
			renderer_mode = "(OpenCL)";
			break;
#endif
		default:
			renderer_mode = "(Hardware renderer)";
			break;
		}

		switch (renderer)
		{
		default:
#ifdef _WIN32
		case GSRendererType::DX1011_HW:
		case GSRendererType::DX1011_SW:
#ifdef ENABLE_OPENCL
		case GSRendererType::DX1011_OpenCL:
#endif
			dev = new GSDevice11();
			s_renderer_name = " D3D11";
			renderer_fullname = "Direct3D 11";
			break;
#endif
		case GSRendererType::Null:
			dev = new GSDeviceNull();
			s_renderer_name = " Null";
			renderer_fullname = "Null";
			break;
		case GSRendererType::OGL_HW:
		case GSRendererType::OGL_SW:
#ifdef ENABLE_OPENCL
		case GSRendererType::OGL_OpenCL:
#endif
			dev = new GSDeviceOGL();
			s_renderer_name = " OGL";
			renderer_fullname = "OpenGL";
			break;
		}

		printf("Current Renderer: %s %s\n", renderer_fullname, renderer_mode);

		if (dev == NULL)
		{
			return -1;
		}

		if (s_gs == NULL)
		{
			switch (renderer)
			{
			default:
#ifdef _WIN32
			case GSRendererType::DX1011_HW:
				s_gs = (GSRenderer*)new GSRendererDX11();
				s_renderer_type = " HW";
				break;
#endif
			case GSRendererType::OGL_HW:
				s_gs = (GSRenderer*)new GSRendererOGL();
				s_renderer_type = " HW";
				break;
			case GSRendererType::DX1011_SW:
			case GSRendererType::OGL_SW:
				s_gs = new GSRendererSW(threads);
				s_renderer_type = " SW";
				break;
			case GSRendererType::Null:
				s_gs = new GSRendererNull();
				s_renderer_type = "";
				break;
#ifdef ENABLE_OPENCL
			case GSRendererType::DX1011_OpenCL:
			case GSRendererType::OGL_OpenCL:
				s_gs = new GSRendererCL();
				s_renderer_type = " OCL";
				break;
#endif
			}
			if (s_gs == NULL)
				return -1;
		}

		s_gs->m_wnd = window;
	}
	catch (std::exception& ex)
	{
		// Allowing std exceptions to escape the scope of the plugin callstack could
		// be problematic, because of differing typeids between DLL and EXE compilations.
		// ('new' could throw std::alloc)

		printf("GSdx error: Exception caught in GSopen: %s", ex.what());

		return -1;
	}

	s_gs->SetRegsMem(s_basemem);
	s_gs->SetIrqCallback(s_irq);
	s_gs->SetVSync(s_vsync);

	if(!old_api)
		s_gs->SetMultithreaded(true);

	if(!s_gs->CreateDevice(dev))
	{
		// This probably means the user has DX11 configured with a video card that is only DX9
		// compliant.  Cound mean drivr issues of some sort also, but to be sure, that's the most
		// common cause of device creation errors. :)  --air

		GSclose();

		return -1;
	}

	if (renderer == GSRendererType::OGL_HW && theApp.GetConfigI("debug_glsl_shader") == 2) {
		printf("GSdx: test OpenGL shader. Please wait...\n\n");
		static_cast<GSDeviceOGL*>(s_gs->m_dev)->SelfShaderTest();
		printf("\nGSdx: test OpenGL shader done. It will now exit\n");
		return -1;
	}
	
	return 0;
}

EXPORT_C_(void) GSosdLog(const char *utf8, uint32 color)
{
	if(s_gs && s_gs->m_dev) s_gs->m_dev->m_osd.Log(utf8);
}

EXPORT_C_(void) GSosdMonitor(const char *key, const char *value, uint32 color)
{
	if(s_gs && s_gs->m_dev) s_gs->m_dev->m_osd.Monitor(key, value);
}

EXPORT_C_(int) GSopen2(void** dsp, uint32 flags)
{
	static bool stored_toggle_state = false;
	bool toggle_state = !!(flags & 4);

	GSRendererType renderer = theApp.GetCurrentRendererType();

	if (renderer != GSRendererType::Undefined && stored_toggle_state != toggle_state)
	{
#ifdef _WIN32
		switch (renderer) {
			// Use alternative renderer (SW if currently using HW renderer, and vice versa, keeping the same API and API version)
			case GSRendererType::DX1011_SW: renderer = GSRendererType::DX1011_HW; break;
			case GSRendererType::DX1011_HW: renderer = GSRendererType::DX1011_SW; break;
			case GSRendererType::OGL_SW: renderer = GSRendererType::OGL_HW; break;
			case GSRendererType::OGL_HW: renderer = GSRendererType::OGL_SW; break;
			default: renderer = GSRendererType::DX1011_SW; break; // If wasn't using one of the above mentioned ones, use best SW renderer.
		}

#endif
#if defined(__unix__)
		switch(renderer) {
			// Use alternative renderer (SW if currently using HW renderer, and vice versa)
		case GSRendererType::OGL_SW: renderer = GSRendererType::OGL_HW; break;
		case GSRendererType::OGL_HW: renderer = GSRendererType::OGL_SW; break;
		default: renderer = GSRendererType::OGL_SW; break; // fallback to OGL SW
		}
#endif
	}
	stored_toggle_state = toggle_state;

	int retval = _GSopen(dsp, "", renderer);

	if (s_gs != NULL)
		s_gs->SetAspectRatio(0);	 // PCSX2 manages the aspect ratios

	gsopen_done = true;

	return retval;
}

EXPORT_C_(int) GSopen(void** dsp, const char* title, int mt)
{
	/*
	if(!XInitThreads()) return -1;

	Display* display = XOpenDisplay(0);

	XCloseDisplay(display);
	*/

	GSRendererType renderer = GSRendererType::Default;

	// Legacy GUI expects to acquire vsync from the configuration files.

	s_vsync = theApp.GetConfigI("vsync");

	if(mt == 2)
	{
		// pcsx2 sent a switch renderer request

#ifdef _WIN32

		renderer = GSRendererType::DX1011_SW;

#endif

		mt = 1;
	}
	else
	{
		// normal init

		renderer = static_cast<GSRendererType>(theApp.GetConfigI("Renderer"));
	}

	*dsp = NULL;

	int retval = _GSopen(dsp, title, renderer);

	if(retval == 0 && s_gs)
	{
		s_gs->SetMultithreaded(!!mt);
	}

	gsopen_done = true;

	return retval;
}

EXPORT_C GSreset()
{
	try
	{
		s_gs->Reset();
	}
	catch (GSDXRecoverableError)
	{
	}
}

EXPORT_C GSgifSoftReset(uint32 mask)
{
	try
	{
		s_gs->SoftReset(mask);
	}
	catch (GSDXRecoverableError)
	{
	}
}

EXPORT_C GSwriteCSR(uint32 csr)
{
	try
	{
		s_gs->WriteCSR(csr);
	}
	catch (GSDXRecoverableError)
	{
	}
}

EXPORT_C GSinitReadFIFO(uint8* mem)
{
	GL_PERF("Init Read FIFO1");
	try
	{
		s_gs->InitReadFIFO(mem, 1);
	}
	catch (GSDXRecoverableError)
	{
	}
	catch (const std::bad_alloc&)
	{
		fprintf(stderr, "GSdx: Memory allocation error\n");
	}
}

EXPORT_C GSreadFIFO(uint8* mem)
{
	try
	{
		s_gs->ReadFIFO(mem, 1);
	}
	catch (GSDXRecoverableError)
	{
	}
	catch (const std::bad_alloc&)
	{
		fprintf(stderr, "GSdx: Memory allocation error\n");
	}
}

EXPORT_C GSinitReadFIFO2(uint8* mem, uint32 size)
{
	GL_PERF("Init Read FIFO2");
	try
	{
		s_gs->InitReadFIFO(mem, size);
	}
	catch (GSDXRecoverableError)
	{
	}
	catch (const std::bad_alloc&)
	{
		fprintf(stderr, "GSdx: Memory allocation error\n");
	}
}

EXPORT_C GSreadFIFO2(uint8* mem, uint32 size)
{
	try
	{
		s_gs->ReadFIFO(mem, size);
	}
	catch (GSDXRecoverableError)
	{
	}
	catch (const std::bad_alloc&)
	{
		fprintf(stderr, "GSdx: Memory allocation error\n");
	}
}

EXPORT_C GSgifTransfer(const uint8* mem, uint32 size)
{
	try
	{
		s_gs->Transfer<3>(mem, size);
	}
	catch (GSDXRecoverableError)
	{
	}
}

EXPORT_C GSgifTransfer1(uint8* mem, uint32 addr)
{
	try
	{
		s_gs->Transfer<0>(const_cast<uint8*>(mem) + addr, (0x4000 - addr) / 16);
	}
	catch (GSDXRecoverableError)
	{
	}
}

EXPORT_C GSgifTransfer2(uint8* mem, uint32 size)
{
	try
	{
		s_gs->Transfer<1>(const_cast<uint8*>(mem), size);
	}
	catch (GSDXRecoverableError)
	{
	}
}

EXPORT_C GSgifTransfer3(uint8* mem, uint32 size)
{
	try
	{
		s_gs->Transfer<2>(const_cast<uint8*>(mem), size);
	}
	catch (GSDXRecoverableError)
	{
	}
}

EXPORT_C GSvsync(int field)
{
	try
	{
#ifdef _WIN32

		if(s_gs->m_wnd->IsManaged())
		{
			MSG msg;

			memset(&msg, 0, sizeof(msg));

			while(msg.message != WM_QUIT && PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}

#endif

		s_gs->VSync(field);
	}
	catch (GSDXRecoverableError)
	{
	}
	catch (const std::bad_alloc&)
	{
		fprintf(stderr, "GSdx: Memory allocation error\n");
	}
}

EXPORT_C_(uint32) GSmakeSnapshot(char* path)
{
	try
	{
		std::string s{path};

		if(!s.empty() && s[s.length() - 1] != DIRECTORY_SEPARATOR)
		{
			s = s + DIRECTORY_SEPARATOR;
		}

		return s_gs->MakeSnapshot(s + "gsdx");
	}
	catch (GSDXRecoverableError)
	{
		return false;
	}
}

EXPORT_C GSkeyEvent(GSKeyEventData* e)
{
	try
	{
		if(gsopen_done)
		{
			s_gs->KeyEvent(e);
		}
	}
	catch (GSDXRecoverableError)
	{
	}
}

EXPORT_C_(int) GSfreeze(int mode, GSFreezeData* data)
{
	try
	{
		if(mode == FREEZE_SAVE)
		{
			return s_gs->Freeze(data, false);
		}
		else if(mode == FREEZE_SIZE)
		{
			return s_gs->Freeze(data, true);
		}
		else if(mode == FREEZE_LOAD)
		{
			return s_gs->Defrost(data);
		}
	}
	catch (GSDXRecoverableError)
	{
	}

	return 0;
}

EXPORT_C GSconfigure()
{
	try
	{
		if(!GSUtil::CheckSSE()) return;

		theApp.Init();

#ifdef _WIN32
		GSDialog::InitCommonControls();
		if(GSSettingsDlg().DoModal() == IDOK)
		{
			// Force a reload of the gs state
			theApp.SetCurrentRendererType(GSRendererType::Undefined);
		}

#else

		if (RunLinuxDialog()) {
			theApp.ReloadConfig();
			// Force a reload of the gs state
			theApp.SetCurrentRendererType(GSRendererType::Undefined);
		}

#endif

	} catch (GSDXRecoverableError)
	{
	}
}

EXPORT_C_(int) GStest()
{
	if(!GSUtil::CheckSSE())
		return -1;

	return 0;
}

EXPORT_C GSabout()
{
}

EXPORT_C GSirqCallback(void (*irq)())
{
	s_irq = irq;

	if(s_gs)
	{
		s_gs->SetIrqCallback(s_irq);
	}
}

void pt(const char* str){
	struct tm *current;
	time_t now;
	
	time(&now);
	current = localtime(&now);

	printf("%02i:%02i:%02i%s", current->tm_hour, current->tm_min, current->tm_sec, str);
}

EXPORT_C_(int) GSsetupRecording(int start, void* data)
{
	if (s_gs == NULL) {
		printf("GSdx: no s_gs for recording\n");
		return 0;
	}
#if defined(__unix__)
	if (!theApp.GetConfigB("capture_enabled")) {
		printf("GSdx: Recording is disabled\n");
		return 0;
	}
#endif

	if(start & 1)
	{
		printf("GSdx: Recording start command\n");
		if (s_gs->BeginCapture()) {
			pt(" - Capture started\n");
		} else {
			pt(" - Capture cancelled\n");
			return 0;
		}
	}
	else
	{
		printf("GSdx: Recording end command\n");
		s_gs->EndCapture();
		pt(" - Capture ended\n");
	}

	return 1;
}

EXPORT_C GSsetGameCRC(uint32 crc, int options)
{
	s_gs->SetGameCRC(crc, options);
}

EXPORT_C GSgetLastTag(uint32* tag)
{
	s_gs->GetLastTag(tag);
}

EXPORT_C GSgetTitleInfo2(char* dest, size_t length)
{
	std::string s{"GSdx"};
	s.append(s_renderer_name).append(s_renderer_type);

	// TODO: this gets called from a different thread concurrently with GSOpen (on linux)
	if (gsopen_done && s_gs != NULL && s_gs->m_GStitleInfoBuffer[0])
	{
		std::lock_guard<std::mutex> lock(s_gs->m_pGSsetTitle_Crit);

		s.append(" | ").append(s_gs->m_GStitleInfoBuffer);

		if(s.size() > length - 1)
		{
			s = s.substr(0, length - 1);
		}
	}

	strcpy(dest, s.c_str());
}

EXPORT_C GSsetFrameSkip(int frameskip)
{
	s_gs->SetFrameSkip(frameskip);
}

EXPORT_C GSsetVsync(int vsync)
{
	s_vsync = vsync;

	if(s_gs)
	{
		s_gs->SetVSync(s_vsync);
	}
}

EXPORT_C GSsetExclusive(int enabled)
{
	s_exclusive = !!enabled;

	if(s_gs)
	{
		s_gs->SetVSync(s_vsync);
	}
}

#ifdef _WIN32

#include <io.h>
#include <fcntl.h>

class Console
{
	HANDLE m_console;
	std::string m_title;

public:
	Console::Console(LPCSTR title, bool open)
		: m_console(NULL)
		, m_title(title)
	{
		if(open) Open();
	}

	Console::~Console()
	{
		Close();
	}

	void Console::Open()
	{
		if(m_console == NULL)
		{
			CONSOLE_SCREEN_BUFFER_INFO csbiInfo;

			AllocConsole();

			SetConsoleTitle(m_title.c_str());

			m_console = GetStdHandle(STD_OUTPUT_HANDLE);

			COORD size;

			size.X = 100;
			size.Y = 300;

			SetConsoleScreenBufferSize(m_console, size);

			GetConsoleScreenBufferInfo(m_console, &csbiInfo);

			SMALL_RECT rect;

			rect = csbiInfo.srWindow;
			rect.Right = rect.Left + 99;
			rect.Bottom = rect.Top + 64;

			SetConsoleWindowInfo(m_console, TRUE, &rect);

			freopen("CONOUT$", "w", stdout);
			freopen("CONOUT$", "w", stderr);

			setvbuf(stdout, nullptr, _IONBF, 0);
			setvbuf(stderr, nullptr, _IONBF, 0);
		}
	}

	void Console::Close()
	{
		if(m_console != NULL)
		{
			FreeConsole();

			m_console = NULL;
		}
	}
};

// HelloWindowsDesktop.cpp
// compile with: /D_UNICODE /DUNICODE /DWIN32 /D_WINDOWS /c

// Global variables

// The main window class name.
static TCHAR szWindowClass[] = _T("DesktopApp");

// The string that appears in the application's title bar.
static TCHAR szTitle[] = _T("Windows Desktop Guided Tour Application");

HINSTANCE hInst;
HWND OtherWindowHWND;
HWND TextureWindowHWND;

// Forward declarations of functions included in this code module:
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK WndProcOtherWindow(HWND, UINT, WPARAM, LPARAM);

int image_count = 0;
FILE* debug_out = NULL;


std::string GetCurrMainScreenFile() {
	char buf[100];
	sprintf(buf, "C:\\Users\\tchan\\Desktop\\pics\\%d.bmp", image_count);
	return std::string(buf);
}

std::string GetCurrTextureFile() {
	char buf[100];
	sprintf(buf, "C:\\Users\\tchan\\Desktop\\pics\\%d.tex.bmp", image_count);
	return std::string(buf);
}

void SaveTexture(GSTexture* tex, const std::string& out_file) {
	if (image_count > 1000) return;
	//((GSTextureOGL*)tex)->SaveBits(out_file.c_str());
	((GSTextureOGL*)tex)->SaveBitmapRGB32(out_file);
}

void SaveMainScreenTexture(GSTexture* tex) {
	SaveTexture(tex, GetCurrMainScreenFile());
}

void SaveTextureTexture(GSTexture* tex) {
	SaveTexture(tex, GetCurrTextureFile());
}

void NextImage() {
	image_count++;
}

void WriteImageBits(std::string out_file_name, int width, int height, void* data) {
	FILE* out_file = fopen(out_file_name.c_str(), "wb");
	assert(width % 32 == 0);
	assert(height % 32 == 0);
	fputc(width / 32, out_file);
	fputc(height / 32, out_file);
	fwrite(data, 1, width * height * 4, out_file);
	fclose(out_file);
}

void ReadPic(std::string in_file_name, int& width, int& height, std::vector<uint8>& data) {
	char buf[100];
	FILE* in_file = fopen(in_file_name.c_str(), "rb");
	width = fgetc(in_file) * 32;
	height = fgetc(in_file) * 32; 
	data.resize(width * height * 4);
	fread(data.data(), 1, data.size(), in_file);
	fclose(in_file);
}

// Initial GS setup and setting of the privileged resisters. These setting are more permanent in that they don't
// change a whole lot so you can set them just once for a simple test program. Also these are set directly in the
// GS memory instead of calling a handler as the general-purpose registers require.
// width: width in pixels of output display
// height: height in pixels of output tdisplay
void GSInitialSetup(int display_width, int display_height) {
	GSreset(); // Approximately what should happen when RESET set to 1 on CSR register

	// CSR register is used to control various status things on the GS.
	GSRegCSR& CSR = s_gs->m_regs->CSR;
	CSR.u64     = 0;    // Zero out.
    CSR.rFIFO	= 1;    // FIFO empty
    CSR.rREV    = 0x1B; // GS Revision
    CSR.rID     = 0x55; // GS ID

	// IMR register is used to mask interrupts on the GS. Just Mask all interrupts.
	GSRegIMR& IMR = s_gs->m_regs->IMR;
	IMR.u64       =  0;     // Zero out.
	IMR.SIGMSK    =  1;     // Mask SIGNAL interrupts
	IMR.FINISHMSK =  1;     // Mask FINISH interrupts
	IMR.HSMSK     =  1;     // Mask Horizontal Sync interrupts
	IMR.VSMSK     =  1;     // Mask Vertical Sync interrupts
	IMR.EDWMSK    =  1;     // Mask Rectangular area write termination interrupts
	IMR.u64      |=  0x600;  // Bits 13 and 14 must be set to 1 always
		
	// SMODE1 is a mysterious register for video mode settings. I believe these values are just obtained by just
	// looking at what the BIOS SetGsCrt syscall sets them to. Look at comments under GSRegSMODE1 struct definition.
	// DO SMODE1 setup for PAL
	GSRegSMODE1& SMODE1 = s_gs->m_regs->SMODE1;
	SMODE1.u64       = 0;     // Zero out.
	SMODE1.CLKSEL    = 1;
	SMODE1.CMOD      = 3;
	SMODE1.EX        = 0;
	SMODE1.GCONT     = 0;
	SMODE1.LC        = 32;
	SMODE1.NVCK      = 1;
	SMODE1.PCK2      = 0;
	SMODE1.PEHS      = 0;
	SMODE1.PEVS      = 0;
	SMODE1.PHS       = 0;
	SMODE1.PRST      = 1;
	SMODE1.PVS       = 0;
	SMODE1.RC        = 4;
	SMODE1.SINT      = 0;
	SMODE1.SLCK      = 0;
	SMODE1.SLCK2     = 1;
	SMODE1.SPML      = 4;
	SMODE1.T1248     = 1;
	SMODE1.VCKSEL    = 1;
	SMODE1.VHP       = 0;
	SMODE1.XPCK      = 0;

	// Video mode initialization - I think games usually initialize this with the SetGsCrt syscall
	GSRegSMODE2& SMODE2 = s_gs->m_regs->SMODE2;
	SMODE2.u64  = 0; // Zero out.
	SMODE2.INT  = 0; // Non-interlaced video
	SMODE2.FFMD = 1; // Frame mode if it was interlaced (which it isn't)
	SMODE2.DPMS = 0; // Set VESA DPMS on. No idea what this is but on is default.
		
	// PMODE has a few other settings for the GS CRTC settings (how the final output is displayed).
	// Taken from the Dreamtime PS2 programming tutorials.
	GSRegPMODE& PMODE = s_gs->m_regs->PMODE;
	PMODE.u64   = 0;    // Zero out.
	PMODE.EN1   = 0;    // ReadCircuit1 OFF 
	PMODE.EN2   = 1;    // ReadCircuit2 ON
	PMODE.CRTMD = 1;    // Must always be 1
	PMODE.MMOD  = 1;    // Use ALP register for Alpha Blending
	PMODE.AMOD  = 1;    // Alpha Value of ReadCircuit2 for output selection
	PMODE.SLBG  = 0;    // Blend Alpha with the output of ReadCircuit2lection
	PMODE.ALP   = 0xFF; // Alpha Value = 1.0

	// DISPFB1/2 sets the area in GS local memory where the frame to be used for the final output will be used.
	// We set only DISPFB because it corresponds to the input of ReadCircuit2 which is the only one being used.
	GSRegDISPFB& DISPFB2 = s_gs->m_regs->DISP[1].DISPFB;
	DISPFB2.u64 = 0;                   // Zero out.
	DISPFB2.FBP = 0;                   // Frame base pointer (units of 2048 words)
	DISPFB2.FBW = display_width / 64;  // Frame width (units of 64 pixels)
	DISPFB2.PSM = PSM_PSMCT32;         // Frame pixel format (32 bit RGBA)
	DISPFB2.DBX = 0;                   // Upper left corner X of rectangular area in frame buffer (in pixels)
	DISPFB2.DBY = 0;                    // Upper left corner Y of rectangular area in frame buffer (in pixels)

	// DISPLAY1/2 has some more setting related to how the rectangular area identified in DISFB1/2 should be displayed
	// on an actual output monitor. Taken from Dreamtime PS2 programming tutorials, setting for PAL 640 x 256.
	// Actually I don't think GSDx uses these values at all since the output setting are just taken from the window that
	// GSDX is rendering to.
	GSRegDISPLAY& DISPLAY2 = s_gs->m_regs->DISP[1].DISPLAY;
	int MAGH = 2560 / display_width;
	DISPLAY2.u64  = 0;
	DISPLAY2.DX   = 656;                      // X value of upper left corner of output display area in VCK units. I don't know why this is 656.
	DISPLAY2.DY   = 36;                       // Y value of upper left corner of output display area in raster units. I don't know why this is 36.
	DISPLAY2.MAGH = MAGH - 1;                 // Horizontal magnification value of output (minus 1). I think should be (2560 / W) - 1 where W is pixel width of frame buffer.
	DISPLAY2.MAGV = 0;                        // Vertical magnification value of output (minus 1). Should always be 0 (for 1x magnification)?
	DISPLAY2.DW   = MAGH * display_width - 1; // Horizontal output width in VCK units (minus 1). So will be (W * (MAGH + 1) - 1) where W is pixel width of frame buffer.
	DISPLAY2.DH   = display_height - 1;       // Vertical output height in pixel (minus 1).

	// Sets the background color for the final output merging.
	GSRegBGCOLOR& BGCOLOR = s_gs->m_regs->BGCOLOR;
	BGCOLOR.u64 = 0; // Zero out
	BGCOLOR.R   = 0;
	BGCOLOR.G   = 0;
	BGCOLOR.B   = 0; // Set black background color
}

// Set the general purpose registers which are the settings for the actual drawing functions.
// These require calling handlers since setting the registers usually have other side effects besides
// just changing the values of the registers. On an actual PS2 these would be se by the game using GIF packets
// sent through a DMA in one of the 3 GS paths.
void GSDrawingSetup(int frame_width, int frame_height) {
	GIFReg reg;

	// Setting for the frame buffer for the drawing commands to draw into.
	// In this simple program we use only set context 1 (FRAME1).
	GIFRegFRAME& FRAME = reg.FRAME;
	FRAME.u64    = 0;                         // Zero out
	FRAME.FBP    = 0;                         // Address of frame buffer (units of 2048 words)
	FRAME.FBW    = frame_width / 64;          // Frame buffer width (units of 64 pixels)
	FRAME.PSM    = PSM_PSMCT32;               // pixel format is 32 bit RGBA
	FRAME.FBMSK  = 0;                         // Do not mask any bits (all bits written to)
	s_gs->GIFRegHandlerFRAME<0>(&reg);        // Update context 1

	// Set the offset between the input coordinates of vertices and the top left corner
	// of the window corresponding to the frame buffer in the coordinate space.
	GIFRegXYOFFSET& XYOFFSET = reg.XYOFFSET;
	XYOFFSET.u64 = 0; // Zero out
	XYOFFSET.OFX = 0;
	XYOFFSET.OFY = 0; // No offset
	s_gs->GIFRegHandlerXYOFFSET<0>(&reg); // Update context 1

	// Set the scissor area for the polygon rasterization. These are set relative to the window defined in XYOFFSET.
	GIFRegSCISSOR& SCISSOR = reg.SCISSOR;
	SCISSOR.u64 =   0;             // Zero out
	SCISSOR.SCAX0 = 0;
	SCISSOR.SCAY0 = 0;
	SCISSOR.SCAX1 = frame_width;
	SCISSOR.SCAY1 = frame_height;  // Set the scissor area to the entire window.
	s_gs->GIFRegHandlerSCISSOR<0>(&reg); // Update context 1
}

// Set some general purpose registers to draw simple polygons to the screen.
void GSDoSprite(int x0, int y0, int x1, int y1, int R, int G, int B) {
	GIFReg reg;

	// Set the type of primitive we are drawing and other settings
	GIFRegPRIM& PRIM = reg.PRIM;
	PRIM.u64 =  0;		   // Zero out
	PRIM.PRIM = GS_SPRITE; // Sprite drawing
	PRIM.IIP  = 0;         // Flat shading
	PRIM.TME  = 0;         // Texture mapping disabled
	PRIM.FGE  = 0;         // Fog disabled
	PRIM.ABE  = 0;		   // ALpha blending disabled
	PRIM.AA1  = 0;		   // Antialiasing off
	PRIM.FST  = 0;		   // Set texture coordinate formate to STQ (not used here)
	PRIM.CTXT = 0;		   // Use context 1
	PRIM.FIX  = 0;		   // 0 is default. Don't know what this is for.
	s_gs->GIFRegHandlerPRIM(&reg);

	// Set the color of the vertex
	GIFRegRGBAQ& RGBAQ = reg.RGBAQ;
	RGBAQ.u64 = 0;     // Zero out
	RGBAQ.R   = R;  // red
	RGBAQ.G   = G;     // green
	RGBAQ.B   = B;     // blue
	RGBAQ.A   = 0x80;  // Alpha not used
	RGBAQ.Q   = 0.0f;  // Q not used
	s_gs->GIFRegHandlerRGBAQ(&reg);

	// Specify the vertex X, Y, coordinates. Format of X, Y is fixed point 12:4;
	GIFRegXYZ& XYZ = reg.XYZ;
	XYZ.u64 = 0;          // Zero out
	XYZ.X   = x0 << 4;
	XYZ.Y   = y0 << 4;
	XYZ.Z   = 0;          // Depth buffering not used
	s_gs->GIFRegHandlerXYZ2<GS_SPRITE, 0, true>(&reg);

	// Specify the vertex X, Y, coordinates. Format of X, Y is fixed point 12:4;
	XYZ.u64 = 0;          // Zero out
	XYZ.X   = x1 << 4;
	XYZ.Y   = y1 << 4;
	XYZ.Z   = 0;          // Depth buffering not used
	s_gs->GIFRegHandlerXYZ2<GS_SPRITE, 0, true>(&reg);
}

// lpszCmdLine:
//   First parameter is the renderer.
//   Second parameter is the gs file to load and run.

EXPORT_C GSReplay(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow)
{
	GSRendererType renderer = GSRendererType::Undefined;

	{
		char* start = lpszCmdLine;
		char* end = NULL;
		long n = strtol(lpszCmdLine, &end, 10);
		if(end > start) {renderer = static_cast<GSRendererType>(n); lpszCmdLine = end;}
	}

	while(*lpszCmdLine == ' ') lpszCmdLine++;

	::SetPriorityClass(::GetCurrentProcess(), HIGH_PRIORITY_CLASS);

	Console console{"GSdx", true};

	const std::string f{lpszCmdLine};
	const bool is_xz = f.size() >= 4 && f.compare(f.size() - 3, 3, ".xz") == 0;

	auto file = is_xz
		? std::unique_ptr<GSDumpFile>{std::make_unique<GSDumpLzma>(lpszCmdLine, nullptr)}
		: std::unique_ptr<GSDumpFile>{std::make_unique<GSDumpRaw>(lpszCmdLine, nullptr)};

	GSinit();

	std::array<uint8, 0x2000> regs;
	GSsetBaseMem(regs.data());

	s_vsync = theApp.GetConfigI("vsync");

	HWND hWnd = nullptr;

	_GSopen((void**)&hWnd, "", renderer);

	GSInitialSetup(640, 480);
	GSDrawingSetup(640, 480);

	s_gs->s_dump = true;
	s_gs->s_save = true;
	s_gs->s_savet = true;
	s_gs->s_savef = true;
	s_gs->s_saven = 0;
	s_gs->s_savel = 100;
	s_gs->m_dump_root = "C:\\Users\\tchan\\Desktop\\GS Dump\\Images\\";

	int x = 0;
	while (1) {
		GSDoSprite(0, 0, 640, 480, 0, 0, 0); // Clear screen
		GSDoSprite((100 + x) % 640, 100, (200 + x) % 640, 200, 0xFF, 0, 0);
		x++;
		GSvsync(s_gs->m_regs->CSR.rFIELD);
		Sleep(100);
	}

	return;

	uint32 crc;
	file->Read(&crc, 4);
	GSsetGameCRC(crc, 0);

	{
		GSFreezeData fd;
		file->Read(&fd.size, 4);
		std::vector<uint8> freeze_data(fd.size);
		fd.data = freeze_data.data();
		file->Read(fd.data, fd.size);
		GSfreeze(FREEZE_LOAD, &fd);
	}

	file->Read(regs.data(), 0x2000);

	GSvsync(1);

	struct Packet {uint8 type, param; uint32 size, addr; std::vector<uint8> buff;};

	auto read_packet = [&file](uint8 type) {
		Packet p;
		p.type = type;

		switch(p.type) {
		case 0:
			file->Read(&p.param, 1);
			file->Read(&p.size, 4);
			switch(p.param) {
			case 0:
				p.buff.resize(0x4000);
				p.addr = 0x4000 - p.size;
				file->Read(&p.buff[p.addr], p.size);
				break;
			case 1:
			case 2:
			case 3:
				p.buff.resize(p.size);
				file->Read(p.buff.data(), p.size);
				break;
			}
			break;
		case 1:
			file->Read(&p.param, 1);
			break;
		case 2:
			file->Read(&p.size, 4);
			break;
		case 3:
			p.buff.resize(0x2000);
			file->Read(p.buff.data(), 0x2000);
			break;
		}

		return p;
	};

	std::list<Packet> packets;
	uint8 type;
	while(file->Read(&type, 1))
		packets.push_back(read_packet(type));

	Sleep(100);

	std::vector<uint8> buff;
	while(IsWindowVisible(hWnd))
	{
		for(auto &p : packets)
		{
			switch(p.type)
			{
			case 0:
				switch(p.param)
				{
				case 0: GSgifTransfer1(p.buff.data(), p.addr); break;
				case 1: GSgifTransfer2(p.buff.data(), p.size / 16); break;
				case 2: GSgifTransfer3(p.buff.data(), p.size / 16); break;
				case 3: GSgifTransfer(p.buff.data(), p.size / 16); break;
				}
				break;
			case 1:
				GSvsync(s_gs->m_regs->CSR.rFIELD);
				break;
			case 2:
				if(buff.size() < p.size) buff.resize(p.size);
				GSreadFIFO2(p.buff.data(), p.size / 16);
				break;
			case 3:
				memcpy(regs.data(), p.buff.data(), 0x2000);
				break;
			}
		}
	}

	Sleep(100);

	GSclose();
	GSshutdown();
}

void MakeBitmapInfoRGB32(int width, int height, BITMAPINFO* bitmap_info) {
	memset(bitmap_info, 0, sizeof(BITMAPINFO));
	BITMAPINFOHEADER& bitmap_info_header = bitmap_info->bmiHeader;
	bitmap_info_header.biSize = sizeof(BITMAPINFOHEADER);
	bitmap_info_header.biWidth = width;
	bitmap_info_header.biHeight = -height;
	bitmap_info_header.biPlanes = 1;
	bitmap_info_header.biBitCount = 32;
	bitmap_info_header.biCompression = BI_RGB;
	bitmap_info_header.biSizeImage = 4 * width * height;
	bitmap_info_header.biXPelsPerMeter = 0;
	bitmap_info_header.biYPelsPerMeter = 0;
	bitmap_info_header.biClrUsed = 0;
	bitmap_info_header.biClrImportant = 0;
}

void MakeBitmapFileHeaderRGB32(const BITMAPINFOHEADER* bitmap_info_header, BITMAPFILEHEADER* bitmap_file_header) {
	memset(bitmap_file_header, 0, sizeof(BITMAPFILEHEADER));

	bitmap_file_header->bfType = 0x4d42; // 0x42 = "B" 0x4d = "M"
	
	// Compute the size of the entire file.
	bitmap_file_header->bfSize = sizeof(BITMAPFILEHEADER) + bitmap_info_header->biSize +
		bitmap_info_header->biSizeImage; 
	bitmap_file_header->bfReserved1 = 0; 
	bitmap_file_header->bfReserved2 = 0; 

	// Compute the offset to the array of color indices.  
	bitmap_file_header->bfOffBits = sizeof(BITMAPFILEHEADER) + bitmap_info_header->biSize;
}

void WriteImageBitsToBitmapRGB32(int width, int height, void* bits, const std::string& out_file_name) {
	BITMAPINFO bitmap_info;
	MakeBitmapInfoRGB32(width, height, &bitmap_info);

	BITMAPINFOHEADER& bitmap_info_header = bitmap_info.bmiHeader;

	BITMAPFILEHEADER bitmap_file_header;
	MakeBitmapFileHeaderRGB32(&bitmap_info_header, &bitmap_file_header);

	FILE* out_file = fopen(out_file_name.c_str(), "wb");
	assert(out_file);

	if (fwrite(&bitmap_file_header, 1, sizeof(BITMAPFILEHEADER), out_file) != sizeof(BITMAPFILEHEADER))
		assert(false);

	if (fwrite(&bitmap_info_header, 1, sizeof(BITMAPINFOHEADER), out_file) != sizeof(BITMAPINFOHEADER))
		assert(false);

	// Bitmaps must be in BGR order but bits is given in RGB order
	std::unique_ptr<uint8> bits_BGR(new uint8[bitmap_info_header.biSizeImage]);
	for (int i = 0; i < bitmap_info_header.biSizeImage / 4; i++) {
		int r = ((uint8*)bits)[4 * i + 0];
		int g = ((uint8*)bits)[4 * i + 1];
		int b = ((uint8*)bits)[4 * i + 2];

		bits_BGR.get()[4 * i + 3] = 0;
		bits_BGR.get()[4 * i + 2] = r;
		bits_BGR.get()[4 * i + 1] = g;
		bits_BGR.get()[4 * i + 0] = b;
	}
	bits = bits_BGR.get();

	if (fwrite(bits, 1, bitmap_info_header.biSizeImage, out_file) != bitmap_info_header.biSizeImage)
		assert(false);

	if (fclose(out_file) != 0)
		assert(false);
}

//void CreateBMPFile(LPCTSTR pszFile, PBITMAPINFO p_bitmap_info, LPBYTE lpBits) { 
//	HANDLE file_handle;                 // file handle  
//	BITMAPFILEHEADER bitmap_file_header;      // bitmap file-header  
//	PBITMAPINFOHEADER p_bitmap_info_header;    // bitmap info-header
//	DWORD dwTmp; 
//
//	p_bitmap_info_header = &p_bitmap_info->bmiHeader; 
//
//	// Create the .BMP file.  
//	file_handle = CreateFile(pszFile, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
//		FILE_ATTRIBUTE_NORMAL, (HANDLE)0); 
//	assert(file_handle != INVALID_HANDLE_VALUE);
//
//	bitmap_file_header.bfType = 0x4d42; // 0x42 = "B" 0x4d = "M"
//	// Compute the size of the entire file.  
//	bitmap_file_header.bfSize = sizeof(BITMAPFILEHEADER) + p_bitmap_info_header->biSize +
//		p_bitmap_info_header->biClrUsed * sizeof(RGBQUAD) + p_bitmap_info_header->biSizeImage; 
//	bitmap_file_header.bfReserved1 = 0; 
//	bitmap_file_header.bfReserved2 = 0; 
//
//	// Compute the offset to the array of color indices.  
//	bitmap_file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + p_bitmap_info_header->biSize +
//		p_bitmap_info_header->biClrUsed * sizeof(RGBQUAD); 
//
//	// Copy the BITMAPFILEHEADER into the .BMP file.  
//	if (!WriteFile(file_handle, &bitmap_file_header, sizeof(BITMAPFILEHEADER), &dwTmp, nullptr))
//		assert(false);
//
//	// Copy the BITMAPINFOHEADER and RGBQUAD array into the file.  
//	if (!WriteFile(file_handle, p_bitmap_info_header,
//		sizeof(BITMAPINFOHEADER) + p_bitmap_info_header->biClrUsed * sizeof(RGBQUAD),
//		&dwTmp, nullptr))
//		assert(false); 
//
//	// Copy the array of color indices into the .BMP file.  
//	if (!WriteFile(file_handle, (LPSTR)lpBits, p_bitmap_info_header->biSizeImage, &dwTmp, nullptr)) 
//		assert(false);
//
//	// Close the .BMP file.  
//	if (!CloseHandle(file_handle)) 
//		assert(false);
//}

void LoadScreen(HWND hWnd, std::string in_file_name) {
	assert(false);

	HDC hdc = GetDC(hWnd);
	std::vector<uint8> data;
	int width, height;
	
	ReadPic(in_file_name, width, height, data);
	for (int i = 0; i < data.size() / 4; i++) { // swap b and r
		int r = data[4 * i];
		int b = data[4 * i + 2];

		data[4 * i + 2] = r;
		data[4 * i] = b;
	}

	BITMAPINFO bi;
	{
		BITMAPINFOHEADER& bih = bi.bmiHeader;
		bih.biSize = sizeof(BITMAPINFOHEADER);
		bih.biWidth = width;
		bih.biHeight = -height;
		bih.biPlanes = 1;
		bih.biBitCount = 32;
		bih.biCompression = BI_RGB;
		bih.biSizeImage = 4 * width * height;
		bih.biXPelsPerMeter = 0;
		bih.biYPelsPerMeter = 0;
		bih.biClrUsed = 0;
		bih.biClrImportant = 0;
	}

	//CreateBMPFile(get_curr_file("bmp").c_str(), &bi, data24.data());

	/*if (image_count == 10) {
		HBITMAP bmp = (HBITMAP)LoadImage(NULL, get_curr_file("bmp").c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
		std::vector<uint8> data_again(width * height * 3);
		GetDIBits(NULL, bmp, 0, height, data_again.data(), &bi, DIB_RGB_COLORS);
		CreateBMPFile(get_curr_file(".2.bmp").c_str(), &bi, data_again.data());
		int i = 10 * 100;
	}*/

	
	//HBITMAP bitmap = CreateCompatibleBitmap(hdc, width, height);
	//SetDIBits(hdc, bitmap, 0, height, data24.data(), &bi, DIB_RGB_COLORS);
	

	//RECT rect;
	//   GetWindowRect(hWnd, &rect);
	//   int destWidth = rect.right - rect.left;
	//   int destHeight = rect.bottom - rect.top;
	StretchDIBits(hdc, 0, 0, width, height, 0, 0, width, height, data.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
	ReleaseDC(hWnd, hdc);
}

void UpdateOtherWindow() {
	LoadScreen(OtherWindowHWND, GetCurrMainScreenFile());
}

void UpdateTextureWindow() {
	LoadScreen(TextureWindowHWND, GetCurrTextureFile());
}

void DoDebugImages() {
	if (s_gs->m_dev->GetCurrent()) {
		SaveTexture(s_gs->m_dev->GetCurrent(), GetCurrMainScreenFile());
		//UpdateOtherWindow();
		if (((GSRendererHW*)s_gs)->m_src && ((GSRendererHW*)s_gs)->m_src->m_texture) {
			SaveTexture(((GSRendererHW*)s_gs)->m_src->m_texture, GetCurrTextureFile());
			//UpdateTextureWindow();
		}
		NextImage();
	}
}

int TextureWindow() {
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style          = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc    = NULL;
	wcex.cbClsExtra     = 0;
	wcex.cbWndExtra     = 0;
	wcex.hInstance      = hInst;
	wcex.hIcon          = LoadIcon(hInst, IDI_APPLICATION);
	wcex.hCursor        = LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName   = NULL;
	wcex.lpszClassName  = "TextureWindow";
	wcex.hIconSm        = LoadIcon(wcex.hInstance, IDI_APPLICATION);

	if (!RegisterClassEx(&wcex)) {
		MessageBox(NULL,
			_T("Call to RegisterClassEx failed!"),
			_T("Windows Desktop Guided Tour"),
			NULL);

		return 1;
	}

	HWND hWnd = CreateWindow(
		szWindowClass,
		"Texture Window",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		640, 448,
		NULL,
		NULL,
		hInst,
		NULL
	);

	if (!hWnd) {
		MessageBox(NULL,
			_T("Call to CreateWindow failed!"),
			_T("Windows Desktop Guided Tour"),
			NULL);
		return 1;
	}


	ShowWindow(hWnd, SW_SHOW);
	TextureWindowHWND = hWnd;
	return 0;
}

int RenderWindow() {
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style          = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc    = WndProcOtherWindow;
	wcex.cbClsExtra     = 0;
	wcex.cbWndExtra     = 0;
	wcex.hInstance      = hInst;
	wcex.hIcon          = LoadIcon(hInst, IDI_APPLICATION);
	wcex.hCursor        = LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName   = NULL;
	wcex.lpszClassName  = "OtherWindow";
	wcex.hIconSm        = LoadIcon(wcex.hInstance, IDI_APPLICATION);

	if (!RegisterClassEx(&wcex)) {
		MessageBox(NULL,
			_T("Call to RegisterClassEx failed!"),
			_T("Windows Desktop Guided Tour"),
			NULL);

		return 1;
	}

	HWND hWnd = CreateWindow(
		szWindowClass,
		"Other Window",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		640, 448,
		NULL,
		NULL,
		hInst,
		NULL
	);

	if (!hWnd) {
		MessageBox(NULL,
			_T("Call to CreateWindow failed!"),
			_T("Windows Desktop Guided Tour"),
			NULL);
		return 1;
	}

	// The parameters to ShowWindow explained:
	// hWnd: the value returned from CreateWindow
	// nCmdShow: the fourth parameter from WinMain
	ShowWindow(hWnd, SW_SHOW);
	//UpdateWindow(hWnd);
	OtherWindowHWND = hWnd;
	return 0;
}

// ok what you should do is manually make a gs packet!! and do a test with sprites to see if overlaps!
void DoGSReplay(HWND hWnd) {
	char args[100];
	sprintf(args, "%d %s", (int)GSRendererType::OGL_HW, " ChoAnikiHighScore.gs");
	GSReplay(hWnd, hInst, args, 0);
}

int CALLBACK WinMain(
   _In_ HINSTANCE hInstance,
   _In_opt_ HINSTANCE hPrevInstance,
   _In_ LPSTR     lpCmdLine,
   _In_ int       nCmdShow) {
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style          = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc    = WndProc;
	wcex.cbClsExtra     = 0;
	wcex.cbWndExtra     = 0;
	wcex.hInstance      = hInstance;
	wcex.hIcon          = LoadIcon(hInstance, IDI_APPLICATION);
	wcex.hCursor        = LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName   = NULL;
	wcex.lpszClassName  = szWindowClass;
	wcex.hIconSm        = LoadIcon(wcex.hInstance, IDI_APPLICATION);

	if (!RegisterClassEx(&wcex))
	{
		MessageBox(NULL,
			_T("Call to RegisterClassEx failed!"),
			_T("Windows Desktop Guided Tour"),
			NULL);

		return 1;
	}

	// Store instance handle in our global variable
	hInst = hInstance;

	// The parameters to CreateWindow explained:
	// szWindowClass: the name of the application
	// szTitle: the text that appears in the title bar
	// WS_OVERLAPPEDWINDOW: the type of window to create
	// CW_USEDEFAULT, CW_USEDEFAULT: initial position (x, y)
	// 500, 100: initial size (width, length)
	// NULL: the parent of this window
	// NULL: this application does not have a menu bar
	// hInstance: the first parameter from WinMain
	// NULL: not used in this application
	HWND hWnd = CreateWindow(
		szWindowClass,
		szTitle,
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		500, 500,
		NULL,
		NULL,
		hInstance,
		NULL
	);

	if (!hWnd) {
		MessageBox(NULL,
			_T("Call to CreateWindow failed!"),
			_T("Windows Desktop Guided Tour"),
			NULL);
		return 1;
	}

	// The parameters to ShowWindow explained:
	// hWnd: the value returned from CreateWindow
	// nCmdShow: the fourth parameter from WinMain
	//ShowWindow(hWnd, nCmdShow);
	//UpdateWindow(hWnd);

	// Main message loop:
	/*MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}*/

	//RenderWindow();
	//TextureWindow();

	debug_out = fopen("out.txt", "w");
	if (!debug_out)
		assert(false);
	DoGSReplay(hWnd);
	return 1;
}

//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
   PAINTSTRUCT ps;
   HDC hdc;
   TCHAR greeting[] = _T("Hello, Windows desktop!");

   switch (message) {
   case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rect;
		int img_width = 500;
		int img_height = 500;
        std::vector<uint8> data(img_width * img_height * 4, 0);
        for (int i = 0; i < data.size() / 4; i++) {
            data[i* 4 + 2] = 255;
        }
        HBITMAP bitmap = CreateCompatibleBitmap(hdc, img_width, img_height);
        BITMAPINFOHEADER bih;
        bih.biSize = sizeof(bih);
        bih.biWidth = img_width;
        bih.biHeight = img_height;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        bih.biSizeImage = 0;
        bih.biXPelsPerMeter = 0;
        bih.biYPelsPerMeter = 0;
        bih.biClrUsed = 0;
        bih.biClrImportant = 0;
        BITMAPINFO bi;
        bi.bmiHeader = bih;
        bi.bmiColors[0].rgbBlue = 0;
        bi.bmiColors[0].rgbGreen = 0;
        bi.bmiColors[0].rgbRed = 0;
        bi.bmiColors[0].rgbReserved = 0;
        SetDIBits(hdc, bitmap, 0, 446, data.data(), &bi, DIB_RGB_COLORS);
        GetWindowRect(hWnd, &rect);
            
        int destWidth = rect.right - rect.left;
        int destHeight = rect.bottom - rect.top;
        StretchDIBits(hdc, 0, 0, 100, 100, 0, 0, img_width, img_height, data.data(), &bi, DIB_RGB_COLORS, SRCCOPY);

        ReleaseDC(hWnd, hdc);
        EndPaint(hWnd, &ps);
      break;
   }
   case WM_DESTROY:
      PostQuitMessage(0);
      break;
   default:
      return DefWindowProc(hWnd, message, wParam, lParam);
      break;
   }

   return 0;
}

LRESULT CALLBACK WndProcOtherWindow(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
   PAINTSTRUCT ps;
   HDC hdc;
   TCHAR greeting[] = _T("Hello, Windows desktop!");

   switch (message) {
   case WM_PAINT: {
      break;
   }
   case WM_DESTROY:
      PostQuitMessage(0);
      break;
   default:
      return DefWindowProc(hWnd, message, wParam, lParam);
      break;
   }

   return 0;
}

EXPORT_C GSBenchmark(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow)
{
	::SetPriorityClass(::GetCurrentProcess(), HIGH_PRIORITY_CLASS);

	Console console("GSdx", true);

	if(1)
	{
		GSLocalMemory* mem = new GSLocalMemory();		

		static struct {int psm; const char* name;} s_format[] =
		{
			{PSM_PSMCT32, "32"},
			{PSM_PSMCT24, "24"},
			{PSM_PSMCT16, "16"},
			{PSM_PSMCT16S, "16S"},
			{PSM_PSMT8, "8"},
			{PSM_PSMT4, "4"},
			{PSM_PSMT8H, "8H"},
			{PSM_PSMT4HL, "4HL"},
			{PSM_PSMT4HH, "4HH"},
			{PSM_PSMZ32, "32Z"},
			{PSM_PSMZ24, "24Z"},
			{PSM_PSMZ16, "16Z"},
			{PSM_PSMZ16S, "16ZS"},
		};

		uint8* ptr = (uint8*)_aligned_malloc(1024 * 1024 * 4, 32);

		for(int i = 0; i < 1024 * 1024 * 4; i++) ptr[i] = (uint8)i;

		//

		for(int tbw = 5; tbw <= 10; tbw++)
		{
			int n = 256 << ((10 - tbw) * 2);

			int w = 1 << tbw;
			int h = 1 << tbw;

			printf("%d x %d\n\n", w, h);

			for(size_t i = 0; i < countof(s_format); i++)
			{
				const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[s_format[i].psm];

				GSLocalMemory::writeImage wi = psm.wi;
				GSLocalMemory::readImage ri = psm.ri;
				GSLocalMemory::readTexture rtx = psm.rtx;
				GSLocalMemory::readTexture rtxP = psm.rtxP;

				GIFRegBITBLTBUF BITBLTBUF;

				BITBLTBUF.SBP = 0;
				BITBLTBUF.SBW = w / 64;
				BITBLTBUF.SPSM = s_format[i].psm;
				BITBLTBUF.DBP = 0;
				BITBLTBUF.DBW = w / 64;
				BITBLTBUF.DPSM = s_format[i].psm;

				GIFRegTRXPOS TRXPOS;

				TRXPOS.SSAX = 0;
				TRXPOS.SSAY = 0;
				TRXPOS.DSAX = 0;
				TRXPOS.DSAY = 0;

				GIFRegTRXREG TRXREG;

				TRXREG.RRW = w;
				TRXREG.RRH = h;

				GSVector4i r(0, 0, w, h);

				GIFRegTEX0 TEX0;

				TEX0.TBP0 = 0;
				TEX0.TBW = w / 64;

				GIFRegTEXA TEXA;

				TEXA.TA0 = 0;
				TEXA.TA1 = 0x80;
				TEXA.AEM = 0;

				int trlen = w * h * psm.trbpp / 8;
				int len = w * h * psm.bpp / 8;

				clock_t start, end;

				printf("[%4s] ", s_format[i].name);

				start = clock();

				for(int j = 0; j < n; j++)
				{
					int x = 0;
					int y = 0;

					(mem->*wi)(x, y, ptr, trlen, BITBLTBUF, TRXPOS, TRXREG);
				}

				end = clock();

				printf("%6d %6d | ", (int)((float)trlen * n / (end - start) / 1000), (int)((float)(w * h) * n / (end - start) / 1000));

				start = clock();

				for(int j = 0; j < n; j++)
				{
					int x = 0;
					int y = 0;

					(mem->*ri)(x, y, ptr, trlen, BITBLTBUF, TRXPOS, TRXREG);
				}

				end = clock();

				printf("%6d %6d | ", (int)((float)trlen * n / (end - start) / 1000), (int)((float)(w * h) * n / (end - start) / 1000));

				const GSOffset* off = mem->GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM);

				start = clock();

				for(int j = 0; j < n; j++)
				{
					(mem->*rtx)(off, r, ptr, w * 4, TEXA);
				}

				end = clock();

				printf("%6d %6d ", (int)((float)len * n / (end - start) / 1000), (int)((float)(w * h) * n / (end - start) / 1000));

				if(psm.pal > 0)
				{
					start = clock();

					for(int j = 0; j < n; j++)
					{
						(mem->*rtxP)(off, r, ptr, w, TEXA);
					}

					end = clock();

					printf("| %6d %6d ", (int)((float)len * n / (end - start) / 1000), (int)((float)(w * h) * n / (end - start) / 1000));
				}

				printf("\n");
			}

			printf("\n");
		}

		_aligned_free(ptr);

		delete mem;
	}

	//

	if(0)
	{
		GSLocalMemory* mem = new GSLocalMemory();

		uint8* ptr = (uint8*)_aligned_malloc(1024 * 1024 * 4, 32);

		for(int i = 0; i < 1024 * 1024 * 4; i++) ptr[i] = (uint8)i;

		const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[PSM_PSMCT32];

		GSLocalMemory::writeImage wi = psm.wi;

		GIFRegBITBLTBUF BITBLTBUF;

		BITBLTBUF.DBP = 0;
		BITBLTBUF.DBW = 32;
		BITBLTBUF.DPSM = PSM_PSMCT32;

		GIFRegTRXPOS TRXPOS;

		TRXPOS.DSAX = 0;
		TRXPOS.DSAY = 1;

		GIFRegTRXREG TRXREG;

		TRXREG.RRW = 256;
		TRXREG.RRH = 256;

		int trlen = 256 * 256 * psm.trbpp / 8;

		int x = 0;
		int y = 0;

		(mem->*wi)(x, y, ptr, trlen, BITBLTBUF, TRXPOS, TRXREG);

		delete mem;
	}

	//

	PostQuitMessage(0);
}

#endif

#if defined(__unix__)

inline unsigned long timeGetTime()
{
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	return (unsigned long)(t.tv_sec*1000 + t.tv_nsec/1000000);
}

// Note
EXPORT_C GSReplay(char* lpszCmdLine, int renderer)
{
	GLLoader::in_replayer = true;
	// Required by multithread driver
	XInitThreads();

	GSinit();

	GSRendererType m_renderer;
	// Allow to easyly switch between SW/HW renderer -> this effectively removes the ability to select the renderer by function args
	m_renderer = static_cast<GSRendererType>(theApp.GetConfigI("Renderer"));

	if (m_renderer != GSRendererType::OGL_HW && m_renderer != GSRendererType::OGL_SW)
	{
		fprintf(stderr, "wrong renderer selected %d\n", static_cast<int>(m_renderer));
		return;
	}

	struct Packet {uint8 type, param; uint32 size, addr; std::vector<uint8> buff;};

	std::list<Packet*> packets;
	std::vector<uint8> buff;
	uint8 regs[0x2000];

	GSsetBaseMem(regs);

	s_vsync = theApp.GetConfigI("vsync");
	int finished = theApp.GetConfigI("linux_replay");
	bool repack_dump = (finished < 0);

	if (theApp.GetConfigI("dump")) {
		fprintf(stderr, "Dump is enabled. Replay will be disabled\n");
		finished = 1;
	}

	long frame_number = 0;

	void* hWnd = NULL;
	int err = _GSopen((void**)&hWnd, "", m_renderer);
	if (err != 0) {
		fprintf(stderr, "Error failed to GSopen\n");
		return;
	}
	if (s_gs->m_wnd == NULL) return;

	{ // Read .gs content
		std::string f(lpszCmdLine);
		bool is_xz = (f.size() >= 4) && (f.compare(f.size()-3, 3, ".xz") == 0);
		if (is_xz)
			f.replace(f.end()-6, f.end(), "_repack.gs");
		else
			f.replace(f.end()-3, f.end(), "_repack.gs");

		GSDumpFile* file = is_xz
			? (GSDumpFile*) new GSDumpLzma(lpszCmdLine, repack_dump ? f.c_str() : nullptr)
			: (GSDumpFile*) new GSDumpRaw(lpszCmdLine, repack_dump ? f.c_str() : nullptr);

		uint32 crc;
		file->Read(&crc, 4);
		GSsetGameCRC(crc, 0);

		GSFreezeData fd;
		file->Read(&fd.size, 4);
		fd.data = new uint8[fd.size];
		file->Read(fd.data, fd.size);

		GSfreeze(FREEZE_LOAD, &fd);
		delete [] fd.data;

		file->Read(regs, 0x2000);

		uint8 type;
		while(file->Read(&type, 1))
		{
			Packet* p = new Packet();

			p->type = type;

			switch(type)
			{
			case 0:
				file->Read(&p->param, 1);
				file->Read(&p->size, 4);

				switch(p->param)
				{
				case 0:
					p->buff.resize(0x4000);
					p->addr = 0x4000 - p->size;
					file->Read(&p->buff[p->addr], p->size);
					break;
				case 1:
				case 2:
				case 3:
					p->buff.resize(p->size);
					file->Read(&p->buff[0], p->size);
					break;
				}

				break;

			case 1:
				file->Read(&p->param, 1);
				frame_number++;

				break;

			case 2:
				file->Read(&p->size, 4);

				break;

			case 3:
				p->buff.resize(0x2000);

				file->Read(&p->buff[0], 0x2000);

				break;
			}

			packets.push_back(p);

			if (repack_dump && frame_number > -finished)
				break;
		}

		delete file;
	}

	sleep(2);


	frame_number = 0;

	// Init vsync stuff
	GSvsync(1);

	while(finished > 0)
	{
		for(auto i = packets.begin(); i != packets.end(); i++)
		{
			Packet* p = *i;

			switch(p->type)
			{
				case 0:

					switch(p->param)
					{
						case 0: GSgifTransfer1(&p->buff[0], p->addr); break;
						case 1: GSgifTransfer2(&p->buff[0], p->size / 16); break;
						case 2: GSgifTransfer3(&p->buff[0], p->size / 16); break;
						case 3: GSgifTransfer(&p->buff[0], p->size / 16); break;
					}

					break;

				case 1:

					GSvsync(s_gs->m_regs->CSR.rFIELD);
					frame_number++;

					break;

				case 2:

					if(buff.size() < p->size) buff.resize(p->size);

					GSreadFIFO2(&buff[0], p->size / 16);

					break;

				case 3:

					memcpy(regs, &p->buff[0], 0x2000);

					break;
			}
		}

		if (finished >= 200) {
			; // Nop for Nvidia Profiler
		} else if (finished > 90) {
			sleep(1);
		} else {
			finished--;
		}
	}

	static_cast<GSDeviceOGL*>(s_gs->m_dev)->GenerateProfilerData();

#ifdef ENABLE_OGL_DEBUG_MEM_BW
	unsigned long total_frame_nb = std::max(1l, frame_number) << 10;
	fprintf(stderr, "memory bandwith. T: %f KB/f. V: %f KB/f. U: %f KB/f\n",
			(float)g_real_texture_upload_byte/(float)total_frame_nb,
			(float)g_vertex_upload_byte/(float)total_frame_nb,
			(float)g_uniform_upload_byte/(float)total_frame_nb
		   );
#endif

	for(auto i = packets.begin(); i != packets.end(); i++)
	{
		delete *i;
	}

	packets.clear();

	sleep(2);

	GSclose();
	GSshutdown();
}
#endif
