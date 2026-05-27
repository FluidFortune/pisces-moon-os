#include <Arduino_GFX_Library.h>
#include "system_ui.h"
#include "theme.h"
#include "touch.h"

extern Arduino_GFX *gfx;

// --- UNIVERSAL HEADER DRAW ---
void draw_os_header(const char* title) {
    // 1. Main header bar
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 24, 320, C_GREEN);
    
    // 2. App Title
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(1);
    gfx->print(title);

    // 3. The [X] Button Hitbox (30x24 pixels on the far right edge)
    // Using Red for the box and White for the X
    gfx->fillRect(290, 0, 30, 24, 0xF800); // C_RED / 0xF800
    gfx->setTextColor(0xFFFF);             // C_WHITE / 0xFFFF
    gfx->setCursor(300, 7);
    gfx->print("X");
}

// --- UNIVERSAL EXIT LISTENER ---
bool check_os_exit() {
    int16_t tx, ty;
    
    // If the screen is currently being touched...
    if (get_touch(&tx, &ty)) {
        
        // Check if the touch is within the top-right corner (X > 280, Y < 30)
        if (tx > 280 && ty < 30) {
            
            // Visual Feedback: Flash the button White with a Red X
            gfx->fillRect(290, 0, 30, 24, 0xFFFF); 
            gfx->setTextColor(0xF800);
            gfx->setCursor(300, 7);
            gfx->print("X");
            
            // Absolute Debounce: Wait for the user to lift their finger
            int16_t dumpX, dumpY;
            while(get_touch(&dumpX, &dumpY)) { 
                delay(10); 
                yield(); 
            }
            
            return true; // Signal the app's while(true) loop to break
        }
    }
    
    return false; // No exit command detected
}