-- wifi_scan.lua — scan and pretty-print WiFi networks.
--
-- Sorts by RSSI (strongest first), shows SSID, signal in dBm,
-- channel, and encryption type.

print("[WiFi] Scanning...")
local nets = pm.wifi.scan()
local count = #nets
print(string.format("[WiFi] %d networks found.", count))

if count == 0 then return end

-- Sort by RSSI descending
table.sort(nets, function(a, b) return a.rssi > b.rssi end)

print()
print(string.format("  %-32s %6s  %3s  %s",
                    "SSID", "RSSI", "CH", "ENC"))
print(string.rep("-", 60))
for _, n in ipairs(nets) do
    local ssid = n.ssid
    if ssid == "" then ssid = "<hidden>" end
    if #ssid > 32 then ssid = ssid:sub(1, 29) .. "..." end
    print(string.format("  %-32s %4ddBm  %3d  %s",
                        ssid, n.rssi, n.channel, n.encryption))
end
