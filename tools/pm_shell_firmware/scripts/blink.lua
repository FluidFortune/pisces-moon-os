-- blink.lua — toggle a GPIO 10 times.
--
-- Set PIN to a safe output on your device. Many devices have an LED
-- or backlight pin you can blink visibly. Examples:
--   T-Deck Plus     PIN = 42   (backlight — turns whole screen off/on)
--   T-LoRa Pager    PIN = 42   (lcd backlight)
--   Cardputer ADV   PIN = 38   (lcd backlight)
--   C28P            PIN = 45   (lcd backlight)
--   Maxine          PIN =  2   (rgb backlight)
--
-- Set to a known-safe output on YOUR device. The script defaults to
-- nil (no-op) so it can't damage anything if blindly invoked.

local PIN = nil   -- ← set this!

if PIN == nil then
    print("blink.lua: edit PIN at the top of the script first")
    return
end

print(string.format("Blinking GPIO %d 10 times...", PIN))
pm.gpio.mode(PIN, "output")
for i = 1, 10 do
    pm.gpio.write(PIN, 1)
    pm.delay(200)
    pm.gpio.write(PIN, 0)
    pm.delay(200)
end
print("Done.")
