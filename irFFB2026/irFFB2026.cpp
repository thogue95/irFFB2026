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

Modified by Tom Hogue Apr 2021:
There is a table below with a list of road cars and their associated values for yaw and lateral factors
in calculating the force level for an oversteer condition to occur.  I modified irFFB so that those values
could be set in the gui as Oversteer Force and a Force Multiplier.  

Modified by Tom Hogue Aug 2022 and renamed it irFFB2022
Simplified the controls.  Added AutoFFB360 function.

Modified by Tom Hogue March 2026 and renamed it irFFB2026.
This is a major upgrade to irFFB with the help of @Grok.  The FFB effects have been rewritten and are now based on the Pacejka Magic Formula for 
self aligning torque and veritical effects.  There is also a significant rework to maintain latency  while reducing CPU utilization.  There are also changes to
increase reliablity.  Force reduction on impact has been implemented.  Disabled Game FFB modes when vjoy is not enabled. Simplified UI for ease of use.
There are probably even more changes that I can't remember right now. 


The debug() function is still in place but the UI checkbox is disabled.  Renable for development.
Users were filling up hard drives leaving the debug on all the time and slowing down run time.
*/ 

//

#include "irFFB2026.h"
#include "Settings.h"
#include "shlwapi.h"
#include "public.h"
#include "yaml_parser.h"
#include "vjoyinterface.h"
#include "fstream"
#include <windows.h>
#include <winuser.h>
#include <commctrl.h>
#include <atomic>





#include <mutex>

//for new sleepSpinUntil function
#include <thread>     // For sleep_for
#include <xmmintrin.h> // For _mm_pause (if fallback needed)
#include <immintrin.h>  // For _mm_hadd_ps and other SSE intrinsics
#include <emmintrin.h>  // SSE2 for _mm_loadl_pi

// Define NtDelayExecution type
typedef NTSTATUS(NTAPI* pNtDelayExecution)(BOOLEAN Alertable, PLARGE_INTEGER DelayInterval);

// Global pointer (load once)
pNtDelayExecution NtDelayExecution = nullptr;

using namespace std;



//using namespace std;

#define MAX_LOADSTRING 100

#define STATUS_CONNECTED_PART 0
#define STATUS_ONTRACK_PART 1
#define STATUS_CAR_PART 2
#define STATUS_TRACK_NAME_PART 3



extern HANDLE hDataValidEvent;


// Globals
HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];
NOTIFYICONDATA niData;

HANDLE globalMutex;

HBRUSH g_hWhiteBrush = NULL;


HANDLE debugHnd = INVALID_HANDLE_VALUE;
wchar_t debugLastMsg[512];
LONG debugRepeat = 0;

LPDIRECTINPUT8 pDI = nullptr;
LPDIRECTINPUTDEVICE8 ffdevice = nullptr;
LPDIRECTINPUTEFFECT effect = nullptr;

CRITICAL_SECTION effectCrit;

DIJOYSTATE joyState;
DWORD axes[1] = { DIJOFS_X };
LONG  dir[1]  = { 0 };
DIPERIODIC pforce;  
DIEFFECT   dieff;

LogiLedData logiLedData;
DIEFFESCAPE logiEscape;

Settings settings;

//For the smoothing filters
__declspec(align(16)) float firc6[] = {
    0.1295867f, 0.2311436f, 0.2582509f, 0.1923936f, 0.1156718f, 0.0729534f
};
__declspec(align(16)) float firc12[] = {
    0.0322661f, 0.0696877f, 0.0967984f, 0.1243019f, 0.1317534f, 0.1388793f,
    0.1129315f, 0.0844297f, 0.0699100f, 0.0567884f, 0.0430215f, 0.0392321f
};

char car[MAX_CAR_NAME];
char trackName[MAX_TRACK_NAME];
const int MAX_ST_SAMPLES = 32;

volatile bool wheelAndEffectReady = false;

float* speed = nullptr;
float* latAccel = nullptr, * longAccel = nullptr;

int force = 0;
volatile float suspForce = 0.0f; 


// Double-buffered shared arrays — [0] = front (readers use this), [1] = back (writer fills this)
// Keep __declspec(align(16)) for the ones that had it originally — helps SIMD if you ever vectorize more
__declspec(align(16)) volatile float yawForce[2][DIRECT_INTERP_SAMPLES];
__declspec(align(16)) volatile float ffbForce[2][DIRECT_INTERP_SAMPLES];
__declspec(align(16)) volatile float suspForceST[2][DIRECT_INTERP_SAMPLES];

// Atomic index telling consumers which buffer is currently active/front (0 or 1)

std::atomic<int> activeBuffer(BUF_FRONT);  // Define and initialize here (use BUF_FRONT or 0 as starting value)



bool onTrack = false, stopped = true, deviceChangePending = false, logiWheel = false;

volatile int ffbMag = 0; //holds the FFB level that is used when writing direct to the DirectInput interface
volatile bool nearStops = false;

//using for setFFB
int* trackSurface = nullptr, * currentLap = nullptr, * lapCompleted = nullptr;

std::atomic<float> oversteerIntensity(0.0f);
std::atomic<float> understeerIntensity(0.0f);

float impactThreshold = 10.0f;


std::atomic<bool> isHighImpact(false);


int numButtons = 0, numPov = 0, vjButtons = 0, vjPov = 0;
UINT samples, clippedSamples;


static int lastFfbType = -1;  // invalid at start.  Used to reduce loosing connection to the wheel.


// fix  These create events should be in code to check for an error
HANDLE wheelEvent = CreateEvent(nullptr, false, false, L"WheelEvent");  // original version



HANDLE ffbEvent   = CreateEvent(nullptr, false, false, L"FFBEvent");

HWND mainWnd, textWnd, statusWnd;
HFONT hTipsFont;

LARGE_INTEGER freq;

int vjDev = 1;
FFB_DATA ffbPacket;  

//Simhub Connector variables
HANDLE fileMapHnd, simHubUpdateMapHnd, simHubChangeRequestMapHnd;
PBYTE simHubConnectorBuffer = NULL;


bool simHubInstalled = false;
bool simHubActive = false;
static bool simHubFirstUpdateDone = false;

HANDLE simHubIPC;

bool simHubConnectorStatus = false;

bool vJoyReady = false;


std::string simHubDLL = "C:\\Program Files (x86)\\SimHub\\Toms.irFFB2026Connector.dll";

bool recievedSimHubChangeRequest = false;



//Data being sent to SimHub
struct SIMHUBCONNECTORDATA
{

    int FFBTYPE;
    int MaxForce;
    int Damping;
    int Bumps;
    int FFBEffectsLevel;  
    int iRacingStatus;
    int irFFBStatus;
    int DirectInputStatus;
    int ClippingStatus;
    int OversteerShaker;
    int UndersteerShaker;
    bool AutoTune;


};

//Data being received from SimHub as a change request
struct SIMHUBCHANGEREQUESTDATA
{
    int Tick;
    int FFBTYPE;
    int MaxForce;
    int Damping;
    int Bumps;
    int FFBEffectsLevel;
    bool AutoTune;

};

SIMHUBCONNECTORDATA* simHubUpdate;
SIMHUBCHANGEREQUESTDATA* simHubChangeRequest;

int iracingConnectedStatus = 0;

//using to send to simhub connector
//int incomingFFBValue = 0;  //The FFB value that iracing sent
//int outgoingFFBValue = 0;  //The FFB value that irFFB sends to the wheel

//using Ints instead of bools as it is easier to track when reading in Simhub

int vjoyStatus = 0;
int irFFBStatus = 0;
int directInputStatus = 0;
int clippingStatus = 0; 
int clippingCounter = 0;  //using this to keep from reporting to fast.  Will send to simhub every 1 second by setting limit to 60 passes.



int lastSimHubChangeDataTick = 0;  //used to keep track of data packets received from Simhub Change Requests

const int MAX_TICKS = 1024;



float *floatvarptr(const char *data, const char *name) {
    int idx = irsdk_varNameToIndex(name);
    if (idx >= 0)
        return (float *)(data + irsdk_getVarHeaderEntry(idx)->offset);
    else
        return nullptr;
}

int *intvarptr(const char *data, const char *name) {
    int idx = irsdk_varNameToIndex(name);
    if (idx >= 0)
        return (int *)(data + irsdk_getVarHeaderEntry(idx)->offset);
    else
        return nullptr;
}

bool *boolvarptr(const char *data, const char *name) {
    int idx = irsdk_varNameToIndex(name);
    if (idx >= 0)
        return (bool *)(data + irsdk_getVarHeaderEntry(idx)->offset);
    else
        return nullptr;
}



