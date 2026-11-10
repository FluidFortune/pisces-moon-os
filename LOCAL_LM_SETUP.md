# Using your own computer's AI with Pisces Moon

The AI terminal can answer from AI models running on your own computer,
through the **Pisces AI Companion** — a small program you run next to
[Ollama](https://ollama.com). Your prompts go to your computer, not to a
cloud service, and the device never holds a cloud API key.

> The old setup in this file (a VM proxy and `PISCES_VM_IP` in
> `secrets.h`) no longer exists. Gemini still works directly with
> `GEMINI_API_KEY` in `secrets.h`; the companion is the local option.

**Supported today:** T-Deck Plus, T-LoRa Pager, Cardputer ADV (the TERMINAL
app). C28P, Maxine and C5 are next.

## 1. On your computer

Install Ollama, then get the companion from the `pisces-ai-companion` repo:

```
ollama pull llama3.2:3b
python3 pisces_companion.py doctor
python3 pisces_companion.py pair --name Cardputer
python3 pisces_companion.py serve
```

`pair` prints a device key such as `7K2M-QX9P-4RTA-ZW3H-B8NE`. It is shown
once; pair again if you lose it. `serve` prints the address to use on your
home network, such as `http://192.168.1.50:8765`.

## 2. On the device

1. Connect to WiFi (WIFI app).
2. Open **TERMINAL** and type `/setup`.
3. **URL:** the address `serve` printed. `192.168.1.50` alone also works
   (port 8765 is assumed for http).
4. **KEY:** the device key. Dashes and capitals don't matter.
5. Pick a model from the list, or `0` for the companion's default.

Then type a question. The answer streams in as it is written. **Q** or
**ESC** stops it (T-Deck: tap the header).

## 3. From anywhere (Tailscale)

On the computer:

```
tailscale funnel --bg 8765
```

Run `/setup` again and use `https://<your-machine>.<your-tailnet>.ts.net`
as the URL. It works at home and away. The device checks the HTTPS
certificate against the Let's Encrypt roots built into the firmware
(`include/pm_ai_roots.h`).

A Funnel address can be reached by anyone on the internet; the device key
is what protects it. Revoke a lost device on the computer with
`python3 pisces_companion.py revoke <id>`.

## Terminal commands

| Command | What it does |
|---|---|
| `/setup` | Connect to the companion (URL + device key) |
| `/model` | Pick a model from the ones installed on the computer |
| `/new` | Start a new conversation |
| `/local` | Use your computer's AI |
| `/gemini` | Use Gemini (needs `GEMINI_API_KEY` in `secrets.h`) |
| `/status` | Show backend, URL, model and WiFi |
| `/clear` | Clear the screen |

## Notes

- **Where settings live.** URL, key, model and backend are stored in the
  device's internal flash (NVS, namespace `pm_ai`), not on the SD card. They
  survive reboots and SD card swaps.
- **Where history lives.** The conversation is kept on the computer, not on
  the device, so long chats cost the device no memory.
- **Cardputer.** AI chat needs normal WiFi, which can't run alongside a
  wardrive on the Cardputer. If a wardrive is running, the terminal asks
  before stopping it.
- **Fonts.** Replies are folded to plain ASCII by the companion (curly quotes
  become straight quotes, and so on), because the device fonts have no
  Unicode.
- **Protocol.** `pisces-ai-companion/docs/PROTOCOL.md`.
