-- audio_test.lua — play a C-major scale via LEDC tone generator.
--
-- This works on ANY device by directly driving a GPIO with PWM. No codec
-- assumptions. Set PIN to your speaker / amp input. On devices with a
-- codec (C28P ES8311) you'll need to additionally enable the amp by
-- toggling PIN_AUDIO_EN before this script will be audible.
--
-- Device suggestions:
--   T-Deck Plus     PIN =  6   (I2S DOUT pin doubles as a tone source
--                                only if the amp is enabled)
--   T-LoRa Pager    PIN = 45   (I2S DOUT to speaker)
--   Cardputer ADV   PIN = 46   (I2S DOUT)
--   C28P            PIN =  6   (I2S DOUT; also pm.gpio.write(1, 0) first
--                                to enable PIN_AUDIO_EN amp)
--   Maxine          (output-only, but speaker pinout unverified)

local PIN = nil   -- ← set this!

if PIN == nil then
    print("audio_test.lua: edit PIN at the top of the script first")
    return
end

-- Frequencies (Hz) for the C-major scale, C4 through C5.
local scale = {
    {note="C4", hz=262},
    {note="D4", hz=294},
    {note="E4", hz=330},
    {note="F4", hz=349},
    {note="G4", hz=392},
    {note="A4", hz=440},
    {note="B4", hz=494},
    {note="C5", hz=523},
}

print(string.format("Playing C-major scale on pin %d...", PIN))
for _, n in ipairs(scale) do
    print("  " .. n.note .. "  (" .. n.hz .. " Hz)")
    pm.audio.tone(PIN, n.hz, 300)
    pm.delay(50)
end
print("Done.")