DWORD WINAPI readWheelThread(LPVOID lParam) {
    UNREFERENCED_PARAMETER(lParam);
    HRESULT res;
    JOYSTICK_POSITION vjData;
    DWORD* hats[] = { &vjData.bHats, &vjData.bHatsEx1, &vjData.bHatsEx2, &vjData.bHatsEx3 };

    // These are now local — no statics outside guard to avoid early init issues
    LONG lastX = 0;
    LARGE_INTEGER lastTime = { 0 }, time, elapsed;
    __declspec(align(16)) float vel[DIRECT_INTERP_SAMPLES] = { 0.0f };
    __declspec(align(16)) float fd[DIRECT_INTERP_SAMPLES];
    int velIdx = 0, vi = 0, fdIdx = 0;
    float d = 0.0f;

  //  debug(L"readWheelThread started – waiting for wheel + effect to be ready");
   

    while (true) {
        // Patient wait until everything is initialized
        if (!wheelAndEffectReady || !ffdevice || !effect) {
            
            Sleep(100);  // 100 ms sleep is fine — low CPU, responsive once ready
            continue;
        }

        // ─── Only reach here once ready ───
        // Safe to reset vJoy now (do it once)
        static bool vjResetDone = false;
        if (!vjResetDone) {
            ResetVJD(vjDev);
            vjResetDone = true;
            //debug(L"vJoy reset complete – polling begins");
        }

        DWORD signaled = WaitForSingleObject(wheelEvent, 1);
        if (signaled != WAIT_OBJECT_0) {
            continue;  // timeout → keep waiting safely
        }

        res = ffdevice->GetDeviceState(sizeof(joyState), &joyState);
        if (res != DI_OK) {
           // debug(L"GetDeviceState returned: 0x%x, requesting reacquire", res);

            text(L"Reacquiring Wheel Device");

            directInputStatus = 0;
            reacquireDIDevice();
            // After reacquire, wheelAndEffectReady should be set again
            continue;
        }
        directInputStatus = 1;

        // ─── Populate vJoy data ───
        vjData.wAxisX = joyState.lX;
        vjData.wAxisY = joyState.lY;
        vjData.wAxisZ = joyState.lZ;
        vjData.wAxisXRot = joyState.lRx;
        vjData.wAxisYRot = joyState.lRy;
        vjData.wAxisZRot = joyState.lRz;

        if (vjButtons > 0) {
            for (int i = 0; i < numButtons; i++) {
                if (joyState.rgbButtons[i])
                    vjData.lButtons |= 1 << i;
                else
                    vjData.lButtons &= ~(1 << i);
            }
        }

        if (vjPov > 0) {
            for (int i = 0; i < numPov && i < 4; i++)
                *hats[i] = joyState.rgdwPOV[i];
        }

        UpdateVJD(vjDev, (PVOID)&vjData);

        // Damping / velocity filtering (only when needed)
        if (settings.getDampingFactor() != 0.0f || nearStops) {
            QueryPerformanceCounter(&time);
            if (lastTime.QuadPart != 0) {
                elapsed.QuadPart = (time.QuadPart - lastTime.QuadPart) * 1000000LL;
                elapsed.QuadPart /= freq.QuadPart;
                vel[velIdx] = (float)(joyState.lX - lastX) / (float)elapsed.QuadPart;
            }
            lastTime.QuadPart = time.QuadPart;
            lastX = joyState.lX;

            vi = velIdx;
            if (++velIdx >= DIRECT_INTERP_SAMPLES)
                velIdx = 0;

            fd[fdIdx] = vel[vi++] * firc6[0];
            for (int i = 1; i < DIRECT_INTERP_SAMPLES; i++) {
                if (vi >= DIRECT_INTERP_SAMPLES)
                    vi = 0;
                fd[fdIdx] += vel[vi++] * firc6[i];
            }

            if (++fdIdx >= DIRECT_INTERP_SAMPLES)
                fdIdx = 0;

            d = ((fd[0] + fd[1]) + (fd[2] + fd[3]) + (fd[4] + fd[5])) / (float)DIRECT_INTERP_SAMPLES;

            if (nearStops)
                d *= DAMPING_MULTIPLIER_STOPS;
            else
                d *= DAMPING_MULTIPLIER * settings.getDampingFactor();
        }
        else {
            d = 0.0f;
        }

        // One-time post-reacquire reset
        static bool firstAfterReacquire = true;
        if (firstAfterReacquire) {
            pforce.lOffset = 0;
            EnterCriticalSection(&effectCrit);
            if (effect) {
                effect->SetParameters(&dieff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
            }
            LeaveCriticalSection(&effectCrit);
            firstAfterReacquire = false;
           // debug(L"Applied first post-reacquire reset to pforce.lOffset");
        }

        

        // ────────────────────────────────────────────────────────────────
        // Read ffbMag safely — add acquire fence for visibility of main-thread writes
        // ────────────────────────────────────────────────────────────────
        std::atomic_thread_fence(std::memory_order_acquire);  // Ensure prior writes are visible
        int currentFfbMag = ffbMag;                           // Local copy — safe now

        // Apply final offset
        pforce.lOffset = currentFfbMag + (int)d;

        // Clamp
        if (pforce.lOffset > DI_MAX)    pforce.lOffset = DI_MAX;
        if (pforce.lOffset < -DI_MAX)   pforce.lOffset = -DI_MAX;

        EnterCriticalSection(&effectCrit);
        if (effect == nullptr) {
            LeaveCriticalSection(&effectCrit);
            continue;
        }

        HRESULT hr = effect->SetParameters(&dieff, DIEP_TYPESPECIFICPARAMS | DIEP_NORESTART);
        if (FAILED(hr)) {
           // debug(L"SetParameters failed: 0x%x - requesting reacquire", hr);
            text(L"Attempting to reaquire the wheel", hr);
            text(L"In iRacing's app.ini file, ensure resetWhenFFBLost = 0");
            directInputStatus = 0;
            reacquireDIDevice();
        }
        else {
            directInputStatus = 1;
        }
        LeaveCriticalSection(&effectCrit);
    }

    return 0;
}

// Direct Mode is the low latency path with vjoy
// 
DWORD WINAPI directFFBThread(LPVOID lParam) {
    UNREFERENCED_PARAMETER(lParam);
    int16_t mag;
    float s;
    alignas(16) float prod[12] = { 0.0f }; // Zero-init for safety
    LARGE_INTEGER start;
    // Spike filter state - persistent across loop iterations
    static float spike_filter_360 = 0.0f;
    static float spike_filter_720 = 0.0f;
    // Tuning values
    const float alpha_360 = 0.40f; // ~14 ms smoothing at 360 Hz
    const float alpha_720 = 0.50f; // ~14 ms equivalent at 720 Hz

   

    while (true) {
        WaitForSingleObject(ffbEvent, INFINITE);

        // Early exit if not in direct mode
        if (settings.getFfbType() != FFBTYPE_GAME_360 &&
            settings.getFfbType() != FFBTYPE_GAME_720) {
            continue;
        }

        if (((ffbPacket.data[0] & 0xF0) >> 4) != vjDev) {
            continue;
        }

        QueryPerformanceCounter(&start);
        mag = (ffbPacket.data[3] << 8) + ffbPacket.data[2];
        force = mag; // signed 16-bit
        s = (float)force;
        //debug(L"DirectThread: iRacing Incoming FFB Force: %d", force);

        float fir_sum = 0.0f;


        // ────────────────────────────────────────────────────────────────
        // DOUBLE-BUFFER: Read the currently active buffer index
        // Use acquire ordering so we see the latest published data from main thread
        // ────────────────────────────────────────────────────────────────
        int readBuf = activeBuffer.load(std::memory_order_acquire);

        // Also ensure visibility of other shared state (isHighImpact, intensities)
        std::atomic_thread_fence(std::memory_order_acquire);



        // 720 Hz branch

        if (settings.getFfbType() == FFBTYPE_GAME_720) {
            std::fill(prod, prod + 12, 0.0f);
            __m128 s_vec = _mm_set1_ps(s);
            for (int grp = 0; grp < 3; grp++) {
                __m128 firc = _mm_load_ps(firc12 + grp * 4);
                __m128 mul = _mm_mul_ps(s_vec, firc);
                _mm_store_ps(prod + grp * 4, mul);
            }
            __m128 sum0 = _mm_load_ps(prod);
            __m128 sum1 = _mm_load_ps(prod + 4);
            __m128 sum2 = _mm_load_ps(prod + 8);
            sum0 = _mm_add_ps(sum0, sum1);
            sum0 = _mm_add_ps(sum0, sum2);
            sum0 = _mm_hadd_ps(sum0, sum0);
            sum0 = _mm_hadd_ps(sum0, sum0);
            fir_sum = _mm_cvtss_f32(sum0);

            // Apply spike filter (720 Hz version)
            spike_filter_720 = alpha_720 * fir_sum + (1.0f - alpha_720) * spike_filter_720;
            float filtered_base = spike_filter_720;
            //debug(L"Direct 720: raw=%d fir_sum=%.1f spike_f=%.1f", force, fir_sum, filtered_base);



            // Precompute all 12 interpolated forces
            __declspec(align(16)) float interpForces[12];
            for (int i = 0; i < 12; i++) {
                int idx = i >> 1;
                bool odd = i & 1;
                float frac = odd ? 0.5f : 0.0f;

                float current = ffbForce[readBuf][idx];

                float next = (idx + 1 < DIRECT_INTERP_SAMPLES)
                    ? ffbForce[readBuf][idx + 1]
                    : current;  // ← safe clamp: repeat last value

                float interp = current + frac * (next - current);

                interpForces[i] = filtered_base + scaleTorque(interp);



            }

            // Ultra-lean timing loop
            for (int i = 0; i < 12; i++) {
                int force = (int)interpForces[i];
                if (isHighImpact) {
                    force = (int)filtered_base;
                   // debug(L"DirectThread: Impact Detected FFB Force Reduced: %d", force);
                    oversteerIntensity = 0;
                    understeerIntensity = 0;

                }
                sleepSpinUntil(&start, 0, 1380 * i);
                setFFB(force);
                //debug(L"DirectThread: GAME_720 Adjusted FFB Force: %d", force);
            }
        }
        else { // FFBTYPE_GAME_360 (360 Hz)
            std::fill(prod, prod + 6, 0.0f);
            __m128 s_vec6 = _mm_set1_ps(s);
            __m128 firc6_0 = _mm_load_ps(firc6);
            __m128 mul0 = _mm_mul_ps(s_vec6, firc6_0);
            _mm_store_ps(prod, mul0);
            __m128 firc6_4 = _mm_loadl_pi(_mm_setzero_ps(), (__m64*)(firc6 + 4));
            __m128 mul1 = _mm_mul_ps(s_vec6, firc6_4);
            _mm_storel_pi((__m64*)(prod + 4), mul1);
            __m128 sum6_0 = _mm_load_ps(prod);
            __m128 sum6_1 = _mm_loadl_pi(_mm_setzero_ps(), (__m64*)(prod + 4));
            sum6_0 = _mm_add_ps(sum6_0, sum6_1);
            sum6_0 = _mm_hadd_ps(sum6_0, sum6_0);
            sum6_0 = _mm_hadd_ps(sum6_0, sum6_0);
            fir_sum = _mm_cvtss_f32(sum6_0);

            // Apply spike filter (360 Hz version)
            spike_filter_360 = alpha_360 * fir_sum + (1.0f - alpha_360) * spike_filter_360;
            float filtered_base = spike_filter_360;
            //debug(L"Direct 360: raw=%d fir_sum=%.1f spike_f=%.1f", force, fir_sum, filtered_base);



            // Precompute all 6 scaled forces
            __declspec(align(16)) float interpForces[6];
            for (int i = 0; i < 6; i++) {

                float interp = ffbForce[readBuf][i];

                interpForces[i] = filtered_base + scaleTorque(interp);


            }

            // Ultra-lean timing loop
            for (int i = 0; i < DIRECT_INTERP_SAMPLES; i++) {
                int force = (int)interpForces[i];
                if (isHighImpact) {
                    force = (int)filtered_base;
                    if (i == 0) {  // Call once per packet
                        oversteerIntensity = 0;
                        understeerIntensity = 0;
                       // if (simHubConnectorStatus == true) updateSimHub();
                    }
                }
                sleepSpinUntil(&start, 2000, 2760 * i);
                setFFB(force);
                //debug(L"DirectThread: DIRECT_360Hz Adjusted FFB Force: %d", force);
            }
        }
    }
    return 0;
}



void resetForces()
{
    suspForce = 0.0f;
    force = 0;

    // Zero both buffers in one go using memset
    memset((void*)suspForceST, 0, sizeof(suspForceST));
    memset((void*)yawForce, 0, sizeof(yawForce));
    memset((void*)ffbForce, 0, sizeof(ffbForce));

    setFFB(0);
}


boolean getTrackName() {

  
    const char* ptr;
    int len = -1;

    trackName[0] = 0;

    // Get track name
    if (!parseYaml(irsdk_getSessionInfoStr(), "WeekendInfo:TrackName:", &ptr, &len))
        return false;

    if (len < 0 || len > sizeof(trackName) - 1)
        return false;

    memcpy(trackName, ptr, len);
    trackName[len] = 0;
  
    return true;

}
boolean getCarName() {

    char buf[64];
    const char *ptr;
    int len = -1, carIdx = -1;

    car[0] = 0;

    // Get car idx
    if (!parseYaml(irsdk_getSessionInfoStr(), "DriverInfo:DriverCarIdx:", &ptr, &len))
        return false;

    if (len < 0 || len > sizeof(buf) - 1)
        return false;
    
    memcpy(buf, ptr, len);
    buf[len] = 0;
    carIdx = atoi(buf);

    // Get car path
    sprintf_s(buf, "DriverInfo:Drivers:CarIdx:{%d}CarPath:", carIdx);
    if (!parseYaml(irsdk_getSessionInfoStr(), buf, &ptr, &len))
        return false;
    if (len < 0 || len > sizeof(car) - 1)
        return false;

    memcpy(car, ptr, len);
    car[len] = 0;

    return true;
      
}

float getCarRedline() {

    char buf[64];
    const char *ptr;
    int len = -1;

    if (parseYaml(irsdk_getSessionInfoStr(), "DriverInfo:DriverCarRedLine:", &ptr, &len)) {

        if (len < 0 || len > sizeof(buf) - 1)
            return 8000.0f;
        
        memcpy(buf, ptr, len);
        buf[len] = 0;
        return strtof(buf, NULL);
    }
    
    return 8000.0f;

}


void clippingReport() {

    float clippedPerCent = samples > 0 ? (float)clippedSamples * 100.0f / samples : 0.0f;
    text(L"%.02f%% of samples were clipped", clippedPerCent);
    //debug(L"%.02f%% of samples were clipped", clippedPerCent);
    if (clippedPerCent > 2.5f)
        text(L"Consider increasing Max Force to reduce clipping");
        //debug(L"Consider increasing max force to reduce clipping");

    samples = clippedSamples = 0;

}

void logiRpmLed(float *rpm, float redline) {
    
    logiLedData.rpmData.rpm = *rpm / (redline * 0.90f);
    logiLedData.rpmData.rpmFirstLed = 0.65f;
    logiLedData.rpmData.rpmRedLine = 1.0f;

    ffdevice->Escape(&logiEscape);

}

void deviceChange() {

    //debug(L"Recieved a Windows Device Change Notification");
    if (!onTrack) {
        text(L"Device Change: Car is off track");
        //debug(L"Device Change: Not on track, processing device change");
        deviceChangePending = false;
        enumDirectInput();
        if (!settings.isFfbDevicePresent())
            text(L"Lost our FFB Device. Releasing Wheel Connection");
            releaseDirectInput();

    }
    else {
        text(L"Car is on track");
        //debug(L"Deferring device change processing while on track");
        deviceChangePending = true;
    }

}

DWORD getDeviceVidPid(LPDIRECTINPUTDEVICE8 dev) {

    DIPROPDWORD dipdw;
    dipdw.diph.dwSize = sizeof(DIPROPDWORD);
    dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    dipdw.diph.dwObj = 0;
    dipdw.diph.dwHow = DIPH_DEVICE;

    if (dev == nullptr)
        return 0;

    if (!SUCCEEDED(dev->GetProperty(DIPROP_VIDPID, &dipdw.diph)))
        return 0;

    return dipdw.dwData;

}
void minimise() {
    //debug(L"Minimizing window");
    ShowWindow(mainWnd, SW_MINIMIZE);
}


std::wstring GetSimHubInstallPath() {
    HKEY hKey;
    wchar_t buffer[512] = { 0 };
    DWORD size = sizeof(buffer);

    // Try per-user first (most common)
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SimHub", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"InstallDir", nullptr, nullptr, (LPBYTE)buffer, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::wstring(buffer);
        }
        RegCloseKey(hKey);
    }

    // Fallback: machine-wide install
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SimHub", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"InstallDir", nullptr, nullptr, (LPBYTE)buffer, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::wstring(buffer);
        }
        RegCloseKey(hKey);
    }

    // Ultimate fallback – very common default paths
    return L"C:\\Program Files\\SimHub";
}



bool initializeSimHubConnector() {
    std::wstring basePath;
    std::wstring dllPath;

    // Step 1: Try to get real path from registry
    basePath = GetSimHubInstallPath();
    if (!basePath.empty()) {
        dllPath = basePath + L"\\Toms.irFFB2026Connector.dll";

        std::ifstream f(dllPath);
        if (f.good()) {
            text(L"SimHub Connector found");
            simHubInstalled = true;
            goto TRY_MAPPING;
        }
        else {
            text(L"Still looking for SimHub Connector");
        }
    }

    // Step 2: Fallback to the two most common default locations
    const wchar_t* defaultPaths[] = {
        L"C:\\Program Files\\SimHub\\Toms.irFFB2026Connector.dll",      // 64-bit default
        L"C:\\Program Files (x86)\\SimHub\\Toms.irFFB2026Connector.dll" // 32-bit legacy
    };

    for (const auto& candidate : defaultPaths) {
        std::ifstream f(candidate);
        if (f.good()) {
            text(L"SimHub connector DLL found");
            dllPath = candidate;
            simHubInstalled = true;
            goto TRY_MAPPING;
        }
    }

    // If we get here → SimHub connector not found anywhere reasonable
    text(L"SimHub connector DLL (Toms.irFFB2026Connector.dll) not found");
    text(L"Checked registry + default 32-bit and 64-bit Program Files locations");
    simHubInstalled = false;
    return false;

TRY_MAPPING:
    // ────────────────────────────────────────────────────────────────
    // Proceed with file mapping (your original logic, just using dllPath)
    // ────────────────────────────────────────────────────────────────

    simHubUpdateMapHnd = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        256,
        L"Local\\FromIrffbFileMap"
    );

    if (simHubUpdateMapHnd == NULL) {
        text(L"SimHub Update Mapping Failed (error %d)", GetLastError());
        debug(L"SimHub Update Mapping Failed (error %d)", GetLastError());
        simHubInstalled = false;
        return false;
    }

    simHubUpdate = (SIMHUBCONNECTORDATA*)MapViewOfFile(
        simHubUpdateMapHnd,
        FILE_MAP_ALL_ACCESS,
        0, 0, 256
    );

    if (simHubUpdate == nullptr) {
        text(L"SimHub Update Failed - MapViewOfFile error %d", GetLastError());
        debug(L"SimHub Update Failed - MapViewOfFile error %d", GetLastError());
        CloseHandle(simHubUpdateMapHnd);
        simHubUpdateMapHnd = NULL;
        simHubInstalled = false;
        return false;
    }

    text(L"SimHub connector initialized successfully");
    debug(L"SimHub connector initialized successfully");
    simHubInstalled = true;
    return true;
}




