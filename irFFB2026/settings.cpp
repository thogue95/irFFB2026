#include "settings.h"
#include <fstream>
#include <iostream>
#include <string>
#include <cmath>




HKEY Settings::getSettingsRegKey() {

    HKEY key;
    if (
        RegCreateKeyExW(
            HKEY_CURRENT_USER, SETTINGS_KEY, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &key, nullptr
        )
        )
        return NULL;
    return key;

}

LSTATUS Settings::setRegSetting(HKEY key, wchar_t* name, int val) {

    DWORD sz = sizeof(int);
    return RegSetValueExW(key, name, 0, REG_DWORD, (BYTE*)&val, sz);

}

LSTATUS Settings::setRegSetting(HKEY key, wchar_t* name, float val) {

    DWORD sz = sizeof(float);
    return RegSetValueExW(key, name, 0, REG_DWORD, (BYTE*)&val, sz);

}


LSTATUS Settings::setRegSetting(HKEY key, wchar_t* name, bool val) {

    DWORD sz = sizeof(DWORD);
    DWORD dw = val ? 1 : 0;
    return RegSetValueExW(key, name, 0, REG_DWORD, (BYTE*)&dw, sz);

}

int Settings::getRegSetting(HKEY key, wchar_t* name, int def) {

    int val;
    DWORD sz = sizeof(int);

    if (RegGetValue(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &val, &sz))
        return def;
    else
        return val;

}

float Settings::getRegSetting(HKEY key, wchar_t* name, float def) {

    float val;
    DWORD sz = sizeof(float);

    if (RegGetValue(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &val, &sz))
        return def;
    else
        return val;

}

bool Settings::getRegSetting(HKEY key, wchar_t* name, bool def) {

    DWORD val, sz = sizeof(DWORD);

    if (RegGetValue(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &val, &sz))
        return def;
    else
        return val > 0;

}

Settings::Settings() {
    memset(ffdevices, 0, MAX_FFB_DEVICES * sizeof(GUID));
    ffdeviceIdx = 0;
    devGuid = GUID_NULL;
}

void Settings::setDevWnd(HWND wnd) { devWnd = wnd; }


void Settings::setFfbWnd(HWND wnd) {
    ffbWnd = wnd;

    // Clear any existing items
    SendMessage(ffbWnd, CB_RESETCONTENT, 0, 0);

    // Always add non-vJoy modes (game modes, 360 Hz, etc.)
    SendMessage(ffbWnd, CB_ADDSTRING, 0, (LPARAM)L"irFFB FFB - 360 Smoothing");
    SendMessage(ffbWnd, CB_ADDSTRING, 0, (LPARAM)L"irFFB FFB - 720 Smoothing");

    // Only add vJoy-dependent modes if vJoy is enabled
    if (vJoyResult) {
        SendMessage(ffbWnd, CB_ADDSTRING, 0, (LPARAM)L"Game FFB - 360 Smoothing");
        SendMessage(ffbWnd, CB_ADDSTRING, 0, (LPARAM)L"Game FFB - 720 Smoothing");
    }


    SendMessage(ffbWnd, CB_SETCURSEL, 0, 0);

    // Update internal ffbType to match selection
    ffbType = 0;  // or read from registry/generic if you want persistence
}

void Settings::setMaxWnd(sWins_t* wnd) {
    maxWnd = wnd;
    SendMessage(maxWnd->trackbar, TBM_SETRANGE, TRUE, MAKELPARAM(MIN_MAXFORCE, MAX_MAXFORCE));
}

void Settings::setBumpsWnd(sWins_t* wnd) { bumpsWnd = wnd; }

void Settings::setDampingWnd(sWins_t* wnd) { dampingWnd = wnd; }



