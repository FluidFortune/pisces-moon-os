# scripts/ — Example Lua scripts

Copy these files to your device's SD card under `/scripts/` to use them.

After flashing pm_shell and connecting via USB serial, run any script with:

```lua
pm.dofile("/scripts/blink.lua")
```

## Files

| Script             | What it does                                          |
| ------------------ | ----------------------------------------------------- |
| `blink.lua`        | Toggles a GPIO 10 times. Demonstrates `pm.gpio.*`.    |
| `wifi_scan.lua`    | Scans WiFi and pretty-prints networks by RSSI.        |
| `i2c_scan.lua`     | Enumerates the I2C bus, prints addresses found.       |
| `audio_test.lua`   | Plays an ascending C-major scale via LEDC. Set the    |
|                    | `PIN` variable at the top to your device's amp pin.   |

## Writing your own

Scripts run in the same `lua_State` as the REPL, so anything you define
becomes available at the next `pm>` prompt. Globals persist; locals don't.

Example: a simple GPIO toggler you can call repeatedly:

```lua
-- save as /scripts/toggle.lua
function toggle(pin)
    pm.gpio.mode(pin, "output")
    pm.gpio.write(pin, 1)
    pm.delay(100)
    pm.gpio.write(pin, 0)
end
print("toggle() is now defined.")
```

After `pm.dofile("/scripts/toggle.lua")`, you can call `toggle(42)` from
the REPL.

## SD card layout convention

```
/scripts/                Lua scripts
/data/                   user data files
/logs/                   anything you write with pm.storage.write
```

No enforcement — these are just suggestions to keep the card tidy.
