import hashlib
import time
import customtkinter as ctk
from tkinter import filedialog, messagebox
import threading

ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("green")

class HashAuditorApp(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("Pisces Moon - High-Speed Hash Auditor")
        self.geometry("500x450")
        self.dict_path = None

        # UI LAYOUT
        self.header = ctk.CTkLabel(self, text="LOCAL HASH AUDITOR", font=("Courier", 24, "bold"), text_color="#18C3")
        self.header.pack(pady=(20, 10))

        self.target_entry = ctk.CTkEntry(self, placeholder_text="Enter Target SHA-256 Hash...", width=400, font=("Courier", 12))
        self.target_entry.pack(pady=10)

        self.load_btn = ctk.CTkButton(self, text="[ SELECT DICTIONARY FILE ]", command=self.load_dict, fg_color="#444444", hover_color="#222222")
        self.load_btn.pack(pady=10)

        self.dict_lbl = ctk.CTkLabel(self, text="No dictionary loaded.", font=("Courier", 10), text_color="#aaaaaa")
        self.dict_lbl.pack(pady=0)

        self.start_btn = ctk.CTkButton(self, text="INITIATE BRUTE FORCE", command=self.start_attack, state="disabled", fg_color="#F800", hover_color="#800000")
        self.start_btn.pack(pady=20)

        self.console = ctk.CTkTextbox(self, width=450, height=150, font=("Courier", 12), fg_color="#000000", text_color="#07E0")
        self.console.pack(pady=10)
        self.log("System Ready. Awaiting parameters.")

    def log(self, text):
        self.console.insert("end", text + "\n")
        self.console.see("end")

    def load_dict(self):
        self.dict_path = filedialog.askopenfilename(title="Select Password List", filetypes=(("Text Files", "*.txt"), ("All Files", "*.*")))
        if self.dict_path:
            self.dict_lbl.configure(text=f"Loaded: {self.dict_path.split('/')[-1]}")
            self.start_btn.configure(state="normal")

    def start_attack(self):
        target_hash = self.target_entry.get().strip().lower()
        if len(target_hash) != 64:
            messagebox.showerror("Error", "Invalid SHA-256 hash length. Must be 64 characters.")
            return

        self.start_btn.configure(state="disabled")
        self.log(f"\n[*] Target Acquired: {target_hash[:16]}...")
        self.log("[*] Engaging CPU Cores...")
        
        # Run in thread so the UI doesn't freeze
        threading.Thread(target=self.crack_hash, args=(target_hash,), daemon=True).start()

    def crack_hash(self, target_hash):
        start_time = time.time()
        attempts = 0
        found = False

        try:
            with open(self.dict_path, 'r', encoding='utf-8', errors='ignore') as f:
                for line in f:
                    word = line.strip()
                    if not word: continue
                    attempts += 1
                    
                    # Core cryptography engine
                    if hashlib.sha256(word.encode('utf-8')).hexdigest() == target_hash:
                        found = True
                        break

                    # Update UI roughly every 500,000 hashes to prevent UI bottleneck
                    if attempts % 500000 == 0:
                        self.log(f"[-] Passed {attempts:,} hashes...")

        except Exception as e:
            self.log(f"[!] File Error: {e}")
            self.start_btn.configure(state="normal")
            return

        duration = time.time() - start_time
        speed = attempts / duration if duration > 0 else 0

        self.log("\n==================================")
        if found:
            self.log(f"[+] MATCH FOUND: >> {word} <<")
        else:
            self.log("[-] EXHAUSTED. No match found.")
        self.log(f"[*] Speed: {speed:,.0f} Hashes/sec")
        self.log(f"[*] Time: {duration:.2f} seconds")
        self.log("==================================")
        self.start_btn.configure(state="normal")

if __name__ == "__main__":
    app = HashAuditorApp()
    app.mainloop()