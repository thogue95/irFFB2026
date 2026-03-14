    #pragma once
#include "stdafx.h"
#include "irFFB2026.h"

class Settings {

public:

    static HKEY getSettingsRegKey();
    static LSTATUS setRegSetting(HKEY, wchar_t*, int);
    static LSTATUS setRegSetting(HKEY, wchar_t*, float);
    static LSTATUS setRegSetting(HKEY, wchar_t*, bool);
    static int getRegSetting(HKEY, wchar_t*, int);
    static float getRegSetting(HKEY, wchar_t*, float);
    static bool getRegSetting(HKEY, wchar_t*, bool);

    Settings();
    //void setOverSteerLabelWnd(HWND); //Not implemented yet

    void setDevWnd(HWND);
    HWND getDevWnd() { return devWnd; };
    void setFfbWnd(HWND);
    HWND getFfbWnd() { return ffbWnd; };

    void setMaxWnd(sWins_t*);
    sWins_t* getMaxWnd() { return maxWnd; };
    void setBumpsWnd(sWins_t*);
    sWins_t* getBumpsWnd() { return bumpsWnd; };
    void setDampingWnd(sWins_t*);
    sWins_t* getDampingWnd() { return dampingWnd; };





    void setFFBEffectsLevelWnd(sWins_t*);  
    sWins_t* getFFBEffectsLevelWnd() { return FFBEffectsLevelWnd; };


      
    void setUseDDWheelWnd(HWND);
    HWND getUseDDWheelWnd() { return useDDWheelWnd; };
    void setAutoTuneWnd(HWND);
    HWND getAutoTuneWnd() { return autoTuneWnd; };

  //  void setUse360Wnd(HWND);
  //  HWND getUse360Wnd() { return use360Wnd; };

  //  void setCarSpecificWnd(HWND);
  //  HWND getCarSpecificWnd() { return carSpecificWnd; };
  //  void setUseCarSpecific(bool, char*, char*);
  //  bool getUseCarSpecific() { return useCarSpecific; };


    void setStartMinimisedWnd(HWND);
    HWND getStartMinimisedWnd() { return startMinimisedWnd; };
    void setDebugWnd(HWND);
    HWND getDebugWnd() { return debugWnd; };

    void clearFfbDevices();
    void addFfbDevice(GUID dev, const wchar_t*);
    void setFfbDevice(int);
    bool isFfbDevicePresent();
    GUID getFfbDevice() { return devGuid; };
    void setFfbType(int);
    int getFfbType() { return ffbType; };

    void setVjoyResult(bool);



    bool setMaxForce(int, HWND);
    int getMaxForce() { return maxForce; };
    float getScaleFactor() { return scaleFactor; };
    float getNormalizedScaleFactor() { return (float)DI_MAX / ((float)maxForce * (IR_MAX / (float)DI_MAX)); }  // New: Normalizes with IR_MAX for raw signal

    bool setBumpsFactor(float, HWND);
    float getBumpsFactor() { return bumpsFactor; };
    bool setDampingFactor(float, HWND);
    float getDampingFactor() { return dampingFactor; };


    bool setFFBEffectsLevel(float, HWND);
    float getFFBEffectsLevel() { return FFBEffectsLevel; }


    void setAutoTune(bool);
    bool getAutoTune() { return autoTune; }

    void setUseDDWheel(bool);
    bool getUseDDWheel() { return useDDWheel; };


    void setStartMinimised(bool);
    bool getStartMinimised() { return startMinimised; };
    void setDebug(bool);
    bool getDebug() { return debug; };
    
 

    void readRegSettings(char*, char*);
    void readGenericSettings();
    void writeRegSettings();
    void writeGenericSettings();
    void readSettingsForCar(char*, char*);
    void writeSettingsForCar(char*, char*);
    PWSTR getLogPath();

    //The use of bump refers to increment decrement by 1.  Not related to Bumps in suspsension.
    void bumpMaxForce(int bumpValue);
    void bumpFFBType(int bumpValue);

    void bumpFFBEffectsLevel(int bumpValue);
    void bumpDamping(int bumpValue);
    void bumpBumps(int bumpValue); //silly name but we are bumping the Bumps setting

    //setting up for precomputing values to lower latency
    float cachedNormLevel = -1.0f;              // invalid sentinel
    float cachedCurvedLevel = 0.0f;
    float cachedOverSteerFactor = 0.0f;
    float cachedUnderSteerFactor = 0.0f;
    float cachedHalfMaxForce = 0.0f; // invalid initial value

private:
    HWND devWnd, ffbWnd, overSteerLabelWnd;  
    sWins_t* FFBEffectsLevelWnd, * minWnd, * maxWnd, * bumpsWnd, * dampingWnd;  
    HWND useDDWheelWnd, carSpecificWnd, autoTuneWnd;
    HWND startMinimisedWnd, debugWnd;
    int ffbType, ffdeviceIdx, maxForce; 

    bool vJoyResult = false;

    
    float FFBEffectsLevel, scaleFactor,  bumpsFactor, dampingFactor; 
    bool useCarSpecific, debug, autoFfbMode, useDDWheel, autoTune;
    bool startMinimised;
    GUID devGuid = GUID_NULL, ffdevices[MAX_FFB_DEVICES];
    wchar_t strbuf[64];
    HANDLE debugHnd;

    wchar_t* ffbTypeString(int);

    PWSTR getIniPath();
    void writeWithNewline(std::ofstream&, char*);





};