void updateSimHub() {

    //This is where we send data to SimHub

    if (simHubUpdate == nullptr) return;

//  Keep for debug
   // text(L"Writing Simhub Connector Update");
   //text(L"FFBTYPE = %d", settings.getFfbType());
  //  text(L"MaxForce = %d\r", settings.getMaxForce());
  //  (text(L"FFBEffects = %.1f", (int)settings.getFFBEffectsLevel()));


    //put in iRacingStatus

    static DWORD lastUpdate = 0;
    if (GetTickCount() - lastUpdate < 200) return;  // max 10 calls per second
    lastUpdate = GetTickCount();

    
    simHubUpdate->FFBTYPE = settings.getFfbType();
    simHubUpdate->MaxForce = settings.getMaxForce();
    simHubUpdate->FFBEffectsLevel = (int)settings.getFFBEffectsLevel();
    simHubUpdate->iRacingStatus = iracingConnectedStatus;
    simHubUpdate->irFFBStatus = irFFBStatus;
    simHubUpdate->DirectInputStatus = directInputStatus;
    simHubUpdate->Damping = (int)settings.getDampingFactor();
    simHubUpdate->Bumps = (int)settings.getBumpsFactor();
    simHubUpdate->ClippingStatus = clippingStatus;
    simHubUpdate->OversteerShaker = (int)oversteerIntensity;
    simHubUpdate->UndersteerShaker = (int)understeerIntensity;
    simHubUpdate->AutoTune = settings.getAutoTune();


     
    return;


}

void checkSimHubrequest() {

    //text(L"In receiveSimHubrequest");
    mutex mutexLock;

    mutexLock.lock();

    simHubChangeRequestMapHnd = OpenFileMapping(FILE_MAP_READ, FALSE, L"Local\\ToIrffbFileMap");



    if (simHubChangeRequestMapHnd == NULL) {
       // text(L"Simhub Change Request: Mapping Failed");

       // text(L"Simhub Change Request Mapping Failed with Error - %s", GetLastError());
       // debug(L"Simhub Change Request Mapping Failed with Error - %s", GetLastError());
       
        recievedSimHubChangeRequest = false;
        mutexLock.unlock();
        return;
    }

    if ((simHubChangeRequest = (SIMHUBCHANGEREQUESTDATA*)MapViewOfFile(simHubChangeRequestMapHnd, FILE_MAP_READ, 0, 0, 256)) == 0) {

        text(L"Simhub Connector Failed Initialization - Map View Failed");
        recievedSimHubChangeRequest = false;
        mutexLock.unlock();
        return;

    }
       

    if (lastSimHubChangeDataTick != simHubChangeRequest->Tick) {
      // keep for debug
      //  text(L"Reading Simhub Connector Update");
      //  text(L"lastSimHubDataTick = %d", lastSimHubChangeDataTick);
      //  text(L"Tick = %d", simHubChangeRequest->Tick);
      //  text(L"MaxForce = %d", simHubChangeRequest->MaxForce);
      //  text(L"Damping = %d", (int)settings.getDampingFactor());
      //  text(L"Bumps = %d", (int)settings.getBumpsFactor());
      //  text(L"FFB Effects Level = %d", (int)settings.getFFBEffectsLevel());


        //We are getting a +1 or a -1 from SimHub.  

        lastSimHubChangeDataTick = simHubChangeRequest->Tick;
        if (lastSimHubChangeDataTick > +MAX_TICKS) lastSimHubChangeDataTick = 0;
        recievedSimHubChangeRequest = true;

        if (simHubChangeRequest->FFBTYPE != 0) settings.bumpFFBType(simHubChangeRequest->FFBTYPE);

        if (simHubChangeRequest->MaxForce != 0)  settings.bumpMaxForce(simHubChangeRequest->MaxForce);

        if (simHubChangeRequest->FFBEffectsLevel != 0) settings.bumpFFBEffectsLevel(simHubChangeRequest->FFBEffectsLevel);

        if (simHubChangeRequest->Damping != 0) settings.bumpDamping(simHubChangeRequest->Damping);

        if (simHubChangeRequest->Bumps != 0)  settings.bumpBumps(simHubChangeRequest->Bumps);

        if (simHubChangeRequest->AutoTune != 0) settings.setAutoTune(!settings.getAutoTune()); //just toggling autoTune here.
       

    } 



    //move this to shutdown of irffb
    if (UnmapViewOfFile(simHubChangeRequest) == FALSE) {
        mutexLock.unlock();
        text(L"Sim Hub Change Request Failed Unmapping of View ");
        return;
    }

    mutexLock.unlock();
    if (simHubConnectorStatus == true) updateSimHub();

    return;


}


void restore() {
    debug(L"Restoring window");
    Shell_NotifyIcon(NIM_DELETE, &niData);
    ShowWindow(mainWnd, SW_SHOW);
    BringWindowToTop(mainWnd);
}

// Sets up new sleepSpinUntil function
// One-time init (call in main/init function)
void initNtDelay() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        NtDelayExecution = (pNtDelayExecution)GetProcAddress(ntdll, "NtDelayExecution");
    }
}