void Settings::setFFBEffectsLevelWnd(sWins_t* wnd) {
    FFBEffectsLevelWnd = wnd;
    SendMessage(FFBEffectsLevelWnd->trackbar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
}



void Settings::setUseDDWheelWnd(HWND wnd) { useDDWheelWnd = wnd; }

void Settings::setAutoTuneWnd(HWND wnd) { autoTuneWnd = wnd; }

//void Settings::setUse360Wnd(HWND wnd) { use360Wnd = wnd; }

// void Settings::setCarSpecificWnd(HWND wnd) { carSpecificWnd = wnd; }

void Settings::setStartMinimisedWnd(HWND wnd) { startMinimisedWnd = wnd; }
void Settings::setDebugWnd(HWND wnd) { debugWnd = wnd; }

void Settings::clearFfbDevices() {
    memset(ffdevices, 0, sizeof(ffdevices));
    ffdeviceIdx = 0;
    SendMessage(devWnd, CB_RESETCONTENT, 0, 0);
}

void Settings::addFfbDevice(GUID dev, const wchar_t* name) {

    if (ffdeviceIdx == MAX_FFB_DEVICES)
        return;
    ffdevices[ffdeviceIdx++] = dev;
    SendMessage(devWnd, CB_ADDSTRING, 0, LPARAM(name));
    if (devGuid == dev)
        setFfbDevice(ffdeviceIdx - 1);

}

void Settings::setFfbDevice(int idx) {
    if (idx >= ffdeviceIdx)
        return;
    SendMessage(devWnd, CB_SETCURSEL, idx, 0);
    devGuid = ffdevices[idx];
    initDirectInput();
}

bool Settings::isFfbDevicePresent() {
    for (int i = 0; i < ffdeviceIdx; i++)
        if (ffdevices[i] == devGuid)
            return true;
    return false;
}

void Settings::setFfbType(int type) {
    if (type >= FFBTYPE_UNKNOWN)
        return;

    ffbType = type;
    SendMessage(ffbWnd, CB_SETCURSEL, ffbType, 0);


}




bool Settings::setMaxForce(int max, HWND wnd) {
    if (max < MIN_MAXFORCE || max > MAX_MAXFORCE)
        return false;
    maxForce = max;
    cachedHalfMaxForce = (float)max / 2.0f;  // cache it here

    if (wnd != maxWnd->trackbar)
        SendMessage(maxWnd->trackbar, TBM_SETPOS, TRUE, maxForce);
    if (wnd != maxWnd->value) {
        swprintf_s(strbuf, L"%d", max);
        SendMessage(maxWnd->value, WM_SETTEXT, NULL, LPARAM(strbuf));
    }
    scaleFactor = (float)DI_MAX / maxForce;
    irsdk_broadcastMsg(
        irsdk_BroadcastFFBCommand, irsdk_FFBCommand_MaxForce, (float)maxForce
    );
    return true;
}

bool Settings::setBumpsFactor(float factor, HWND wnd) {
    if (factor < 0.0f || factor > 100.0f)
        return false;

    bumpsFactor = factor;

    if (wnd != bumpsWnd->trackbar)
        SendMessage(bumpsWnd->trackbar, TBM_SETPOS, TRUE, (int)factor);
    if (wnd != bumpsWnd->value) {
        swprintf_s(strbuf, L"%.1f", factor);
        SendMessage(bumpsWnd->value, WM_SETTEXT, NULL, LPARAM(strbuf));
    }
    return true;
}

bool Settings::setDampingFactor(float factor, HWND wnd) {
    if (factor < 0.0f || factor > 100.0f)
        return false;
    dampingFactor = factor;
    if (wnd != dampingWnd->trackbar)
        SendMessage(dampingWnd->trackbar, TBM_SETPOS, TRUE, (int)factor);
    if (wnd != dampingWnd->value) {
        swprintf_s(strbuf, L"%.1f", factor);
        SendMessage(dampingWnd->value, WM_SETTEXT, NULL, LPARAM(strbuf));
    }
    return true;
}



bool Settings::setFFBEffectsLevel(float factor, HWND wnd) {
    FFBEffectsLevel =  (float)factor;

    // Recompute cached values only when slider actually changes
    float newNorm = factor / 100.0f;
    if (newNorm != cachedNormLevel) {          // only if changed
        cachedNormLevel = newNorm;
        float power = 1.6f;
        cachedCurvedLevel = powf(cachedNormLevel, power);
        cachedOverSteerFactor = cachedCurvedLevel * 1.2f;
        cachedUnderSteerFactor = cachedCurvedLevel * 1.4f;
    }


    // Sync UI if not from this control
    if (wnd != FFBEffectsLevelWnd->trackbar)
        SendMessage(FFBEffectsLevelWnd->trackbar, TBM_SETPOS, TRUE, (int)(factor));  // Scale to 0–100
    if (wnd != FFBEffectsLevelWnd->value) {
        swprintf_s(strbuf, L"%.1f", factor);  // Display as 0–100
        SendMessage(FFBEffectsLevelWnd->value, WM_SETTEXT, NULL, LPARAM(strbuf));
    }
    // Save to INI or reg if needed
    return true;
}






void Settings::setUseDDWheel(bool set) {
    useDDWheel = set;
    SendMessage(useDDWheelWnd, BM_SETCHECK, set ? BST_CHECKED : BST_UNCHECKED, NULL);
}

void Settings::setAutoTune(bool set) {
    autoTune = set;
    SendMessage(autoTuneWnd, BM_SETCHECK, set ? BST_CHECKED : BST_UNCHECKED, NULL);
}

void Settings::setVjoyResult(bool set) {

    vJoyResult = set;

}


void Settings::setStartMinimised(bool minimised) {
    startMinimised = minimised;
    SendMessage(startMinimisedWnd, BM_SETCHECK, minimised ? BST_CHECKED : BST_UNCHECKED, NULL);
}

void Settings::setDebug(bool enabled) {
    debug = enabled;
    SendMessage(debugWnd, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, NULL);
}



void Settings::readRegSettings(char* car, char* track) {
    wchar_t dguid[GUIDSTRING_MAX];
    DWORD dgsz = sizeof(dguid);
    HKEY key = getSettingsRegKey();
    if (key == NULL) {
        setStartMinimised(false);
        return;
    }
    if (!RegGetValue(key, nullptr, L"device", RRF_RT_REG_SZ, nullptr, dguid, &dgsz))
        if (FAILED(IIDFromString(dguid, &devGuid)))
            devGuid = GUID_NULL;
    setStartMinimised(getRegSetting(key, L"startMinimised", false));

    // Car-specific is now always on – load if car/track available
    if (car && car[0] != 0 && track && track[0] != 0) {
        readSettingsForCar(car, track);
    }

    RegCloseKey(key);
}

void Settings::readGenericSettings() {

    wchar_t dguid[GUIDSTRING_MAX];
    DWORD dgsz = sizeof(dguid);
    HKEY key = getSettingsRegKey();

    if (key == NULL) {
        setFfbType(FFBTYPE_GAME_360);
        setMaxForce(30, (HWND)-1);
        setBumpsFactor(5.0f, (HWND)-1);
        setDampingFactor(5.0f, (HWND)-1);
        setFFBEffectsLevel(50.0f, (HWND)-1);
        setAutoTune(true);
       


        return;
    }

    setFfbType(getRegSetting(key, L"ffb", FFBTYPE_IRFFB_360));

    setMaxForce(getRegSetting(key, L"maxForce", 30), (HWND)-1);
    setDampingFactor(getRegSetting(key, L"dampingFactor", 5.0f), (HWND)-1);
    setBumpsFactor(getRegSetting(key, L"bumpsFactor", 5.0f), (HWND)-1);
    setFFBEffectsLevel(getRegSetting(key, L"FFBEffectsLevel", 50.0f), (HWND)-1);
    setAutoTune(getRegSetting(key, L"autoTune", true));
    
   
    RegCloseKey(key);

}

void Settings::writeRegSettings() {

    wchar_t* guid;
    int len;
    HKEY key = getSettingsRegKey();

    if (key == NULL)
        return;

    if (SUCCEEDED(StringFromCLSID(devGuid, (LPOLESTR*)&guid))) {
        len = (lstrlenW(guid) + 1) * sizeof(wchar_t);
        RegSetValueEx(key, L"device", 0, REG_SZ, (BYTE*)guid, len);
    }


    setRegSetting(key, L"startMinimised", getStartMinimised());

    RegCloseKey(key);

}





void Settings::writeGenericSettings() {

    HKEY key = getSettingsRegKey();

    if (key == NULL)
        return;

    setRegSetting(key, L"ffb", ffbType);
    setRegSetting(key, L"maxForce", maxForce);
    setRegSetting(key, L"dampingFactor", dampingFactor);
    setRegSetting(key, L"bumpsFactor",bumpsFactor);
    setRegSetting(key, L"FFBEffectsLevel", FFBEffectsLevel);
    setRegSetting(key, L"useDDWheel", useDDWheel);
    setRegSetting(key, L"autoTune", autoTune);




    RegCloseKey(key);

}

void Settings::readSettingsForCar(char* car, char* track) {

    PWSTR path = getIniPath();

    if (path == nullptr) {
        text(L"Failed to locate documents folder, can't read ini");
        return;
    }

    std::ifstream iniFile(path);
    std::string line;



    char carName[MAX_CAR_NAME];
    char trackName[MAX_TRACK_NAME];
    int type = 0, max = 30, useDDwheel = 0, autoTuneInt = 0;
    float  bumps = 5.0f, damping = 5.0f, FFBEffectsLevel = 50.f;
    
    int foundCarTrack = 0;


    memset(carName, 0, sizeof(carName));
    memset(trackName, 0, sizeof(trackName)); //not sure if needed

    while (std::getline(iniFile, line)) {
        if (
            sscanf_s(
                line.c_str(), INI_SCAN_FORMAT,

                carName, sizeof(carName), trackName, sizeof(trackName),
                &type, &max, &damping, &bumps, &FFBEffectsLevel, &useDDwheel, &autoTuneInt
            ) < 9
            ) {


            
          //  text(L"Reading Settings: Read scan <8");
            continue;

        } //end if scan

  //      text(L"Reading Settings:  Looking for car %s", carName);
  //      text(L"Reading Settings: Looking for track %s", trackName);

  //      text(L"Reading Settings:  scanned in car %s", carName);
  //      text(L"Reading Settings: scanned in track %s", trackName);

        if ((strcmp(carName, car) == 0) && (strcmp(trackName, track) == 0)) {
     //       text(L"!!Found Car & Track Combo!!");
     //       text(L"Reading Settings:  scanned in car %s", carName);
      //     text(L"Reading Settings: scanned in track %s", trackName);
            foundCarTrack = 1;

                break;
        }

    }  //end while



    if (foundCarTrack == 0) {
        text(L"Car and track not found. Loading generic settings");
        
        goto DONE;

    }

 //  text(L"Pre-load -Found Car and Track = %d", foundCarTrack);
 //  text(L"readSettings: Loading settings for car %s", car);
 //  text(L"readSettings: scanned in car %s", carName);
 //  text(L"readSettings: Loading settings for track %s", track);
 //  text(L"readSettings: scanned in track %s", trackName);


    //If vJoy is not ready but reading in a mode that requires vjoy, setting irFFB mode 360
    if (vJoyResult = true) {
           setFfbType(type); 
    }
    else {
        setFfbType(FFBTYPE_IRFFB_360);
    }

 
    setMaxForce(max, (HWND)-1);    
    setDampingFactor(damping, (HWND)-1);
    setBumpsFactor(bumps, (HWND)-1);
    setFFBEffectsLevel(FFBEffectsLevel, (HWND)-1);
    setUseDDWheel(useDDwheel);
    setAutoTune((int)autoTuneInt);

  
   
    foundCarTrack = 0;
DONE:
    iniFile.close();
    delete[] path;

}

void Settings::writeSettingsForCar(char* car,  char* track) {

    PWSTR path = getIniPath();

    if (path == nullptr) {
        text(L"Failed to locate documents folder, can't write ini");
        return;
    }

    PWSTR tmpPath = new wchar_t[lstrlen(path) + 5];
    lstrcpy(tmpPath, path);
    lstrcat(tmpPath, L".tmp");

    std::ifstream iniFile(path);
    std::ofstream tmpFile(tmpPath);
    std::string line;


    char carName[MAX_CAR_NAME], buf[256];
    char trackName[MAX_TRACK_NAME];

    int type = 0, max = 30, useDDwheel = 0, autoTuneInt = 0;
    float bumps = 5.0f, damping = 5.0f, FFBEffectsLevel = 50.0f;


    bool written = false, iniPresent = iniFile.good();

    text(L"Saving car settings");
    //text(L"Writing settings for track %s", track);
    

    memset(carName, 0, sizeof(carName));
    memset(trackName, 0, sizeof(trackName));

    while (std::getline(iniFile, line)) {

        if (line.length() > 255)
            continue;

        
      
        if (

            sscanf_s(
                line.c_str(), INI_SCAN_FORMAT,
                carName, sizeof(carName), trackName, sizeof(trackName), 
                &type, &max, &damping, &bumps, &FFBEffectsLevel, &useDDwheel, &autoTuneInt
                 
            ) < 9 
            

            ) {
            //here we are copying non-car text back to file
           // text(L"Scan Count < 8");
            strcpy_s(buf, line.c_str());
            writeWithNewline(tmpFile, buf);
            continue;
        }
        //Save for future debug
        //We have a car line.  Now check for match
 //       text(L"Write Settings: scanned car -%s-", carName);
 //       text(L"Write Settings: Looking for car -%s-", car);
 //       text(L"Write Settings: scanned track -%s-", trackName);
 //       text(L"Write Settings: Looking for track -%s-", track);





        if ((strcmp(carName, car) != 0) || (strcmp(trackName, track) !=0)) {
           // text(L"Write Settings: Could not find existing car and track - writing new line");
            strcpy_s(buf, line.c_str());
            writeWithNewline(tmpFile, buf);
            continue;
        }

        sprintf_s(
            buf, INI_PRINT_FORMAT,
            car, track, ffbType, maxForce, dampingFactor, bumpsFactor, getFFBEffectsLevel(), useDDWheel, (int)autoTune
            );

        writeWithNewline(tmpFile, buf);
        written = true;
    }

    if (written) {
        //text(L"Write Settings: Appending to existing file ");
        goto MOVE;
    }


    if (!iniPresent) {
        sprintf_s(buf, "Car:Track:FFB Type: Max Force: Damping:Bumps: FFB Effects: Use DD Wheel: Auto Tune \r\n\r\n");
        tmpFile.write(buf, strlen(buf));


        sprintf_s(buf, "ffbType                   | 0 = irFFB, 1 = irFFB-720, 2 = Game-360, 3 = Game-720\r\n");
        tmpFile.write(buf, strlen(buf));

        sprintf_s(buf, "Max Force                 | min = %d, max = %d\r\n", MIN_MAXFORCE, MAX_MAXFORCE);
        tmpFile.write(buf, strlen(buf));

        sprintf_s(buf, "Damping                   | min = 0, max = 100\r\n");
        tmpFile.write(buf, strlen(buf));

        sprintf_s(buf, "Bumps                     | min = 0, max = 100\r\n");
        tmpFile.write(buf, strlen(buf));

        sprintf_s(buf, "FFB Effects   | min = 0, max = 100\r\n");
        tmpFile.write(buf, strlen(buf));

        sprintf_s(buf, "Use DD Wheel              | false = 0, true = 1\r\r\n");
        tmpFile.write(buf, strlen(buf));

        sprintf_s(buf, "Auto Tune              | false = 0, true = 1\r\r\n");
        tmpFile.write(buf, strlen(buf));



 
    }

    sprintf_s(
        buf, INI_PRINT_FORMAT,
        car, track, ffbType, maxForce, dampingFactor, bumpsFactor, getFFBEffectsLevel(), useDDWheel, (int)autoTune
         
    );


  

    writeWithNewline(tmpFile, buf);

MOVE:
    iniFile.close();
    tmpFile.close();

    if (!MoveFileEx(tmpPath, path, MOVEFILE_REPLACE_EXISTING))
        text(L"Failed to update ini file, error %d", GetLastError());

    delete[] path;
    delete[] tmpPath;

}



wchar_t* Settings::ffbTypeString(int type) {
    switch (type) {
    case FFBTYPE_IRFFB_360:             return L"irFFB FFB - 360 Smoothing";
    case FFBTYPE_IRFFB_720:      return L"irFFB FFB - 720 Smoothing";
    case FFBTYPE_GAME_360:     return L"Game FFB - 360 Smoothing";
    case FFBTYPE_GAME_720: return L"Game FFB - 720 Smoothing";
    default:                        return L"Unknown FFB type";
    }
}



PWSTR Settings::getIniPath() {

    PWSTR docsPath;
    wchar_t* path;

    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &docsPath) != S_OK)
        return nullptr;

    path = new wchar_t[lstrlen(docsPath) + lstrlen(INI_PATH) + 1];

    lstrcpyW(path, docsPath);
    lstrcatW(path, INI_PATH);
    CoTaskMemFree(docsPath);

    return path;

}

