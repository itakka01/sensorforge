#include "drone_mode.h"
#include <Preferences.h>
#include <Arduino.h>
namespace { bool enabledFlag=false; }
void droneModeBegin(){
    Preferences prefs;
    if(prefs.begin("sf_drone", true)){
        enabledFlag=prefs.getBool("enabled",false);
        prefs.end();
    }
    Serial.printf("[DRONE] boot setting=%s\n",enabledFlag?"ON":"OFF");
}
bool droneModeEnabled(){return enabledFlag;}
bool droneModeSet(bool enabled){
    if(enabled==enabledFlag)return true;
    Preferences prefs;
    if(!prefs.begin("sf_drone",false))return false;
    size_t written=prefs.putBool("enabled",enabled);
    prefs.end();
    if(written!=1)return false;
    enabledFlag=enabled;
    Serial.printf("[DRONE] readiness=%s (persistent)\n",enabled?"ON":"OFF");
    return true;
}