int APIENTRY wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow
) {

    UNREFERENCED_PARAMETER(hPrevInstance);

    //Fix - source for an unexplained crash

    globalMutex = CreateMutex(NULL, false, L"Global\\irFFB_Mutex");

    if (GetLastError() == ERROR_ALREADY_EXISTS)
        exit(0);

    INITCOMMONCONTROLSEX ccEx;

    HANDLE handles[1];
    char *data = nullptr;
    bool irConnected = false;
    MSG msg;

    

    float *swTorque = nullptr, *swTorqueST = nullptr, *steer = nullptr, *steerMax = nullptr;
    float  *throttle = nullptr, *rpm = nullptr;
    float *LFshockDeflST = nullptr, *RFshockDeflST = nullptr, *CFshockDeflST = nullptr;
    float *LRshockDeflST = nullptr, *RRshockDeflST = nullptr;
    float *vel_LF = nullptr, *vel_RF = nullptr, *vel_LR = nullptr, *vel_RR = nullptr;
    float *vX = nullptr, *vY = nullptr;
    float *yawRate = nullptr;
    float LFshockDeflLast = -10000, RFshockDeflLast = -10000, CFshockDeflLast = -10000;
    float LRshockDeflLast = -10000, RRshockDeflLast = -10000;
    bool* isOnTrack = nullptr;
    int * gear = nullptr;



    bool inGarage = false;
    int numHandles = 0, dataLen = 0;
    int STnumSamples = 0, STmaxIdx = 0, lastTrackSurface = -1;
    float halfSteerMax = 0, lastTorque = 0, lastSuspForce = 0, redline;
    float yaw = 0.0f, yawFilter[DIRECT_INTERP_SAMPLES];


    // NEW: Normalize to 0–100 for SimHub shaker
    const float MAX_EFFECT_FORCE = 0.8f;   // tune this — roughly max realistic yaw addition before clamping


 

    

    ccEx.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    ccEx.dwSize = sizeof(ccEx);
    InitCommonControlsEx(&ccEx);

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_IRFFB));

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_IRFFB, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Setup DI FFB effect
    pforce.dwMagnitude = 0;
    pforce.dwPeriod = INFINITE;
    pforce.dwPhase = 0;
    pforce.lOffset = 0;

    ZeroMemory(&dieff, sizeof(dieff));
    dieff.dwSize = sizeof(DIEFFECT);
    dieff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    dieff.dwDuration = INFINITE;
    dieff.dwSamplePeriod = 0;
    dieff.dwGain = DI_FFNOMINALMAX;
    dieff.dwTriggerButton = DIEB_NOTRIGGER;
    dieff.dwTriggerRepeatInterval = 0;
    dieff.cAxes = 1;
    dieff.rgdwAxes = axes;
    dieff.rglDirection = dir;
    dieff.lpEnvelope = NULL;
    dieff.cbTypeSpecificParams = sizeof(DIPERIODIC);
    dieff.lpvTypeSpecificParams = &pforce;
    dieff.dwStartDelay = 0;

    ZeroMemory(&logiLedData, sizeof(logiLedData));
    logiLedData.size = sizeof(logiLedData);
    logiLedData.version = 1;
    ZeroMemory(&logiEscape, sizeof(logiEscape));
    logiEscape.dwSize = sizeof(DIEFFESCAPE);
    logiEscape.dwCommand = 0;
    logiEscape.lpvInBuffer = &logiLedData;
    logiEscape.cbInBuffer = sizeof(logiLedData);

  

    InitializeCriticalSection(&effectCrit);

    if (!InitInstance(hInstance, nCmdShow))
        return FALSE;

    memset(car, 0, sizeof(car));
    memset(trackName, 0, sizeof(trackName));
    setCarStatus(car);
    setTrackNameStatus(trackName); 
    setConnectedStatus(false);
    setOnTrackStatus(false);
    settings.readGenericSettings();
    settings.readRegSettings(car, trackName);




    if (settings.getStartMinimised())
        minimise();
    else
        restore();

    //Initializing new sleepSpinUntil
    initNtDelay();


    enumDirectInput();


    LARGE_INTEGER start;
    QueryPerformanceFrequency(&freq);

    memset((void*)yawForce, 0, sizeof(yawForce));
    memset((void*)ffbForce, 0, sizeof(ffbForce));
    memset((void*)suspForceST, 0, sizeof(suspForceST));
    activeBuffer.store(BUF_FRONT, std::memory_order_relaxed);
    resetForces();


    initVJD();
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);



    HANDLE hDirectFFB = CreateThread(NULL, 0, directFFBThread, NULL, 0, NULL);
    SetThreadPriority(hDirectFFB, THREAD_PRIORITY_HIGHEST);


    //Simhub connector initialization
    
    simHubConnectorStatus = initializeSimHubConnector();
    
    if (simHubConnectorStatus == true) {

        text(L"Sim Hub Connector is Active");
        debug(L"Sim Hub Connector is Active");

       // updateSimHub();
    }
    else {

        text(L"Sim Hub Connector Status is NOT active");
    }



    // Create readWheelThread ONLY after basic init
    HANDLE hReadWheel = NULL;


    while (TRUE) {


        DWORD res;
        const irsdk_header *hdr = NULL;

        // 1. Attempt DI init if not ready yet (retry if failed)
        if (!wheelAndEffectReady) {

            initDirectInput();  // safe to call multiple times — early returns if already good
        }


        if (!hReadWheel && wheelAndEffectReady) {
            hReadWheel = CreateThread(NULL, 0, readWheelThread, NULL, 0, NULL);
            if (hReadWheel) {
                SetThreadPriority(hReadWheel, THREAD_PRIORITY_HIGHEST);
               // text(L"Started readWheelThread after successful DI init");
                
            }
            else {
                text(L"Failed to create readWheelThread");
                
            }
        }

        if (
            irsdk_startup() && (hdr = irsdk_getHeader()) &&
            hdr->status & irsdk_stConnected && hdr->bufLen != dataLen && hdr->bufLen != 0
        ) {

            text(L"New iRacing session");

            handles[0] = hDataValidEvent;
            numHandles = 1;

            if (data != NULL)
                free(data);

            dataLen = irsdk_getHeader()->bufLen;
            data = (char *)malloc(dataLen);
            setConnectedStatus(true);


            if (getCarName() && getTrackName()) {
                setCarStatus(car);
                setTrackNameStatus(trackName);
                settings.readSettingsForCar(car, trackName);
                if (simHubConnectorStatus == true) updateSimHub();
            }
            else {
                setCarStatus(nullptr);
                setTrackNameStatus(nullptr);
            }



 
            if (simHubConnectorStatus == true && !simHubFirstUpdateDone) {
                updateSimHub();
                simHubFirstUpdateDone = true;
            }
    
            redline = getCarRedline();
             //debug(L"Redline is %d rpm", (int)redline);
            
 

            // Inform iRacing of the maxForce setting
            irsdk_broadcastMsg(irsdk_BroadcastFFBCommand, irsdk_FFBCommand_MaxForce, (float)settings.getMaxForce());



            swTorque = floatvarptr(data, "SteeringWheelTorque");
            swTorqueST = floatvarptr(data, "SteeringWheelTorque_ST");
            steer = floatvarptr(data, "SteeringWheelAngle");
            steerMax = floatvarptr(data, "SteeringWheelAngleMax");
            speed = floatvarptr(data, "Speed");
            throttle = floatvarptr(data, "Throttle");
            rpm = floatvarptr(data, "RPM");
            gear = intvarptr(data, "Gear");
            isOnTrack = boolvarptr(data, "IsOnTrack");

            trackSurface = intvarptr(data, "PlayerTrackSurface");
            lapCompleted = intvarptr(data, "LapCompleted");

            vX = floatvarptr(data, "VelocityX");
            vY = floatvarptr(data, "VelocityY");

            latAccel = floatvarptr(data, "LatAccel");
            longAccel = floatvarptr(data, "LongAccel");

            yawRate = floatvarptr(data, "YawRate");

            RFshockDeflST = floatvarptr(data, "RFshockDefl_ST");
            LFshockDeflST = floatvarptr(data, "LFshockDefl_ST");
            LRshockDeflST = floatvarptr(data, "LRshockDefl_ST");
            RRshockDeflST = floatvarptr(data, "RRshockDefl_ST");
            CFshockDeflST = floatvarptr(data, "CFshockDefl_ST");

            vel_LF = floatvarptr(data, "LFshockVel_ST");
            vel_RF = floatvarptr(data, "RFshockVel_ST");
            vel_LR = floatvarptr(data, "LRshockVel_ST");
            vel_RR = floatvarptr(data, "RRshockVel_ST");


            int swTorqueSTidx = irsdk_varNameToIndex("SteeringWheelTorque_ST");
            STnumSamples = irsdk_getVarHeaderEntry(swTorqueSTidx)->count;
            STmaxIdx = STnumSamples - 1;

            lastTorque = 0.0f;
            onTrack = false;
            resetForces();
            irConnected = true;
            timeBeginPeriod(1);


        }
       

       //end if irsdk_startup
        
        res = MsgWaitForMultipleObjects(numHandles, handles, FALSE, 1000, QS_ALLINPUT);

        QueryPerformanceCounter(&start);
        int ffbType = settings.getFfbType();
        if (ffbType != lastFfbType) {
           // debug(L"FFB mode changed: %d → %d → restarting DI effect", lastFfbType, ffbType);
            

            lastFfbType = ffbType;

            // Force restart of the effect
            EnterCriticalSection(&effectCrit);
            

            if (effect) {
                effect->Stop();
                // Optional: small delay to let it settle
                Sleep(10);
                HRESULT hr = effect->SetParameters(&dieff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
                if (FAILED(hr)) {
                    debug(L"Failed to restart effect on mode change: 0x%x", hr);
                    
                    // Fallback: recreate if needed
                    releaseDirectInput();
                    initDirectInput();  // will recreate effect
                }
            }
            LeaveCriticalSection(&effectCrit);


            // Also reset internal states
            resetForces();

            ffbMag = 0;
            pforce.lOffset = 0;

        }



        if (numHandles > 0 && res == numHandles - 1 && irsdk_getNewData(data)) {
            

            if (onTrack && !*isOnTrack) {
                debug(L"No longer on track");
                onTrack = false;
                setOnTrackStatus(onTrack);
                lastTorque = lastSuspForce = 0.0f;
                oversteerIntensity = 0;
                understeerIntensity = 0;
                resetForces();
                clippingReport();
            }

            else if (!onTrack && *isOnTrack) {
                debug(L"----------------------");
                debug(L"FFB Mode is %d", ffbType);              
                debug(L"Now on track");
                debug(L"Track surface is now: %d", *trackSurface);
                debug(L"Max Force is %d", settings.getMaxForce());
                debug(L"Bumps is %.01f%", settings.getBumpsFactor());
                debug(L"FFB Effects Level is %.01f%", settings.getFFBEffectsLevel());

  

                settings.bumpMaxForce(1);
                settings.bumpMaxForce(-1);

                

                onTrack = true;
                setOnTrackStatus(onTrack);
                RFshockDeflLast = LFshockDeflLast = 
                    LRshockDeflLast = RRshockDeflLast = 
                        CFshockDeflLast = -10000.0f;

                clippedSamples = samples = 0;
                memset(yawFilter, 0, DIRECT_INTERP_SAMPLES * sizeof(float));


            }

            if (simHubConnectorStatus == true) {
                checkSimHubrequest();
                
                if (recievedSimHubChangeRequest == true) {
                   // text(L"Received Simhub Change Request");
                    recievedSimHubChangeRequest = false;  //resetting flag

                    //Increment or Decrement the controls per data recieved from SimHub

                }
            }




            if (*trackSurface != lastTrackSurface) {
                debug(L"Track surface is now: %d", *trackSurface);
                lastTrackSurface = *trackSurface;
            }



            if (ffdevice && logiWheel)
                logiRpmLed(rpm, redline);


            

            yaw = 0.0f;


            // NEW: Precompute scaled forces as early as possible (only once per frame)
            __declspec(align(16)) float scaledForces[MAX_ST_SAMPLES];
    // ────────────────────────────────────────────────────────────────
    // DOUBLE-BUFFER: Choose the buffer to WRITE to (the one NOT currently active)
    // ────────────────────────────────────────────────────────────────
            int writeBuf = 1 - activeBuffer.load(std::memory_order_relaxed);



            if (latAccel != nullptr && longAccel != nullptr) {
                float latG = fabsf(*latAccel) / 9.81f;
                float longG = fabsf(*longAccel) / 9.81f;
                float resultantG = sqrtf((latG * latG) + (longG * longG));
                isHighImpact = (resultantG > impactThreshold);


               // text(L"impactThreshold = %.1f resultantG = %.1f", impactThreshold, resultantG);
            }

            //debug(L"Mainthread before speed > 5: impactThreshold = %.1f", impactThreshold);

            if (*speed > 2.0f) {

                float bumpsFactor = settings.getBumpsFactor();
                float sopFactor = (float)settings.getFFBEffectsLevel();


                const float overSteerOffset = 0.030f;  // ~2 degrees radians (early oversteer)
                const float underSteerOffset = 0.040f;  // ~3 degrees radians (delayed understeer)


                const float Q_DZ2 = -0.2f;  // Peak SAT reduction with load (negative: digressive)
                const float FZ_NOM = 3000.0f;  // Nominal wheel load (N); tune per car class if needed
                const float CZ_BASE = 2000.0f; // Base vertical damping (Ns/m); from MF vertical models

                
          


                // Calculate yaw value
                // === Pacejka SoP (Over + Under) v2.0 — Both Pacejka SAT ===
                if (*speed > 5.0f) {
                   // debug(L"Main: Speed is > 5.0\r");
                  // text(L"Main: Starting FFB Force: %d", force);
                    float dfz_front = 0.0f;
                    float dfz_rear = 0.0f;

                    // Pacejka Vertical Model for Bumps
                    float bumpsLevel = (settings.getBumpsFactor() / 100.0f)* BUMPSFORCE_MULTIPLIER;;  // Already 0.0-1.0 from setter


                    if (isHighImpact) {
                        bumpsLevel = 0.0f;  // Now prevents delta_fz amplification during impact
                    }

                    if (bumpsLevel > 0.0f && vel_LF != nullptr && vel_RF != nullptr && vel_LR != nullptr && vel_RR != nullptr) {
                        // Get shock velocities from the latest sample (STmaxIdx is the last index in the _ST array)
                        float current_vel_LF = vel_LF[STmaxIdx];
                        float current_vel_RF = vel_RF[STmaxIdx];
                        float current_vel_LR = vel_LR[STmaxIdx];
                        float current_vel_RR = vel_RR[STmaxIdx];

                        // EMA filter velocities (α=0.030f like your r filter)
                        static float filtered_vel_front = 0.0f;
                        static float filtered_vel_rear = 0.0f;
                        static bool first_bump = true;
                        if (first_bump) {
                            filtered_vel_front = (current_vel_LF + current_vel_RF) / 2.0f;
                            filtered_vel_rear = (current_vel_LR + current_vel_RR) / 2.0f;
                            first_bump = false;
                        }
                        else {
                            float avg_front = (current_vel_LF + current_vel_RF) / 2.0f;
                            float avg_rear = (current_vel_LR + current_vel_RR) / 2.0f;
                            filtered_vel_front = 0.030f * avg_front + 0.970f * filtered_vel_front;
                            filtered_vel_rear = 0.030f * avg_rear + 0.970f * filtered_vel_rear;
                        }

                        // Compute ΔFz per axle (damping term dominates bumps; scale with bumpsLevel)
                        // Sign: Assume positive vel = compression (increased load); invert if SDK opposite
                        float delta_fz_front = CZ_BASE * filtered_vel_front * bumpsLevel;
                        float delta_fz_rear = CZ_BASE * filtered_vel_rear * bumpsLevel;

                        // NEW: Cap / attenuate when velocities are extreme (prevents Gen 4 blow-up)
                        const float VEL_THRESHOLD = 2.5f;      // m/s — tune: ~2.0–3.0; where Gen 4 starts spiking
                        const float ATTENUATION_RATE = 0.45f;  // how aggressively to reduce (0.3–0.6 range)

                        float max_vel = fmaxf(fabsf(filtered_vel_front), fabsf(filtered_vel_rear));
                        if (max_vel > VEL_THRESHOLD) {
                            float excess = max_vel - VEL_THRESHOLD;
                            float attenuation = 1.0f / (1.0f + excess * ATTENUATION_RATE);

                            // Apply to both axles (or separately if you want front/rear bias)
                            delta_fz_front *= attenuation;
                            delta_fz_rear *= attenuation;


                        }




                        // dfz per wheel (axle avg ~ per wheel for simplicity)
                        dfz_front = delta_fz_front / FZ_NOM;
                        dfz_rear = delta_fz_rear / FZ_NOM;

                        // ────────────────────────────────────────────────────────────────
                         // WRITE the bump/suspension force contribution back to the buffer
                        // ────────────────────────────────────────────────────────────────
                        float bumpForce = (delta_fz_front + delta_fz_rear) * LOADFORCE_MULTIPLIER;
                        // ^ tune LOADFORCE_MULTIPLIER if needed — converts ΔFz → steering torque contribution

                        for (int i = 0; i < DIRECT_INTERP_SAMPLES; i++) {
                            suspForceST[writeBuf][i] = bumpForce;
                           
                        }
                    }
                    else
                    {
                        // No bumps → clear / zero the buffer entry
                        for (int i = 0; i < DIRECT_INTERP_SAMPLES; i++) {
                            suspForceST[writeBuf][i] = 0.0f;
                        }
                    }


                    float halfMaxForce = settings.cachedHalfMaxForce;

                    // Raw rear sideslip (r)
                    float r_raw = *vY / *vX;
                    if (*vX < 0.0f) r_raw = -r_raw;

                    // Shared EMA filter (α=0.030)
                    static float filtered_r = 0.0f;
                    static bool first_frame = true;
                    if (first_frame) {
                        filtered_r = r_raw;
                        first_frame = false;
                    }
                    else {
                        filtered_r = 0.030f * r_raw + 0.970f * filtered_r;
                    }
                    float r = filtered_r;
                    float ar = fabsf(r);
                   
                    float effectiveOverSteerFactor = settings.cachedOverSteerFactor;
                    float underSteerFactor = settings.cachedUnderSteerFactor;

                    // === Oversteer (Rear SAT Pacejka) ===
                    const float B_o = 2.5f, C_o = 1.85f, D_o_base = 0.85f, E_o = -0.88f;
                    float D_o = D_o_base * (1.0f + Q_DZ2 * dfz_rear);  // Modulate peak with rear dfz
                    const float soft_ramp = 0.015f;

                    float br = B_o * r;
                    float x = br - E_o * (br - atanf(br));
                    float arg = C_o * atanf(x);
                    arg = fminf(fmaxf(arg, -1.5708f), 1.5708f);
                    float sa = D_o * sinf(arg);

                    float ramp = (ar < soft_ramp) ? (ar / soft_ramp) : 1.0f;
                    float asa = fabsf(sa);

                    float yaw_over = 0.0f;

                    if (asa > overSteerOffset) {
                        sa -= csignf(overSteerOffset, sa);
                        yaw_over = sa * (2.0f - asa) * effectiveOverSteerFactor * ramp;
                    }



                    // === Understeer (Front SAT Pacejka) ===
                    float yaw_under = 0.0f;
                    if (underSteerFactor > 0.0f) {
                        // Understeer internal
                        float underInternalOffset = underSteerOffset;

                        //When iRacing gives us wheelbase in telemetry
                        // Front slip angle (alpha_f) — replace 2.7f with *wheelbase
                        float alpha_f = *steer - (*yawRate * (2.7f / 2.0f) / *speed);
                        float a_af = fabsf(alpha_f);

                        // Tuned params
                        const float B_u = 3.0f, C_u = 1.75f, D_u_base = 0.70f, E_u = -0.55f;
                        float D_u = D_u_base * (1.0f + Q_DZ2 * dfz_front);  // Modulate peak with front dfz

                        float bu = B_u * alpha_f;
                        float xu = bu - E_u * (bu - atanf(bu));
                        float argu = C_u * atanf(xu);
                        argu = fminf(fmaxf(argu, -1.5708f), 1.5708f);
                        float sa_u = D_u * sinf(argu);

                        float asau = fabsf(sa_u);

                        if (asau > underInternalOffset) {
                            sa_u -= csignf(underInternalOffset, sa_u);
                            yaw_under = sa_u * (2.0f - asau) * underSteerFactor * 0.65f;
                            float entryRamp = (fabsf(*yawRate) < 0.1f) ? 0.5f : 1.0f;
                            yaw_under *= entryRamp;
                        }

                    }

                    //recombinging yaws
                    // Combine (your existing yaw)
                    yaw = yaw_over - yaw_under;   // note sign: oversteer usually positive yaw correction, understeer negative

                    // Final clamp + mode upscale
                    yaw = fminf(fmaxf(yaw, -halfMaxForce), halfMaxForce);


                    // ────────────────────────────────────────────────────────────────
                    // Compute and safely store shaker intensities (atomic write)
                    // ────────────────────────────────────────────────────────────────
                    if (isHighImpact.load(std::memory_order_relaxed)) {  // if you made isHighImpact atomic too
                        debug(L"isHighImpact = true. Suppressing forces");
                        yaw *= 0.25f;

                        // Reset to zero during impact
                        oversteerIntensity.store(0.0f, std::memory_order_relaxed);
                        understeerIntensity.store(0.0f, std::memory_order_relaxed);
                    }
                    else {
                        // Normal case: compute raw 0–100 values
                        float overRaw = fabsf(yaw_over) / MAX_EFFECT_FORCE * 100.0f;
                        float underRaw = fabsf(yaw_under) / MAX_EFFECT_FORCE * 100.0f;

                        // Soft clip to valid 0–100 range before storing
                        float overClipped = fminf(100.0f, fmaxf(0.0f, overRaw));
                        float underClipped = fminf(100.0f, fmaxf(0.0f, underRaw));

                        // Atomic store – relaxed is safe here (no strict ordering needed between these two)
                        oversteerIntensity.store(overClipped, std::memory_order_relaxed);
                        understeerIntensity.store(underClipped, std::memory_order_relaxed);
                    }
                

                    stopped = false;
                }
            }


                else {
                    // Speed is less than or equal to 2.0f
                    stopped = true;
                    oversteerIntensity = 0;
                    understeerIntensity = 0;
                    if (simHubConnectorStatus == true) updateSimHub();
                    
                   
                }



                // ────────────────────────────────────────────────────────────────
                // Write filtered yaw and combined forces to the BACK buffer
                // ────────────────────────────────────────────────────────────────
                for (int i = 0; i < DIRECT_INTERP_SAMPLES; i++) {
                    yawFilter[i] = yaw * firc6[i];  // still local

                    yawForce[writeBuf][i] = yawFilter[0] + yawFilter[1] + yawFilter[2] +
                        yawFilter[3] + yawFilter[4] + yawFilter[5];

                    ffbForce[writeBuf][i] = yawForce[writeBuf][i];  // Pacejka-modulated yaw
                }
                // Precompute scaledForces using the NEW back-buffer values
                    // (this ensures scaledForces are consistent with what consumers will see)
                for (int i = 0; i <= STmaxIdx; i++) {
                    float total = swTorqueST[i] + suspForceST[writeBuf][i] + yawForce[writeBuf][i];
                    scaledForces[i] = (float)scaleTorque(total);
                }



            halfSteerMax = *steerMax / 2.0f;
            if (abs(halfSteerMax) < 8.0f && abs(*steer) > halfSteerMax - STOPS_MAXFORCE_RAD * 2.0f)
                nearStops = true;
            else
                nearStops = false;

            //If we are in vjoy/direct modes then we get out of here.  
            if (
                !*isOnTrack ||
                settings.getFfbType() == FFBTYPE_GAME_360 ||
                settings.getFfbType() == FFBTYPE_GAME_720
            )
                continue;

            // Bump stops
            if (abs(halfSteerMax) < 8.0f && abs(*steer) > halfSteerMax) {
                
                float factor, invFactor;

                if (*steer > 0) {
                    factor = (-(*steer - halfSteerMax)) / STOPS_MAXFORCE_RAD;
                    factor = maxf(factor, -1.0f);
                    invFactor = 1.0f + factor;
                }
                else {
                    factor = (-(*steer + halfSteerMax)) / STOPS_MAXFORCE_RAD;
                    factor = minf(factor, 1.0f);
                    invFactor = 1.0f - factor;
                }

                setFFB((int)(factor * DI_MAX + scaleTorque(*swTorque) * invFactor));
               // debug(L"Main: Bumpstops: FFB Force: %d", force);
                continue;

            }
            // ────────────────────────────────────────────────────────────────
            // Publish the new buffer — consumers will now see writeBuf as front
            // ────────────────────────────────────────────────────────────────
            activeBuffer.store(writeBuf, std::memory_order_release);

            // Telemetry FFB
            switch (settings.getFfbType()) {
            case FFBTYPE_IRFFB_360: {


                for (int i = 0; i < STmaxIdx; i++) {
                    int force = (int)scaledForces[i];
                    if (isHighImpact) {

                        force = force / 5; //reduced force by 80% but not to zero
                       
                    }
                    setFFB(force);
                    sleepSpinUntil(&start, 2000, 2760 * (i + 1));
                }

                int force = (int)scaledForces[STmaxIdx];
                if (isHighImpact) {

                    force = force / 5; //reduced force by 80% but not to zero
                    
                }
                setFFB(force);
            }
             break;

            case FFBTYPE_IRFFB_720: {
;

                float diff = (swTorqueST[0] - lastTorque) / 2.0f;

                float sdiff = (suspForceST[writeBuf][0] - lastSuspForce) / 2.0f;

                int force = (int)(scaledForces[0] + diff + sdiff);




                if (isHighImpact) {
                    
                    force = force / 5; //reduced force by 80% but not to zero
                    
                }
                setFFB(force);
                sleepSpinUntil(&start, 0, 1380 * 1);

                int iMax = STmaxIdx << 1;
                for (int i = 1; i < iMax; i++) {
                    int idx = i >> 1;
                    if (i & 1) {
                        diff = (swTorqueST[idx + 1] - swTorqueST[idx]) / 2.0f;

                        sdiff = (suspForceST[writeBuf][idx + 1] - suspForceST[writeBuf][idx]) / 2.0f;
                        force = (int)(scaledForces[idx] + diff + sdiff);
                    }
                    else {
                        force = (int)scaledForces[idx];
                    }
                    if (isHighImpact) {

                        force = force / 5; //reduced force by 80% but not to zero
                        
                    }
                    sleepSpinUntil(&start, 0, 1380 * (i + 1));
                    setFFB(force);
                }

                force = (int)scaledForces[STmaxIdx];
                if (isHighImpact) {

                    force = force / 5; //reduced force by 80% but not to zero
                }
                sleepSpinUntil(&start, 0, 1380 * (iMax + 1));
                setFFB(force);

                lastTorque = swTorqueST[STmaxIdx];

                lastSuspForce = suspForceST[writeBuf][STmaxIdx];
            }
                                     break;

            }
        }
        
        // Did we lose iRacing?
        if (numHandles > 0 && !(hdr->status & irsdk_stConnected)) {
            text(L"Disconnected from iRacing");
            debug(L"Disconnected from iRacing");
            numHandles = 0;
            dataLen = 0;
            if (data != NULL) {
                free(data);
                data = NULL;
            }
            resetForces();
            onTrack = false;
            setOnTrackStatus(onTrack);
            setConnectedStatus(false);
;
            timeEndPeriod(1);


            if (car[0] != 0 && trackName[0] != 0) {
                settings.writeSettingsForCar(car, trackName);
            }
            debug(L"Lost Iracing: Writing Settings for car and track");
            iracingConnectedStatus = 0;
            

        }

        // Window messages
        if (res == numHandles) {

            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT)
                    DestroyWindow(mainWnd);
                if (!IsDialogMessage(mainWnd, &msg)) {
                    if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
                        TranslateMessage(&msg);
                        DispatchMessage(&msg);
                    }
                }
            }

        }

    }

    

    return (int)msg.wParam;

}