PWSTR Settings::getLogPath() {

    PWSTR docsPath;
    wchar_t buf[64];
    wchar_t* path;
    SYSTEMTIME lt;

    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &docsPath) != S_OK)
        return nullptr;

    GetLocalTime(&lt);

    lstrcpyW(buf, L"\\irFFB2026-");
    int len = wcslen(buf) * sizeof(wchar_t);
    StringCbPrintf(
        buf + wcslen(buf), sizeof(buf) - len, L"%d-%02d-%02d-%02d-%02d-%02d.log",
        lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond
    );

    path = new wchar_t[lstrlen(docsPath) + lstrlen(buf) + 1];

    lstrcpyW(path, docsPath);
    lstrcatW(path, buf);
    CoTaskMemFree(docsPath);

    return path;

}

void Settings::writeWithNewline(std::ofstream& file, char* buf) {
    int len = strlen(buf);
    buf[len] = '\n';
    file.write(buf, len + 1);
}

void Settings::bumpFFBType(int bumpValue) {

 int tempType = getFfbType();

        if (bumpValue == 1) {

            if (vJoyResult) {
               
                if (tempType < FFBTYPE_GAME_720) {
                    setFfbType(tempType + 1);
                }
                else {
                    setFfbType(FFBTYPE_IRFFB_360);
                }

            }
            else {
                if (tempType < FFBTYPE_IRFFB_720) {
                    setFfbType(tempType + 1);
                }
                else {
                    setFfbType(FFBTYPE_IRFFB_360);
                }


            }


        } 




    return;
}


