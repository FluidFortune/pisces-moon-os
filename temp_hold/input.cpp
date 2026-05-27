/**
 * PISCES MOON OS — T-DECK PLUS INPUT ENGINE
 * Hardware: LilyGo T-Deck Plus
 * Input: BLE Gamepad (Primary) + Hardware Trackball (Fallback)
 */

#include "input.h"
#include "trackball.h"
#include <NimBLEDevice.h>

InputEvent current_event = {InputEvent::NONE};
bool ble_gamepad_connected = false;

static NimBLEClient* pClient  = nullptr;
static NimBLERemoteCharacteristic* pHIDCharacteristic = nullptr;

// ─────────────────────────────────────────────
//  HID REPORT DECODER
// ─────────────────────────────────────────────
void notifyCB(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    if (length < 5) return; 

    uint8_t hat = pData[2] & 0x0F; 
    uint8_t buttons = pData[3];
    uint8_t sys_buttons = pData[4];

    current_event.type = InputEvent::NONE; 

    // 1. D-PAD (Hat Switch)
    if      (hat == 0 || hat == 1 || hat == 7) current_event.type = InputEvent::UP;
    else if (hat == 3 || hat == 4 || hat == 5) current_event.type = InputEvent::DOWN;
    else if (hat == 6)                         current_event.type = InputEvent::LEFT;
    else if (hat == 2)                         current_event.type = InputEvent::RIGHT;

    // 2. ACTION BUTTONS
    if (buttons & 0x01)     current_event.type = InputEvent::SELECT; // 'A' 
    if (buttons & 0x02)     current_event.type = InputEvent::BACK;   // 'B' 
    
    // 3. SYSTEM BUTTONS
    if (sys_buttons & 0x20) current_event.type = InputEvent::MENU;   // 'Start' 
}

// ─────────────────────────────────────────────
//  BLE CONNECTION MANAGER
// ─────────────────────────────────────────────
class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pclient) {
        Serial.println("[BLE] 8BitDo Zero 2 Connected!");
        ble_gamepad_connected = true;
    }
    void onDisconnect(NimBLEClient* pclient) {
        Serial.println("[BLE] 8BitDo Zero 2 Disconnected.");
        ble_gamepad_connected = false;
    }
};

void connectToGamepad(NimBLEAdvertisedDevice* device) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new ClientCallbacks());

    if (pClient->connect(device)) {
        NimBLERemoteService* pService = pClient->getService("1812"); 
        if (pService != nullptr) {
            pHIDCharacteristic = pService->getCharacteristic("2a4d");
            if (pHIDCharacteristic != nullptr && pHIDCharacteristic->canNotify()) {
                pHIDCharacteristic->subscribe(true, notifyCB);
                Serial.println("[BLE] Subscribed to HID Reports.");
            }
        }
    }
}

class ScanCallbacks: public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        if (advertisedDevice->getName().find("8BitDo") != std::string::npos || 
            advertisedDevice->getName().find("Zero 2") != std::string::npos) {
            Serial.println("[BLE] Found Zero 2! Halting scan...");
            NimBLEDevice::getScan()->stop();
            connectToGamepad(advertisedDevice);
        }
    }
};

// ─────────────────────────────────────────────
//  PUBLIC API
// ─────────────────────────────────────────────
void input_init() {
    Serial.println("[SYSTEM] Initializing BLE HID Host...");
    NimBLEDevice::init("PiscesMoon_Sentry");
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
    pScan->setActiveScan(true);
    pScan->start(5, false); 
    
    // Initialize the physical trackball hardware
    init_trackball(); 
}

InputEvent input_poll() {
    InputEvent temp = current_event;
    current_event.type = InputEvent::NONE; // Clear BLE queue
    
    // If the BLE gamepad didn't send anything this frame, check the trackball
    if (temp.type == InputEvent::NONE) {
        TrackballState tb = update_trackball();
        if (tb.x == -1) temp.type = InputEvent::LEFT;
        else if (tb.x ==  1) temp.type = InputEvent::RIGHT;
        else if (tb.y ==  1) temp.type = InputEvent::UP;
        else if (tb.y == -1) temp.type = InputEvent::DOWN;
    }

    return temp;
}