ATOM MyRegisterClass(HINSTANCE hInstance) {

    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_IRFFB));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_IRFFB);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);

}

LRESULT CALLBACK EditWndProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subId, DWORD_PTR rData) {

    if (msg == WM_CHAR) {

        wchar_t buf[8];

        if (subId == EDIT_FLOAT) {

            if (GetWindowTextW(wnd, buf, 8) && StrChrIW(buf, L'.') && wParam == '.')
                return 0;

            if (
                !(
                    (wParam >= L'0' && wParam <= L'9') ||
                    wParam == L'.' ||
                    wParam == VK_RETURN ||
                    wParam == VK_DELETE ||
                    wParam == VK_BACK
                )
            )
                return 0;

            LRESULT ret = DefSubclassProc(wnd, msg, wParam, lParam);

            wchar_t *end;
            float val = 0.0f;

            GetWindowText(wnd, buf, 8);
            val = wcstof(buf, &end);
            if (end - buf == wcslen(buf))
                SendMessage(GetParent(wnd), WM_EDIT_VALUE, reinterpret_cast<WPARAM &>(val), (LPARAM)wnd);

            return ret;

        }
        else {

            if (
                !(
                    (wParam >= L'0' && wParam <= L'9') ||
                    wParam == VK_RETURN ||
                    wParam == VK_DELETE ||
                    wParam == VK_BACK
                )
            )
                return 0;

            LRESULT ret = DefSubclassProc(wnd, msg, wParam, lParam);
            GetWindowText(wnd, buf, 8);
            int val = _wtoi(buf);
            SendMessage(GetParent(wnd), WM_EDIT_VALUE, (WPARAM)val, (LPARAM)wnd);
            return ret;

        }

    }

    return DefSubclassProc(wnd, msg, wParam, lParam);

}

HWND combo(HWND parent, wchar_t *name, int x, int y) {

    CreateWindowW(
        L"STATIC", name,
        WS_CHILD | WS_VISIBLE,
        x, y, 300, 20, parent, NULL, hInst, NULL
    );
    return 
        CreateWindow(
            L"COMBOBOX", nullptr,
            CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE | WS_OVERLAPPED | WS_TABSTOP,
            x + 12, y + 26, 300, 240, parent, nullptr, hInst, nullptr
        );

}

sWins_t *slider(HWND parent, wchar_t *name, int x, int y, wchar_t *start, wchar_t *end, bool floatData) {

    sWins_t *wins = (sWins_t *)malloc(sizeof(sWins_t));

    wins->label = CreateWindowW(
        L"STATIC", name,
        WS_CHILD | WS_VISIBLE,
        x, y, 300, 20, parent, NULL, hInst, NULL
    );

    wins->value = CreateWindowW(
        L"EDIT", L"", 
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_CENTER,
        x + 210, y, 50, 20, parent, NULL, hInst, NULL
    );

    SetWindowSubclass(wins->value, EditWndProc, floatData ? 1 : 0, 0);

    SendMessage(wins->value, EM_SETLIMITTEXT, 5, 0);

    wins->trackbar = CreateWindowExW(
        0, TRACKBAR_CLASS, name,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_TOOLTIPS | TBS_TRANSPARENTBKGND,
        x + 40, y + 26, 240, 30,
        parent, NULL, hInst, NULL
    );

    HWND buddyLeft = CreateWindowEx(
        0, L"STATIC", start,
        SS_LEFT | WS_CHILD | WS_VISIBLE,
        0, 0, 40, 20, parent, NULL, hInst, NULL
    );
    SendMessage(wins->trackbar, TBM_SETBUDDY, (WPARAM)TRUE, (LPARAM)buddyLeft);

    HWND buddyRight = CreateWindowEx(
        0, L"STATIC", end,
        SS_RIGHT | WS_CHILD | WS_VISIBLE,
        0, 0, 52, 20, parent, NULL, hInst, NULL
    );
    SendMessage(wins->trackbar, TBM_SETBUDDY, (WPARAM)FALSE, (LPARAM)buddyRight);

    return wins;

}

HWND checkbox(HWND parent, wchar_t *name, int x, int y) {

    return 
        CreateWindowEx(
            0, L"BUTTON", name,
            BS_CHECKBOX | BS_MULTILINE | WS_CHILD | WS_TABSTOP | WS_VISIBLE,
            x, y, 360, 58, parent, nullptr, hInst, nullptr
        );

}



BOOL InitInstance(HINSTANCE hInstance, int nCmdShow) {


    hInst = hInstance;

    mainWnd = CreateWindowW(
        szWindowClass, szTitle,
        WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 975, 900,
        NULL, NULL, hInst, NULL
    );

    // In InitInstance, before creating the combo
    initVJD();  // safe to call multiple times — it checks internally

    if (!mainWnd)
        return FALSE;

    // ─── Create cached white brush once ───
    if (!g_hWhiteBrush) {
        g_hWhiteBrush = CreateSolidBrush(RGB(255, 255, 255));
    }
    
    memset(&niData, 0, sizeof(niData));
    niData.uVersion = NOTIFYICON_VERSION;
    niData.cbSize = NOTIFYICONDATA_V1_SIZE;
    niData.hWnd = mainWnd;
    niData.uID = 1;
    niData.uFlags = NIF_ICON | NIF_MESSAGE;
    niData.uCallbackMessage = WM_TRAY_ICON;
    niData.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SMALL));



HFONT hTipsFont = CreateFontW(
    17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
);

