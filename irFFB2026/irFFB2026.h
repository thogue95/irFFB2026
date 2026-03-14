/*
Copyright (c) 2016 NLP

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "resource.h"
#include "stdafx.h"
#include "irsdk_defines.h"

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken = '6595b64144ccf1df' language = '*'\"")

#include <atomic>
extern std::atomic<int> activeBuffer;

extern std::atomic<float> oversteerIntensity;
extern std::atomic<float> understeerIntensity;
extern std::atomic<bool> isHighImpact;


#define MAX_FFB_DEVICES 16

#define MAX_FFB_TYPES 4  //using for Sim Hub Data Request
#define MAX_AUTO360_MPH 200.0f
#define MAX_EFFECTS_STRENGTH 100.0f

 
#define DI_MAX 10000
#define IR_MAX 9996
#define IR_RAW_MAX 9996.0f
#define MINFORCE_MULTIPLIER 100
#define MIN_MAXFORCE 5 //used to set range on Max Force Slider
#define MAX_MAXFORCE 100 //used to set range on Max Force Slider
#define MIN_FORCE 10 //hard coding min force on wheels if Direct Drive wheel is not selected.  10 seems to be a good number
#define PARKED_FORCE_REDUCER 4
#define BUMPSFORCE_MULTIPLIER 1.6f
#define LOADFORCE_MULTIPLIER 0.08f
#define LONGLOAD_STDPOWER 4
#define LONGLOAD_MAXPOWER 8
#define STOPS_MAXFORCE_RAD 0.2618f // 15 deg
#define DAMPING_MULTIPLIER 800.0f
#define DAMPING_MULTIPLIER_STOPS 150000.0f
#define MAX_DAMPING 100
#define MAX_BUMPS 100
#define MPH_to_MPS_CONVERSION .44704f
#define MAXCLIPPINGCOUNT 60

#define BUF_FRONT 0
#define BUF_BACK  1


#define DIRECT_INTERP_SAMPLES 6
#define SETTINGS_KEY L"Software\\irFFB2026\\Settings"
#define RUN_ON_STARTUP_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define INI_PATH L"\\irFFB2026.ini"

#define INI_SCAN_FORMAT  "%[^:]:%[^:]:%d:%d:%f:%f:%f:%d:%d"
#define INI_PRINT_FORMAT "%s:%s:%d:%d:%0.1f:%0.1f:%0.1f:%d:%d\r" 
 

#define MAX_CAR_NAME 32
#define MAX_TRACK_NAME 64
#define MAX_LATENCY_TIMES 32
#define LATENCY_MIN_DX 60
#define HID_CLASS_GUID { 0x745a17a0, 0x74d3, 0x11d0, 0xb6, 0xfe, 0x00, 0xa0, 0xc9, 0x0f, 0x57, 0xda };
#define WM_TRAY_ICON WM_USER+1
#define WM_EDIT_VALUE WM_USER+2
#define EDIT_INT 0
#define EDIT_FLOAT 1
#define ID_TRAY_EXIT 40000

#define SVCNAME L"irFFB2026svc"
#define CMDLINE_HGSVC    L"service"
#define CMDLINE_HGINST   L"hgInst"
#define CMDLINE_HGREPAIR L"hgRepair"

#define G25PID  0xc299
#define DFGTPID 0xc29a
#define G27PID  0xc29b

#define G25PATH  L"vid_046d&pid_c299"
#define DFGTPATH L"vid_046d&pid_c29a"
#define G27PATH  L"vid_046d&pid_c29b"

#define LOGI_WHEEL_HID_CMD "\x00\xf8\x81\x84\x03\x00\x00\x00\x00"
#define LOGI_WHEEL_HID_CMD_LEN 9

#define SLEEP_SPIN_MAX_ITERATIONS   2000000     // ~50–200 ms depending on CPU speed
#define SLEEP_SPIN_EMERGENCY_MS     1           // last-resort sleep if everything fails




enum ffbType {
    FFBTYPE_IRFFB_360,
    FFBTYPE_IRFFB_720,
    FFBTYPE_GAME_360,
    FFBTYPE_GAME_720,
    FFBTYPE_UNKNOWN
};
 

typedef struct sWins {
    HWND trackbar;
    HWND label;
    HWND value;
} sWins_t;

struct LogiRpmData {
    float rpm;
    float rpmFirstLed;
    float rpmRedLine;
};

struct LogiLedData {
    DWORD size;
    DWORD version;
    LogiRpmData rpmData;
};

DWORD WINAPI readWheelThread(LPVOID);
DWORD WINAPI directFFBThread(LPVOID);

ATOM MyRegisterClass(HINSTANCE);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

HWND combo(HWND, wchar_t*, int, int);
sWins_t* slider(HWND, wchar_t*, int, int, wchar_t*, wchar_t*, bool);
HWND checkbox(HWND, wchar_t*, int, int);




bool initVJD();
void text(wchar_t*, ...);
void text(wchar_t*, char*);
void debug(wchar_t*);
template <typename... T> void debug(wchar_t*, T...);
void setTrackNameStatus(char*);
void setCarStatus(char*);
void setConnectedStatus(bool);
void setOnTrackStatus(bool);
void enumDirectInput();
void initDirectInput();
void releaseDirectInput();
void reacquireDIDevice();
inline void sleepSpinUntil(PLARGE_INTEGER, UINT, UINT);
inline int scaleTorque(float);
inline void setFFB(int);
void initAll();
void releaseAll();
void updateSimHub();



BOOL CALLBACK EnumFFDevicesCallback(LPCDIDEVICEINSTANCE, VOID*);

// The compiler seems to like branches
inline float minf(float a, float b) {

    __m128 ma = _mm_set_ss(a);
    __m128 mb = _mm_set_ss(b);
    return _mm_cvtss_f32(_mm_min_ss(ma, mb));

}

inline float maxf(float a, float b) {

    __m128 ma = _mm_set_ss(a);
    __m128 mb = _mm_set_ss(b);
    return _mm_cvtss_f32(_mm_max_ss(ma, mb));

}

inline float csignf(float a, float b) {

    float mask = -0.0f;

    __m128 ma = _mm_set_ss(a);
    __m128 mb = _mm_set_ss(b);
    __m128 mm = _mm_set_ss(mask);
    ma = _mm_andnot_ps(mm, ma);
    mb = _mm_and_ps(mb, mm);
    return _mm_cvtss_f32(_mm_or_ps(ma, mb));

}