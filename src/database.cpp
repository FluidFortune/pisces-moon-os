// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// This program is free software: you can redistribute it
// and/or modify it under the terms of the GNU Affero General
// Public License as published by the Free Software Foundation,
// either version 3 of the License, or any later version.
//
// fluidfortune.com

#include "database.h"
#include "SdFat.h"
#include <ArduinoJson.h>

extern SdFat sd;

// Global variables to track the current isolated memory session
int current_session_id = 1;
char current_file_path[64];

bool init_database() {
    // 1. Create the Root Vault Folder
    if (!sd.exists("/vault")) {
        sd.mkdir("/vault");
        Serial.println("[VAULT] Created /vault directory.");
    }
    
    // 2. Create the Sub-folder for isolated text files
    if (!sd.exists("/vault/memories")) {
        sd.mkdir("/vault/memories");
        Serial.println("[VAULT] Created /vault/memories directory.");
    }

    // 3. Create the "Brain" Index File (The Lookup Table)
    if (!sd.exists("/vault/index.json")) {
        FsFile indexFile = sd.open("/vault/index.json", O_WRITE | O_CREAT);
        if (indexFile) {
            indexFile.println("{}"); // Initialize an empty JSON object
            indexFile.close();
            Serial.println("[VAULT] Created blank index.json.");
        }
    }

    // 4. Find the next available Session ID so we don't overwrite old memories
    while (true) {
        snprintf(current_file_path, sizeof(current_file_path), "/vault/memories/memory_%03d.txt", current_session_id);
        
        if (!sd.exists(current_file_path)) {
            // Found a blank slate!
            break;
        }
        current_session_id++;
    }

    // 5. Initialize the new Session File with a clean header
    FsFile file = sd.open(current_file_path, O_WRITE | O_CREAT);
    if (file) {
        file.printf("--- PISCES MOON VAULT: SESSION %03d ---\n\n", current_session_id);
        file.close();
        Serial.printf("[VAULT] Session initialized at: %s\n", current_file_path);
        return true;
    }

    Serial.println("[VAULT] ERROR: Failed to create session file.");
    return false;
}

bool save_chat_to_vault(const char* role, const char* message) {
    // Open the current session's file in APPEND mode
    FsFile file = sd.open(current_file_path, O_WRITE | O_APPEND);
    if (!file) {
        Serial.println("[VAULT] ERROR: Cannot open session file for writing.");
        return false;
    }

    // Write the interaction
    file.printf("%s: %s\n", role, message);
    file.println("----------------------------------------");
    
    file.close();
    Serial.printf("[VAULT] Appended 1 message to %s.\n", current_file_path);
    return true;
}