HWND hTips = CreateWindowExW(
    WS_EX_CLIENTEDGE,
    L"EDIT",
    L"                                                  Quick Tips\r\n"
    L"--------------------------------------------------------------------------------\r\n"
    L"Select your wheel and recalibrate the wheel in iRacing\r\n"
    L"Configure in the following order for best results:\r\n"
    L"(1) Mode, (2) Max Force,(3) FFB Effects, (4) Damping, (5) Bumps\r\n"
    L"FFB Effects should be at a number greater than zero when setting Max Force.\r\n"
    L"--------------------------------------------------------------------------------\r\n"
    L"Game and irFFB modes will give you the same effects\r\n"
    L"Game mode requires vjoy and has lower latency\r\n"
    L"irFFB modes do not require vjoy but slightly higher latency\r\n"
    L"Game modes are not available if vJoy is not running\r\n"
    L"--------------------------------------------------------------------------------\r\n"
    L"Max Force is wheel strength.\r\n"
    L"Low number is stronger wheel. High number is weaker wheel.\r\n"
    L"Lower the number until you get less than 1% of clipping but more than zero.\r\n"
    L"Too low of a Max Force setting will cause clipping.\r\n"
    L"Excessive clipping will cause wheel oscillation. Avoid wheel oscillation.\r\n"
    L"--------------------------------------------------------------------------------\r\n"
    L"Auto Tune increases your Max Force setting to stop clipping.\r\n"
    L"Auto Tune will NOT lower your Max Force setting (make the wheel stronger).\r\n"
    L"Use SimHub AutoTune button binding to toggle Auto Tune on and off from car.\r\n"
    L"--------------------------------------------------------------------------------\r\n"
    L"FFB Effects controls how much of the seat-of-the-pants feel you get.\r\n"
    L"Increasing for more oversteer/understeer feeling in the wheel.\r\n"
    L"Lower Max Force(stronger wheel) setting needs higher FFB Effects setting.\r\n"
    L"--------------------------------------------------------------------------------\r\n"
    L"Settings are automatically saved in Documents folder.\r\n"
    L"--------------------------------------------------------------------------------\r\n"
    L"Set Bumps to your preference.\r\n"
    L"Set Damping to your preference.\r\n" 
    L"Try increasing Damping as you raise Max Force setting.\r\n"
    L"--------------------------------------------------------------------------------\r\n"
    L"Use SimHub overlays and button bindings to configure settings while in car.\r\n"
    L"In iRacing's \\Documents\\Racing\\app.ini file, ensure the following settings:\r\n"
    L"resetWhenFFBLost = 0\r\n"
    L"forceResetBeforeInit = 0\r\n"
    L"alwaysRestartFX = 0\r\n",
    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
    440, 60, 500, 625,
    mainWnd, NULL, hInst, NULL
);



