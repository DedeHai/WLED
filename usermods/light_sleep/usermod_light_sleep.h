#pragma once

#include "wled.h"
//#include "driver/rtc_io.h"
#include "esp_pm.h"
#ifdef ESP8266
#error The "Light Sleep" usermod does not support ESP8266
#endif

#define LIGHTSLEEP_STARTUPDELAY 3000 // delay in ms before entering light sleep after bootup

class LightSleepUsermod : public Usermod {

  private:
    bool enabled = false;
    bool skipSleep = false; // skips light sleep (set in UM config) in case of issues, still uses wifi-sleep and lower CPU clock in off mode
    bool didSleep = false;
    bool wakeUp = false;
    
    static const char _name[];
    static const char _enabled[];

  public:
    inline void enable(bool enable) { enabled = enable; } // Enable/Disable the usermod
    inline bool isEnabled() { return enabled; } //Get usermod enabled/disabled state

    // setup is called at boot (or in this case after every exit of sleep mode)
    void setup() { } // nothing to set-up

    void loop() {
      //delay(10);
      if (!enabled) // disabled
        return;

      if(!offMode) {
        if(didSleep) { // after wake-up, set wifi-sleep back to user setting
        
          #if defined(CONFIG_IDF_TARGET_ESP32C3) // ESP32 C3
            setCpuFrequencyMhz(160);      // set CPU frequency back to 160MHz
          #else
           esp_wifi_stop();
           //WiFi.disconnect(); // much slower than using stop and start
            setCpuFrequencyMhz(240);      // set CPU frequency back to 240MHz
           //  delay(1);
          if(noWifiSleep) {
            WiFi.setSleep(WIFI_PS_NONE); // disable wifi sleep again
            //delay(50); // not sure this is needed... can it be removed or reduced?
          }
          esp_wifi_start();
          WiFi.reconnect();
          uint32_t timestamp = millis();
           Serial.print("Waiting for WiFi to connect");
           int counter = 0;
          while(WiFi.isConnected() == false) {
            Serial.println(millis() - timestamp);
            delay(1);
            counter++;
            if(counter > 5000) {
             break;
            }
          }

          #endif

          

          didSleep = false;
        }
        //if(!WiFi.isConnected()) // TODO: this only checks STA mode, need to handle AP mode as well, also, only check like every 5 seconds or so -> this should actually be done in handle connection?
          //WLED::instance().initConnection(); // re-init connection (sets wifi sleep mode to user value)  TODO: is there a better, faster way to check and re-connect?
        return;
      }
      /*
      pinMode(8, OUTPUT);  // DEBUG output -> led on GPIO8 (C3 supermini)
      digitalWrite(8, HIGH);  // TODO: why is power consumption lower when using a delay here?
      delay(50);  // a delay is needed to properly handle wifi stuff, connection is lost if this is set lower than 50 ms
      digitalWrite(8, LOW);
      digitalWrite(7, HIGH); // debug, relay on
*/
      static int skipcounter = 0;

      // if we are in off mode, enable wifi sleep and spend some time in light sleep
      if(millis() > LIGHTSLEEP_STARTUPDELAY) { // wait a bit before going to sleep
        if(skipcounter > 0) {
          skipcounter--;
          return;
        }
        wakeUp = false;
        if(!didSleep) { // first time sleep call

         esp_wifi_stop(); // needed on ESP32: when changing CPU frequency, wifi connection is lost (known bug), stopping wifi reconnects much faster (in about 100ms) but all connections are lost
           //WiFi.disconnect(); // much slower than using stop and start
              setCpuFrequencyMhz(80); // slow down CPU to 80MHz to save power (80MHz is lowest possible frequency with wifi enabled)
           //  delay(1);
       //  if(WiFi.getSleep() != WIFI_PS_MIN_MODEM) { // check if wifi sleep is enabled, enable it if it is not
          WiFi.setSleep(WIFI_PS_MIN_MODEM); // save power by enabling wifi auto sleep (does nothing if it is already enabled)
          //delay(10); // make sure wifi is stable (not sure this is needed)
        //}
          esp_wifi_start();
          WiFi.reconnect();
        }
        




         //TODO: how to handle local time? needs a timer to keep track of time and update millis() after wake-up?
       // while(!wakeUp && offMode) {
          // enable wakeup on any configured button pins
          /*
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
          esp_sleep_enable_timer_wakeup(2000000);  // fallback: wake up every 2 seconds to check wifi
          esp_sleep_enable_wifi_wakeup(); // note: not avialable on classic ESP32 -> wakes up every beacon inverval
          if (!apActive && !skipSleep) // do not go to sleep in AP mode, it will disconnect
            esp_light_sleep_start();  // Enter light sleep  No light sleep: about 30mA, with light sleep: 15mA
          else
            skipcounter = 5000;
            */
          // we woke up, check if we should go back to sleep
          //check wakeup reason
          /*
          esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
          if(wakeup_reason == ESP_SLEEP_WAKEUP_WIFI) {
           delay(5); // process wifi stuff before going back to sleep
           wakeUp = true; //!offMode; // wake up if wifi changed the state
          }
          else
          {
              wakeUp = true; // wake up if button or timeout triggered and run the main loop
          }*/
/*
          if(WLED_WIFI_CONFIGURED) {
            if(WiFi.status() != WL_CONNECTED) { //not connected, reconnect
              //WiFi.reconnect();
              //WLED::instance().handleConnection(); // check wifi state, re-connect if necessary
              WLED::instance().initConnection();
              skipcounter = 2000; // run main loop a few times before going back to sleep (otherwise wifi breaks) 200 works for wifi but AP does not start, 2000 works with AP
              }*/
         // }

        //}
        didSleep = true;
        // note: when skipping the below lines, connection can be lost after some time so better keep this -> TODO: is there a better way? if wifi is down, it reboots when having this code
        // BUT: when not having it, connection is lost, ESPNow still works though so something is wrong with the wifi connection


       // if(!WiFi.isConnected()) { // TODO: this only checks STA mode, need to handle AP mode as well 
       // WLED::instance().initConnection(); // re-init connection (sets wifi sleep mode to user value)  TODO: is there a better, faster way to check and re-connect?
        //  didSleep = false; // reset sleep flag to re-enable wifi sleep if set by user
       // }
        //forceReconnect = true; //TODO: does this help with lost connection?
        //WLED::instance().handleConnection(); // check wifi state, re-connect if necessary
        
      }
    }
    //void connected() {} //unused, this is called every time the WiFi is (re)connected

    bool onEspNowMessage(uint8_t* sender, uint8_t* payload, uint8_t len) {
      wakeUp = true; // wake up on ESP-NOW message
      return false; // not used
    }
    


    void addToConfig(JsonObject& root) override
    {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
      top["skipSleep"] = skipSleep;
    }

    bool readFromConfig(JsonObject& root) override
    {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled);
      configComplete &= getJsonValue(top["skipSleep"], skipSleep, false);
      return configComplete;
    }

    /*
     * appendConfigData() is called when user enters usermod settings page
     * it may add additional metadata for certain entry fields (adding drop down is possible)
     * be careful not to add too much as oappend() buffer is limited to 3k
     */
    void appendConfigData() override
    {
      oappend(SET_F("addInfo('LightSleep:skipSleep',1,'(no LightSleep, only WiFiSleep)','');")); // first string is suffix, second string is prefix
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