-- i2c_scan.lua — scan the I2C bus and print devices found.
--
-- The I2C bus is automatically initialized on first call to any pm.i2c.*
-- function using the device's PIN_I2C_SDA / PIN_I2C_SCL build flags.

print("[I2C] Scanning bus...")
local addrs = pm.i2c.scan()

if #addrs == 0 then
    print("[I2C] No devices responded.")
    print("      Check pull-ups, connections, and that the bus is live.")
    return
end

print(string.format("[I2C] %d device(s) found:", #addrs))
for _, a in ipairs(addrs) do
    -- Common device hints
    local hint = ""
    if     a == 0x18 then hint = "  (ES8311 codec / LIS3DH accel)"
    elseif a == 0x1A then hint = "  (ES8311 codec alt addr)"
    elseif a == 0x38 then hint = "  (FT6336G capacitive touch)"
    elseif a == 0x3C then hint = "  (SSD1306 OLED)"
    elseif a == 0x40 then hint = "  (PCA9685 / INA219)"
    elseif a == 0x5D then hint = "  (GT911 touch)"
    elseif a == 0x68 then hint = "  (DS3231 / MPU6050 / ICM-20948)"
    elseif a == 0x71 then hint = "  (TCA8418 keypad / PCA9554)"
    elseif a == 0x76 then hint = "  (BME280 / BMP280)"
    elseif a == 0x77 then hint = "  (BME280 alt addr)"
    end
    print(string.format("  0x%02X  (%d)%s", a, a, hint))
end