SendMessage(hTips, 0x0445, FALSE, RGB(255, 255, 255));  //Used 0x0445 because EM_SETBKGNDCOLOR was causing error.
SendMessage(hTips, WM_SETFONT, (WPARAM)hTipsFont, TRUE);

    settings.setDevWnd(combo(mainWnd, L"FFB device:", 44, 20));
    settings.setFfbWnd(combo(mainWnd, L"FFB source - Smoothing:", 44, 80));

    settings.setUseDDWheelWnd(
        checkbox(mainWnd, L"Using A Direct Drive Wheel", 44, 130)
    );

    settings.setAutoTuneWnd(
        checkbox(mainWnd, L" Auto Tune to Stop Clipping", 44, 170)
    );

  settings.setMaxWnd(slider(mainWnd, L"Max Force:", 44, 230, L"5", L"200", false));

  CreateWindowW(
        L"STATIC",
        L"Stronger                                        Weaker",           
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        55, 286, 300, 20,               
        mainWnd, NULL, hInst, NULL
    );
    
 
    settings.setFFBEffectsLevelWnd(slider(mainWnd, L"FFB Effects:", 44, 325, L"0", L"100", true));


    settings.setBumpsWnd(slider(mainWnd, L"Bumps Intensity:", 44, 385, L"0", L"100", true));  

    settings.setDampingWnd(slider(mainWnd, L"Damping:", 44, 445, L"0", L"100", true));



    settings.setStartMinimisedWnd(
        checkbox(mainWnd, L" Start minimized?", 44, 492)
    );


    //Saving for re-enabling debug when time to code again
    //Turning off so people don't fill up their hard drives
    //Maybe put them ifdefs
     
 //   settings.setDebugWnd(
 //       checkbox(mainWnd, L"Debug logging?", 440, 700)
  //  );



    int statusParts[] = { 256, 340, 600, 864 };

    statusWnd = CreateWindowEx(
        0, STATUSCLASSNAME, NULL,
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, mainWnd, NULL, hInst, NULL
    );
    SendMessage(statusWnd, SB_SETPARTS, 4, LPARAM(statusParts));

    
    textWnd = CreateWindowEx(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_VISIBLE | WS_VSCROLL | WS_CHILD | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL,
        32, 553, 376, 240,
        mainWnd, NULL, hInst, NULL
    );
    SendMessage(textWnd, EM_SETLIMITTEXT, WPARAM(256000), 0);

    ShowWindow(mainWnd, SW_HIDE);
    UpdateWindow(mainWnd);

    if (simHubConnectorStatus == true) updateSimHub();

    return TRUE;

}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {

    HWND wnd = (HWND)lParam;

    switch (message) {

        case WM_COMMAND: {

            int wmId = LOWORD(wParam);
            switch (wmId) {

                case IDM_ABOUT:
                    DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                    break;
                case IDM_EXIT:
                    DestroyWindow(hWnd);
                    break;

                default:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        if (wnd == settings.getDevWnd()) {
                            GUID oldDevice = settings.getFfbDevice();
                            DWORD vidpid = 0;
                            if (oldDevice != GUID_NULL)
                                vidpid = getDeviceVidPid(ffdevice);
                                settings.setFfbDevice(SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0));


                        }
                        else if (wnd == settings.getFfbWnd()) {
                            settings.setFfbType(SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0)); 
                            if (simHubConnectorStatus == true) updateSimHub();

                        }


                    }
                    else if (HIWORD(wParam) == BN_CLICKED) {
                        bool oldValue = SendMessage((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        if (wnd == settings.getUseDDWheelWnd())
                            settings.setUseDDWheel(!oldValue);
                    

                        if (wnd == settings.getAutoTuneWnd())
                            settings.setAutoTune(!oldValue);
                    


                        else if (wnd == settings.getStartMinimisedWnd())
                            settings.setStartMinimised(!oldValue);

                        else if (wnd == settings.getDebugWnd()) {

                            settings.setDebug(!oldValue);
                            if (settings.getDebug()) {
                                debugHnd = CreateFileW(settings.getLogPath(), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                                int chars = SendMessageW(textWnd, WM_GETTEXTLENGTH, 0, 0);
                                wchar_t *buf = new wchar_t[chars + 1], *str = buf;
                                SendMessageW(textWnd, WM_GETTEXT, chars + 1, (LPARAM)buf);
                                wchar_t *end = StrStrW(str, L"\r\n");
                                while (end) {                                    
                                    *end = '\0';
                                    debug(str);
                                    str = end + 2;
                                    end = StrStrW(str, L"\r\n");
                                }
                                delete[] buf;
                            }
                            else if (debugHnd != INVALID_HANDLE_VALUE) {
                                CloseHandle(debugHnd);
                                debugHnd = INVALID_HANDLE_VALUE;
                            }
                        }
                    }
                    return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;

        case WM_EDIT_VALUE: {
            if (wnd == settings.getMaxWnd()->value) {
                settings.setMaxForce(wParam, wnd);
                if (simHubConnectorStatus == true) updateSimHub();
            }



            else if (wnd == settings.getBumpsWnd()->value)
                settings.setBumpsFactor(reinterpret_cast<float&>(wParam), wnd);


            else if (wnd == settings.getDampingWnd()->value) {
                settings.setDampingFactor(reinterpret_cast<float&>(wParam), wnd);
                if (simHubConnectorStatus == true) updateSimHub();
            }

            else if (wnd == settings.getFFBEffectsLevelWnd()->value)
            {

                settings.setFFBEffectsLevel(reinterpret_cast<float&>(wParam), wnd);

                if (simHubConnectorStatus == true) updateSimHub();

            }

           

            
        }
        break;
             

        case WM_HSCROLL: {
            if (wnd == settings.getMaxWnd()->trackbar) {
                settings.setMaxForce(SendMessage(wnd, TBM_GETPOS, 0, 0), wnd);
                if (simHubConnectorStatus == true) updateSimHub();
            }


            else if (wnd == settings.getBumpsWnd()->trackbar)
                settings.setBumpsFactor((float)SendMessage(wnd, TBM_GETPOS, 0, 0), wnd);

            else if (wnd == settings.getDampingWnd()->trackbar)
                settings.setDampingFactor((float)SendMessage(wnd, TBM_GETPOS, 0, 0), wnd);



            else if (wnd == settings.getFFBEffectsLevelWnd()->trackbar)
                settings.setFFBEffectsLevel((float)SendMessage(wnd, TBM_GETPOS, 0, 0), wnd);
            


        }
        break;


        case WM_CTLCOLORSTATIC: {
            SetBkColor((HDC)wParam, RGB(255, 255, 255));
            SetBkMode((HDC)wParam, OPAQUE);
            SetTextColor((HDC)wParam, RGB(0, 0, 0));
            return (LRESULT)GetStockObject(WHITE_BRUSH);  // no leak
        }
                              break;


        case WM_PRINTCLIENT: {
            RECT r = { 0 };
            GetClientRect(hWnd, &r);
            FillRect((HDC)wParam, &r, CreateSolidBrush(RGB(0xff, 0xff, 0xff)));
        }
        break;

        case WM_SIZE: {
            SendMessage(statusWnd, WM_SIZE, wParam, lParam);
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;

        case WM_POWERBROADCAST: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case PBT_APMSUSPEND:
                    debug(L"Computer is suspending, release all");
                    releaseAll();
                break;
                case PBT_APMRESUMESUSPEND:
                    debug(L"Computer is resuming, init all");
                    initAll();
                break;
            }
        }
        break;

        case WM_TRAY_ICON: {
            switch (lParam) {
                case WM_LBUTTONUP:
                    restore();
                    break;
                case WM_RBUTTONUP: {
                    HMENU trayMenu = CreatePopupMenu();
                    POINT curPoint;
                    AppendMenuW(trayMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
                    GetCursorPos(&curPoint);
                    SetForegroundWindow(hWnd);
                    if (
                        TrackPopupMenu(
                            trayMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                            curPoint.x, curPoint.y, 0, hWnd, NULL
                        ) == ID_TRAY_EXIT
                    )
                        PostQuitMessage(0);
                    DestroyMenu(trayMenu);
                }
                break;
            }
                    
        }
        break;

        case WM_DEVICECHANGE: {
            DEV_BROADCAST_HDR *hdr = (DEV_BROADCAST_HDR *)lParam;
            if (wParam != DBT_DEVICEARRIVAL && wParam != DBT_DEVICEREMOVECOMPLETE)
                return 0;
            if (hdr->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE)
                return 0;
            deviceChange();
        }
        break;

        case WM_SYSCOMMAND: {
            switch (wParam & 0xfff0) {
                case SC_MINIMIZE:
                    minimise();
                    return 0;
                default:
                    return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;

        case WM_DESTROY: {
            debug(L"Exiting");
            Shell_NotifyIcon(NIM_DELETE, &niData);
            releaseAll();


            if (g_hWhiteBrush) {
                DeleteObject(g_hWhiteBrush);
                g_hWhiteBrush = NULL;
            }

            if (hTipsFont) {
                DeleteObject(hTipsFont);
                hTipsFont = NULL;
            }

            if (car[0] != 0 && trackName[0] != 0) {
                settings.writeSettingsForCar(car, trackName);
            }
            else
                settings.writeGenericSettings();
            settings.writeRegSettings();
            if (debugHnd != INVALID_HANDLE_VALUE)
                CloseHandle(debugHnd);
            CloseHandle(globalMutex);
            exit(0);
        }


        break;

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }

    return 0;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {

    UNREFERENCED_PARAMETER(lParam);

    switch (message) {

        case WM_INITDIALOG:
            return (INT_PTR)TRUE;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                EndDialog(hDlg, LOWORD(wParam));
                return (INT_PTR)TRUE;
            }
        break;
    }

    return (INT_PTR)FALSE;

}

void text(wchar_t *fmt, ...) {

    va_list argp;
    wchar_t msg[512];
    va_start(argp, fmt);

    StringCbVPrintf(msg, sizeof(msg) - 2 * sizeof(wchar_t), fmt, argp);
    va_end(argp);
    StringCbCat(msg, sizeof(msg), L"\r\n");

    SendMessage(textWnd, EM_SETSEL, 0, -1);
    SendMessage(textWnd, EM_SETSEL, -1, 1);
    SendMessage(textWnd, EM_REPLACESEL, 0, (LPARAM)msg);
    SendMessage(textWnd, EM_SCROLLCARET, 0, 0);

    debug(msg);

}

void text(wchar_t *fmt, char *charstr) {

    int len = strlen(charstr) + 1;
    wchar_t *wstr = new wchar_t[len];
    mbstowcs_s(nullptr, wstr, len, charstr, len);
    text(fmt, wstr);
    delete[] wstr;

}

void debug(wchar_t *msg) {

    if (!settings.getDebug())
        return;
    
    DWORD written;
    wchar_t buf[512];
    SYSTEMTIME lt;

    GetLocalTime(&lt);
    StringCbPrintfW(
        buf, sizeof(buf), L"%d-%02d-%02d %02d:%02d:%02d.%03d ",
        lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds
    );

    StringCbCat(buf, sizeof(buf), msg);
    StringCbCat(buf, sizeof(buf), L"\r\n");

    if (!wcscmp(msg, debugLastMsg)) {
        debugRepeat++;
        return;
    }
    else if (debugRepeat) {
        wchar_t rm[256];
        StringCbPrintfW(rm, sizeof(rm), L"-- Last message repeated %d times --\r\n", debugRepeat);
        WriteFile(debugHnd, rm, wcslen(rm) * sizeof(wchar_t), &written, NULL);
        debugRepeat = 0;
    }

    StringCbCopy(debugLastMsg, sizeof(debugLastMsg), msg);
    WriteFile(debugHnd, buf, wcslen(buf) * sizeof(wchar_t), &written, NULL);

}

template <typename... T>
void debug(wchar_t *fmt, T... args) {

    if (!settings.getDebug())
        return;

    wchar_t msg[512];
    StringCbPrintf(msg, sizeof(msg), fmt, args...);
    debug(msg);

}

void setTrackNameStatus(char* trackNameStr) {

    if (!trackNameStr || trackNameStr[0] == 0) {
        SendMessage(statusWnd, SB_SETTEXT, STATUS_TRACK_NAME_PART, LPARAM(L"Track: "));
        return;
    }

    int len = strlen(trackNameStr) + 1;
    wchar_t* wstr = new wchar_t[len + 5];
    lstrcpy(wstr, L"Track: ");
    mbstowcs_s(nullptr, wstr + 5, len, trackNameStr, len);
    SendMessage(statusWnd, SB_SETTEXT, STATUS_TRACK_NAME_PART, LPARAM(wstr));
    delete[] wstr;

}

void setCarStatus(char *carStr) {

    if (!carStr || carStr[0] == 0) {
        SendMessage(statusWnd, SB_SETTEXT, STATUS_CAR_PART, LPARAM(L"Car: generic"));
        return;
    }

    int len = strlen(carStr) + 1;
    wchar_t *wstr = new wchar_t[len + 5];
    lstrcpy(wstr, L"Car: ");
    mbstowcs_s(nullptr, wstr + 5, len, carStr, len);
    SendMessage(statusWnd, SB_SETTEXT, STATUS_CAR_PART, LPARAM(wstr));
    delete[] wstr;

}

void setConnectedStatus(bool connected) {

    SendMessage(
        statusWnd, SB_SETTEXT, STATUS_CONNECTED_PART,
        LPARAM(connected ? L"iRacing connected" : L"iRacing disconnected")
    );
    if (connected == true) {
        iracingConnectedStatus = 1;
    }
    else if (connected == false) {
        iracingConnectedStatus = 0;
    }
}

void setOnTrackStatus(bool onTrack) {

    SendMessage(
        statusWnd, SB_SETTEXT, STATUS_ONTRACK_PART,
        LPARAM(onTrack ? L"On track" : L"Not on track")
    );

    if (!onTrack && deviceChangePending) {
        debug(L"Processing deferred device change notification");
        deviceChange();
    }

}

void setLogiWheelRange(WORD prodId) {

    if (prodId == G25PID || prodId == DFGTPID || prodId == G27PID) {

        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);

        text(L"DFGT/G25/G27 detected, setting range using raw HID");

        HANDLE devInfoSet = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devInfoSet == INVALID_HANDLE_VALUE) {
            text(L"LogiWheel: Error enumerating HID devices");
            return;
        }

        SP_DEVICE_INTERFACE_DATA intfData;
        SP_DEVICE_INTERFACE_DETAIL_DATA *intfDetail;
        DWORD idx = 0;
        DWORD error = 0;
        DWORD size;

        while (true) {

            intfData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

            if (!SetupDiEnumDeviceInterfaces(devInfoSet, NULL, &hidGuid, idx++, &intfData)) {
                if (GetLastError() == ERROR_NO_MORE_ITEMS)
                    break;
                continue;
            }

            if (!SetupDiGetDeviceInterfaceDetailW(devInfoSet, &intfData, NULL, 0, &size, NULL))
                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
                    text(L"LogiWheel: Error getting intf detail");
                    continue;
                }

            intfDetail = (SP_DEVICE_INTERFACE_DETAIL_DATA *)malloc(size);
            intfDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

            if (!SetupDiGetDeviceInterfaceDetailW(devInfoSet, &intfData, intfDetail, size, NULL, NULL)) {
                free(intfDetail);
                continue;
            }

            if (
                wcsstr(intfDetail->DevicePath, G25PATH)  != NULL ||
                wcsstr(intfDetail->DevicePath, DFGTPATH) != NULL ||
                wcsstr(intfDetail->DevicePath, G27PATH) != NULL
            ) {

                HANDLE file = CreateFileW(
                    intfDetail->DevicePath,
                    GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL
                );

                if (file == INVALID_HANDLE_VALUE) {
                    text(L"LogiWheel: Failed to open HID device");
                    free(intfDetail);
                    SetupDiDestroyDeviceInfoList(devInfoSet);
                    return;
                }

                DWORD written;

                if (!WriteFile(file, LOGI_WHEEL_HID_CMD, LOGI_WHEEL_HID_CMD_LEN, &written, NULL))
                    text(L"LogiWheel: Failed to write to HID device");
                else
                    text(L"LogiWheel: Range set to 900 deg via raw HID");

                CloseHandle(file);
                free(intfDetail);
                SetupDiDestroyDeviceInfoList(devInfoSet);
                return;

            }

            free(intfDetail);

        }

        text(L"Failed to locate Logitech wheel HID device, can't set range");
        SetupDiDestroyDeviceInfoList(devInfoSet);
        return;

    }

    text(L"Attempting to set range via LGS");

    UINT msgId = RegisterWindowMessage(L"LGS_Msg_SetOperatingRange");
    if (!msgId) {
        text(L"Failed to register LGS window message, can't set range..");
        return;
    }

    HWND LGSmsgHandler =
        FindWindowW(
            L"LCore_MessageHandler_{C464822E-04D1-4447-B918-6D5EB33E0E5D}",
            NULL
        );

    if (LGSmsgHandler == NULL) {
        text(L"Failed to locate LGS msg handler, can't set range..");
        return;
    }

    SendMessageW(LGSmsgHandler, msgId, prodId, 900);
    text(L"Range of Logitech wheel set to 900 deg via LGS");

}

BOOL CALLBACK EnumFFDevicesCallback(LPCDIDEVICEINSTANCE diDevInst, VOID *wnd) {

    UNREFERENCED_PARAMETER(wnd);

    if (lstrcmp(diDevInst->tszProductName, L"vJoy Device") == 0)
        return true;

    settings.addFfbDevice(diDevInst->guidInstance, diDevInst->tszProductName);
    debug(L"Adding DI device: %s", diDevInst->tszProductName);

    return true;

}

BOOL CALLBACK EnumObjectCallback(const LPCDIDEVICEOBJECTINSTANCE inst, VOID *dw) {

    UNREFERENCED_PARAMETER(inst);

    (*(int *)dw)++;
    return DIENUM_CONTINUE;

}

void enumDirectInput() {

    settings.clearFfbDevices();

    if (
        FAILED(
            DirectInput8Create(
                GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8,
                (VOID **)&pDI, nullptr
            )
        )
    ) {
        text(L"Failed to initialise DirectInput");
        return;
    }

    pDI->EnumDevices(
        DI8DEVCLASS_GAMECTRL, EnumFFDevicesCallback, settings.getDevWnd(),
        DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK
    );

}

void initDirectInput() {

    DIDEVICEINSTANCE di;
    HRESULT hr;

    numButtons = numPov = 0;
    di.dwSize = sizeof(DIDEVICEINSTANCE);

    text(L"Initializing Direct Input");

    if (ffdevice && effect && ffdevice->GetDeviceInfo(&di) >= 0 && di.guidInstance == settings.getFfbDevice())
        return;

    

    releaseDirectInput();  //releasing because we failed to connect to wheel device

    if (
        FAILED(
            DirectInput8Create(
                GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8,
                (VOID **)&pDI, nullptr
            )
        )
    ) {
        text(L"Failed to initialise DirectInput");
        return;
    }

    if (FAILED(pDI->CreateDevice(settings.getFfbDevice(), &ffdevice, nullptr))) {
        text(L"Failed to create DI device");
        text(L"Is it connected and powered on?");
        return;
    }
    if (FAILED(ffdevice->SetDataFormat(&c_dfDIJoystick))) {
        text(L"Failed to set DI device DataFormat!");
        return;
    }
    if (FAILED(ffdevice->SetCooperativeLevel(mainWnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND))) {
        text(L"Failed to set DI device CooperativeLevel!");
        return;
    }

    if (FAILED(ffdevice->GetDeviceInfo(&di))) {
        text(L"Failed to get info for DI device!");
        return;
    }

    if (FAILED(ffdevice->EnumObjects(EnumObjectCallback, (VOID *)&numButtons, DIDFT_BUTTON))) {
        text(L"Failed to enumerate DI device buttons");
        return;
    }

    if (FAILED(ffdevice->EnumObjects(EnumObjectCallback, (VOID *)&numPov, DIDFT_POV))) {
        text(L"Failed to enumerate DI device povs");
        return;
    }

    if (FAILED(ffdevice->SetEventNotification(wheelEvent))) {
        text(L"Failed to set event notification on DI device");
        return;
    }

    DWORD vidpid = getDeviceVidPid(ffdevice);
    if (LOWORD(vidpid) == 0x046d) {
        logiWheel = true;
        setLogiWheelRange(HIWORD(vidpid));
    }
    else
        logiWheel = false;

    if (FAILED(ffdevice->Acquire())) {
        text(L"Failed to acquire DI device");
        return;
    }

    text(L"Acquired DI device with %d buttons and %d POV", numButtons, numPov);

    wheelAndEffectReady = true;
    debug(L"Wheel and effect fully initialized → readWheelThread can start polling");

    EnterCriticalSection(&effectCrit);

    if (FAILED(ffdevice->CreateEffect(GUID_Sine, &dieff, &effect, nullptr))) {
        text(L"Failed to create sine periodic effect");
        LeaveCriticalSection(&effectCrit);
        return;
    }

    if (!effect) {
        text(L"Effect creation failed");
        LeaveCriticalSection(&effectCrit);
        return;
    }

    hr = effect->SetParameters(&dieff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
    if (hr == DIERR_NOTINITIALIZED || hr == DIERR_INPUTLOST || hr == DIERR_INCOMPLETEEFFECT || hr == DIERR_INVALIDPARAM)
        text(L"Error setting parameters of DIEFFECT: %d", hr);

    LeaveCriticalSection(&effectCrit);
    //  Signal that polling can safely begin ───
    wheelAndEffectReady = true;
    debug(L"Wheel and effect fully initialized → readWheelThread can start polling");

}

void releaseDirectInput() {

    text(L"Releasing Direct Input Connection");

    if (effect) {
        oversteerIntensity = 0;
        understeerIntensity = 0;
        setFFB(0);
        EnterCriticalSection(&effectCrit);
        effect->Stop();
        effect->Release();
        effect = nullptr;
        LeaveCriticalSection(&effectCrit);
    }
    if (ffdevice) {
        ffdevice->Unacquire();
        ffdevice->Release();
        ffdevice = nullptr;
    }
    if (pDI) {
        pDI->Release();
        pDI = nullptr;
    }
    // Pause polling during release ───
    wheelAndEffectReady = false;
    debug(L"DirectInput released → pausing readWheelThread polling");

}


void reacquireDIDevice() {
    text(L"require device");

    if (ffdevice == nullptr) {
        text(L"FFB failed to reacquire - device null");
        debug(L"ffdevice was null during reacquire");
        directInputStatus = 0;
        return;
    }
    HRESULT hr;
    ffdevice->Unacquire();
    if (FAILED(ffdevice->Acquire())) {
        text(L"Reacquire failed");
        directInputStatus = 0;
        return;
    }
    EnterCriticalSection(&effectCrit);
    // Stop only if effect exists
    if (effect != nullptr) {
        effect->Stop();
    }
    if (effect == nullptr) {
        // Recreate if lost
        if (FAILED(ffdevice->CreateEffect(GUID_Sine, &dieff, &effect, nullptr))) {
            text(L"Failed to recreate effect on reacquire");
            LeaveCriticalSection(&effectCrit);
            directInputStatus = 0;
            return;
        }
    }
    // Now effect should exist — restart it
    Sleep(5); // tiny settle time
    hr = effect->SetParameters(&dieff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
    if (FAILED(hr)) {
        debug(L"SetParameters failed on reacquire: 0x%x - releasing and recreating", hr);
        if (effect != nullptr) {
            effect->Release();
            effect = nullptr;
        }
        if (FAILED(ffdevice->CreateEffect(GUID_Sine, &dieff, &effect, nullptr))) {
            text(L"Effect recreation failed on reacquire");
            LeaveCriticalSection(&effectCrit);
            directInputStatus = 0;
            return;
        }
        // Try set params one more time
        hr = effect->SetParameters(&dieff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
        if (FAILED(hr)) {
            text(L"Final SetParameters failed on reacquire: 0x%x", hr);
            LeaveCriticalSection(&effectCrit);
            directInputStatus = 0;
            return;
        }
    }
    LeaveCriticalSection(&effectCrit);
    directInputStatus = 1;
    wheelAndEffectReady = true;
    debug(L"Reacquire succeeded → readWheelThread polling resumed");
}




// ----------------------------------------------------------------------------
// Precise timed wait using NtDelayExecution when available, otherwise spin
// with safety timeout to prevent infinite CPU usage
// ----------------------------------------------------------------------------
inline void sleepSpinUntil(LARGE_INTEGER* base, UINT initialSleepUs, UINT offsetUs)
{
    LARGE_INTEGER now, target;
    target.QuadPart = base->QuadPart +
        ((LONGLONG)offsetUs * freq.QuadPart) / 1000000LL;

    // Optional short sleep at beginning (helps when we know we're early)
    if (initialSleepUs > 0)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(initialSleepUs));
    }

    QueryPerformanceCounter(&now);
    if (now.QuadPart >= target.QuadPart) return;   // already past target

    LONGLONG remaining_ticks = target.QuadPart - now.QuadPart;
    LONGLONG remaining_ns = (remaining_ticks * 1000000000LL) / freq.QuadPart;

    if (NtDelayExecution && remaining_ns > 2000)   // only use Nt if meaningfully long
    {
        LARGE_INTEGER delay;
        delay.QuadPart = -(remaining_ns / 100LL);   // negative = relative, 100ns units

        NTSTATUS status = NtDelayExecution(FALSE, &delay);
        if (status >= 0)
        {
            return;   // success → we're done
        }
        // else fall through to spin (Nt failed or interrupted)
    }

    // ────────────────────────────────────────────────────────────────
    // Fallback spin loop with safety escape
    // ────────────────────────────────────────────────────────────────
    int spins = 0;
    static bool warned_infinite = false;

    do
    {
        _mm_pause();                    // reduce power consumption & hyper-threading contention
        QueryPerformanceCounter(&now);

        spins++;
        if (spins >= SLEEP_SPIN_MAX_ITERATIONS)
        {
            if (!warned_infinite)
            {
                debug(L"sleepSpinUntil: hit max spin iterations! (possible timing glitch)");
                warned_infinite = true;
            }
            // Emergency short sleep so we don't completely lock the core
            std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_SPIN_EMERGENCY_MS));
            break;
        }
    } while (now.QuadPart < target.QuadPart);
}