void Settings::bumpMaxForce(int bumpValue) {

    if (bumpValue == 1) {
        int tempType = getMaxForce();
        if (tempType < MAX_MAXFORCE) setMaxForce((tempType + 1), (HWND)-1);
    }

    if (bumpValue == -1) {
        int tempType = getMaxForce();
        if (tempType > 0) setMaxForce((tempType - 1), (HWND)-1);
    }
    return;
}


void Settings::bumpFFBEffectsLevel(int bumpValue) {


    if (bumpValue == 1) {
        float tempType = getFFBEffectsLevel();
        if (tempType < MAX_EFFECTS_STRENGTH) setFFBEffectsLevel((tempType + 1), (HWND)-1);

    }

    if (bumpValue == -1) {
        float tempType = getFFBEffectsLevel();
        if (tempType > 0) setFFBEffectsLevel((tempType - 1), (HWND)-1);
    }
    return;
}



void Settings::bumpDamping(int bumpValue) {

    if (bumpValue == 1) {
        float tempType = getDampingFactor();
        if (tempType < MAX_DAMPING) setDampingFactor((tempType + 1), (HWND)-1);
    }

    if (bumpValue == -1) {
        float tempType = getDampingFactor();
        if (tempType > 0) setDampingFactor((tempType - 1), (HWND)-1);
    }
    return;
}
void Settings::bumpBumps(int bumpValue) {

    if (bumpValue == 1) {
        float tempType = getBumpsFactor();
        if (tempType < MAX_BUMPS) setBumpsFactor((tempType + 1), (HWND)-1);
    }

    if (bumpValue == -1) {
        float tempType = getBumpsFactor();
        if (tempType > 0) setBumpsFactor((tempType - 1), (HWND)-1);
    }
    return;
}

