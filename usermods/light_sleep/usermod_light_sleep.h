#pragma once

#include "wled.h"
//#include "driver/rtc_io.h"

#ifdef ESP8266
#error The "Light Sleep" usermod does not support ESP8266
#endif

#define LIGHTSLEEP_STARTUPDELAY 3000 // delay in ms before entering light sleep after bootup

class LightSleepUsermod : public Usermod {

  private:
    bool enabled = false;
    bool didSleep = false;
    static const char _name[];
    static const char _enabled[];

  public:
    inline void enable(bool enable) { enabled = enable; } // Enable/Disable the usermod
    inline bool isEnabled() { return enabled; } //Get usermod enabled/disabled state

    // setup is called at boot (or in this case after every exit of sleep mode)
    void setup() { } // nothing to set-up

    void loop() {
      if (!enabled) // disabled
        return;

      if(!offMode) {
        if(didSleep) { // after wake-up, set wifi-sleep back to user setting
          //WiFi.setSleep(!noWifiSleep);
          //WLED::instance().initConnection(); // re-init connection (sets wifi sleep mode to user value, if set manually here, it can sometimes cause wifi issues)
          //TODO: initConnection is quite slow, after wake-up, UI is unresponsive for some time... maybe do a quick re-connect right here?
          //WiFi.disconnect();           // disconnect from wifi
          WiFi.setSleep(!noWifiSleep); // set user setting
         // WiFi.reconnect();            // reconnect to wifi
          didSleep = false;
        }
        //if(!WiFi.isConnected()) // TODO: this only checks STA mode, need to handle AP mode as well, also, only check like every 5 seconds or so -> this should actually be done in handle connection?
          //WLED::instance().initConnection(); // re-init connection (sets wifi sleep mode to user value)  TODO: is there a better, faster way to check and re-connect?
        return;
      }
      pinMode(8, OUTPUT);  // DEBUG output -> led on GPIO8 (C3 supermini)
      digitalWrite(8, HIGH);
      delay(50);
      digitalWrite(8, LOW);
      // if we are in off mode, enable wifi sleep and enter light sleep
      if(millis() > LIGHTSLEEP_STARTUPDELAY) {
        if(!didSleep) // first time sleep call
          WiFi.setSleep(true); // save power
        esp_sleep_enable_timer_wakeup(1000000);  // fallback: wake up every second
        esp_sleep_enable_wifi_wakeup(); // note: not avialable on classic ESP32

         //TODO: how to handle local time? needs a timer to keep track of time and update millis() after wake-up?

        // enable wakeup on any configured button pins
        for(int i = 0; i < WLED_MAX_BUTTONS; i++) {
          if(btnPin[i] >= 0 && buttonType[i] > BTN_TYPE_RESERVED && buttonType[i] < BTN_TYPE_TOUCH) { // TODO: add touch button support for S3 and S2
            #if defined(CONFIG_IDF_TARGET_ESP32C3) // ESP32 C3
            bool wakeWhenHigh = digitalRead(btnPin[i]) == LOW; // button is currently low, wake when high (and vice versa)
            if(wakeWhenHigh)
              gpio_wakeup_enable((gpio_num_t)btnPin[i], GPIO_INTR_HIGH_LEVEL);
            else
              gpio_wakeup_enable((gpio_num_t)btnPin[i], GPIO_INTR_LOW_LEVEL);

            esp_sleep_enable_gpio_wakeup();
            #else // S2, S3
            bool wakeWhenHigh = digitalRead(btnPin[i]) == LOW; // button is currently low, wake when high (and vice versa)
            if(wakeWhenHigh)
              esp_sleep_enable_ext0_wakeup((gpio_num_t)btnPin[i], HIGH); // TODO: use ext1 for touch pins
            else
              esp_sleep_enable_ext0_wakeup((gpio_num_t)btnPin[i], LOW);
            #endif
          }
        }
        esp_light_sleep_start();  // Enter light sleep
        didSleep = true;
        if(!WiFi.isConnected()) { // TODO: this only checks STA mode, need to handle AP mode as well 
          WLED::instance().initConnection(); // re-init connection (sets wifi sleep mode to user value)  TODO: is there a better, faster way to check and re-connect?
          didSleep = false; // reset sleep flag to re-enable wifi sleep
        }
        WLED::instance().handleConnection(); // check wifi state, re-connect if necessary
      }
    }
    //void connected() {} //unused, this is called every time the WiFi is (re)connected

    void addToConfig(JsonObject& root) override
    {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
    }

    bool readFromConfig(JsonObject& root) override
    {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled);
      return configComplete;
    }

    /*
     * getId() allows you to optionally give your V2 usermod an unique ID (please define it in const.h!).
     * This could be used in the future for the system to determine whether your usermod is installed.
     */
    uint16_t getId() {
        return USERMOD_ID_LIGHT_SLEEP;
    }

};

// add more strings here to reduce flash memory usage
const char LightSleepUsermod::_name[]    PROGMEM = "LightSleep";
const char LightSleepUsermod::_enabled[] PROGMEM = "enabled";