//
inline int scaleTorque(float t) {

    return (int)(t * settings.getScaleFactor());

}






// ============================================================================
// setFFB – Raise-only adaptive maxForce with lap-based stable learning
// - Clip window updated EVERY FRAME for accurate counting
// - Heavy raise/lap/learn logic throttled to every 10th call (~166 ms @ 60 Hz)
// - Safe fallback when telemetry not ready (nullptr pointers)
// - Learns stable maxForce after X consecutive laps without raises
// - Resets lap counter on raise, slider change, or off-track
// - Uses higher clip threshold after learning
// - Ignores impact spikes after partial stability
// ============================================================================
inline void setFFB(int incomingForce)
{
    if (!effect) return;

    int processed = incomingForce;

    // Basic processing always runs (min force, parked, status) — low latency
    if (!settings.getUseDDWheel())
    {
        const int minForce = MIN_FORCE;
        if (processed > 0 && processed < minForce) processed = minForce;
        if (processed < 0 && processed > -minForce) processed = -minForce;
    }

    if (stopped)
    {
        processed /= PARKED_FORCE_REDUCER;
    }

    irFFBStatus = (processed != 0) ? 1 : 0;

    // Early telemetry safety – skip adaptive entirely if pointers missing

    if (!trackSurface || !lapCompleted)
    {
       // debug(L"setFFB: trackSurface:%d lapCompleted: %d", *trackSurface, *lapCompleted);
       // debug(L"setFFB: basic processing");
        // Basic passthrough only
        if (processed > IR_MAX) processed = IR_MAX;
        if (processed < -IR_MAX) processed = -IR_MAX;



        ffbMag = processed;
        return;
    }
    //debug(L"setFFB: clipping check");

    // ─── Clip detection & window update – EVERY FRAME (cheap, keeps count accurate) ───
    bool isClippingThisFrame = (incomingForce <= -IR_MAX || incomingForce >= IR_MAX);
    if (isClippingThisFrame)
    {
        clippedSamples++;
        clippingStatus = 1;
        clippingCounter++;
    }
    else
    {
        clippingStatus = 0;
    }
   // debug(L"setFFB: Clipping Count: %d", clippingCounter);


    if (settings.getAutoTune()) {

        // ─── Throttled heavy adaptive logic ───
        static int frameSkip = 0;
        bool runHeavyLogic = (++frameSkip % 10 == 0);

        // Persistent state
        static int userPreferredMaxForce = settings.getMaxForce();
        static int activeMaxForce = userPreferredMaxForce;
        static int highestUnstableMax = userPreferredMaxForce;
        static int clipWindow[40] = { 0 };
        static int windowPos = 0;
        static int recentClipsInWindow = 0;
        static int framesSinceAnyChange = 0;
        static int lastKnownUserMax = -1;
        static bool everRaised = false;

        static int consecutiveStableLaps = 0;
        static int lastLapCompleted = -1;
        static int learnedStableMaxForce = 0;
        static bool wasRacingSurfaceLastCall = false;

        // Tunables
        const int WINDOW_SIZE = 40;
        const int BASE_CLIPS_NEEDED_TO_RAISE = 10;
        const int STABLE_CLIPS_NEEDED_TO_RAISE = 40;
        const int MIN_FRAMES_COOLDOWN = 180;
        const int LAPS_TO_LEARN_STABLE = 3;
        const int RACING_SURFACE = 3;
        const int RAISE_STEP_NM = 2;
        const int MAX_SAFE_MAXFORCE = 60;

        // 1. Off-track detection – every frame (needs to catch surface changes quickly)
        if ((*trackSurface != RACING_SURFACE) && wasRacingSurfaceLastCall)
        {
            //debug(L"Off track → resetting stable lap counter");
            consecutiveStableLaps = 0;
        }
        wasRacingSurfaceLastCall = (*trackSurface == RACING_SURFACE);

        // 2. Slider change detection – every frame (user expects instant response)
        int currentUserMax = settings.getMaxForce();
        if (lastKnownUserMax != currentUserMax)
        {
            //debug(L"Slider changed: %d → %d", lastKnownUserMax, currentUserMax);

            if (learnedStableMaxForce > 0 && currentUserMax < learnedStableMaxForce)
            {
                //debug(L"User lowered below learned stable → resetting learned value");
                learnedStableMaxForce = 0;
                consecutiveStableLaps = 0;
                everRaised = false;
                memset(clipWindow, 0, sizeof(clipWindow));
                recentClipsInWindow = 0;
                windowPos = 0;
            }

            userPreferredMaxForce = currentUserMax;
            activeMaxForce = currentUserMax;
            highestUnstableMax = currentUserMax;
            lastKnownUserMax = currentUserMax;
            framesSinceAnyChange = 0;

            //debug(L"Slider change processed");
        }

        framesSinceAnyChange++;

        // 3. Clip window update – EVERY FRAME (critical for accurate counting)
        bool addClipToWindow = true;
        if (consecutiveStableLaps >= 1 && isHighImpact && isClippingThisFrame)
        {
            // debug(L"Ignoring clip during high impact (post-stability)");
            addClipToWindow = false;
        }

        recentClipsInWindow -= clipWindow[windowPos];
        clipWindow[windowPos] = addClipToWindow && isClippingThisFrame ? 1 : 0;
        recentClipsInWindow += clipWindow[windowPos];
        windowPos = (windowPos + 1) % WINDOW_SIZE;

        samples++;

        //debug(L"setFFB: framesSinceChange: %d recentClipsWin: %d", framesSinceAnyChange, recentClipsInWindow);
        // 4. Heavy raise / lap / learn logic – only every 10th frame
        bool didRaiseThisFrame = false;
        if (runHeavyLogic)
        {
            // debug(L"setFFB: running heavy logic");
             // Raise check
            if (framesSinceAnyChange >= MIN_FRAMES_COOLDOWN)
            {
                int currentThreshold = (learnedStableMaxForce > 0)
                    ? STABLE_CLIPS_NEEDED_TO_RAISE
                    : BASE_CLIPS_NEEDED_TO_RAISE;

                if (recentClipsInWindow >= currentThreshold)
                {
                    int candidate = activeMaxForce + RAISE_STEP_NM;
                    candidate = min(candidate, MAX_SAFE_MAXFORCE);

                    if (candidate > activeMaxForce)
                    {
                        settings.setMaxForce(candidate, (HWND)-1);
                        activeMaxForce = candidate;
                        lastKnownUserMax = activeMaxForce;

                        if (simHubConnectorStatus) updateSimHub();

                        if (candidate > highestUnstableMax)
                        {
                            highestUnstableMax = candidate;
                            text(L"Increasing Max Force to %d", candidate);
                            //debug(L"Increasing Max Force to %d Nm (new ceiling)", candidate);
                        }
                        else
                        {
                            text(L"Increasing Max Force to %d Nm due to clipping", candidate);
                            //debug(L"Increasing Max Force to %d Nm due to clipping", candidate);
                        }

                        everRaised = true;
                        didRaiseThisFrame = true;
                        framesSinceAnyChange = 0;
                        memset(clipWindow, 0, sizeof(clipWindow));
                        recentClipsInWindow = 0;
                        windowPos = 0;
                        consecutiveStableLaps = 0;

                        // debug(L"Raise occurred → stable lap count reset to 0");
                    }
                }
            }


            if (lapCompleted) {
                int newLap = *lapCompleted;
                if (newLap > lastLapCompleted && newLap >= 0) {
                    if (!didRaiseThisFrame)
                    {
                        consecutiveStableLaps++;
                        // debug(L"Clean lap completed → stable laps: %d / %d",
                        //     consecutiveStableLaps, LAPS_TO_LEARN_STABLE);
                    }
                    else
                    {
                        consecutiveStableLaps = 0;
                        // debug(L"Raise this lap → stable count reset to 0");
                    }
                    lastLapCompleted = newLap;
                }
            }


            // Learn stable point
            if (!didRaiseThisFrame && everRaised &&
                consecutiveStableLaps >= LAPS_TO_LEARN_STABLE &&
                learnedStableMaxForce == 0)
            {
                learnedStableMaxForce = activeMaxForce;
                text(L"Stablized Max Force: %d after %d clean laps",
                    learnedStableMaxForce, consecutiveStableLaps);
                // debug(L"Learned stable maxForce: %d after %d clean laps",
                //     learnedStableMaxForce, consecutiveStableLaps);
            }


        }

        // Final clip & apply (always runs)
        if (processed > IR_MAX) processed = IR_MAX;
        if (processed < -IR_MAX) processed = -IR_MAX;

        ffbMag = processed;
    }
    else {

        
            // Auto Tune OFF: use fixed user max force, no raises
            // Still apply basic clip
            if (processed > IR_MAX) processed = IR_MAX;
            if (processed < -IR_MAX) processed = -IR_MAX;

            ffbMag = processed;
        }

    // Periodic SimHub report
    if (clippingCounter >= MAXCLIPPINGCOUNT)
    {
        if (simHubConnectorStatus) updateSimHub();
        clippingCounter = 0;
    }
}


bool initVJD() {

    WORD verDrv;
    int maxVjDev;
    VjdStat vjdStatus = VJD_STAT_UNKN;

    if (!vJoyEnabled()) {
        text(L"vJoy not enabled!");
        vJoyReady = false;
        settings.setVjoyResult(false);
        return false;
    }
    else
        text(L"vJoy driver version %04x init OK", &verDrv);

    vjDev = 1;

    if (!GetvJoyMaxDevices(&maxVjDev)) {
        text(L"Failed to determine max number of vJoy devices");
        vJoyReady = false;
        settings.setVjoyResult(false);
        return false;
    }

    while (vjDev <= maxVjDev) {

        vjdStatus = GetVJDStatus(vjDev);

        if (vjdStatus == VJD_STAT_BUSY || vjdStatus == VJD_STAT_MISS)
            goto NEXT;
        if (!GetVJDAxisExist(vjDev, HID_USAGE_X))
            goto NEXT;
        if (!IsDeviceFfb(vjDev))
            goto NEXT;
        if (
            !IsDeviceFfbEffect(vjDev, HID_USAGE_CONST) ||
            !IsDeviceFfbEffect(vjDev, HID_USAGE_SINE)  ||
            !IsDeviceFfbEffect(vjDev, HID_USAGE_DMPR)  ||
            !IsDeviceFfbEffect(vjDev, HID_USAGE_FRIC)  ||
            !IsDeviceFfbEffect(vjDev, HID_USAGE_SPRNG)
        ) {
            text(L"vjDev %d: Not all required FFB effects are enabled", vjDev);
            text(L"Enable all FFB effects to use this device");
            vJoyReady = false;
            goto NEXT;
        }
        break;

NEXT:
        vjDev++;

    }

    if (vjDev > maxVjDev) {
        text(L"Failed to find suitable vJoy device!");
        text(L"Create a device with an X axis and all FFB effects enabled");
        vJoyReady = false;
        settings.setVjoyResult(false);
        return false;
    }

    memset(&ffbPacket, 0 ,sizeof(ffbPacket));

    if (vjdStatus == VJD_STAT_OWN) {
        RelinquishVJD(vjDev);
        vjdStatus = GetVJDStatus(vjDev);
    }
    if (vjdStatus == VJD_STAT_FREE) {
        if (!AcquireVJD(vjDev, ffbEvent, &ffbPacket)) {
            text(L"Failed to acquire vJoy device %d!", vjDev);
            vJoyReady = false;
            settings.setVjoyResult(false);
            settings.setVjoyResult(false);
            return false;
        }
    }
    else {
        text(L"ERROR: vJoy device %d status is %d", vjDev, vjdStatus);
        vJoyReady = false;
        settings.setVjoyResult(false);
        return false;
    }

    vjButtons = GetVJDButtonNumber(vjDev);
    vjPov = GetVJDContPovNumber(vjDev);
    vjPov += GetVJDDiscPovNumber(vjDev);

    text(L"Acquired vJoy device %d", vjDev);
    ResetVJD(vjDev);
    vJoyReady = true;
    settings.setVjoyResult(true);

    return true;

}

bool initVoy2() {

    return true;

}

void initAll() {

    initVJD();
    initDirectInput();

}

void releaseAll() {

    releaseDirectInput();


    if (simHubUpdate)    UnmapViewOfFile(simHubUpdate);
    if (simHubChangeRequest) UnmapViewOfFile(simHubChangeRequest);
    if (simHubUpdateMapHnd)    CloseHandle(simHubUpdateMapHnd);
    if (simHubChangeRequestMapHnd) CloseHandle(simHubChangeRequestMapHnd);

    RelinquishVJD(vjDev);


    irsdk_shutdown();
    CloseHandle(wheelEvent);
    CloseHandle(ffbEvent);

    text(L"releaseAll: Shutting down");

}
