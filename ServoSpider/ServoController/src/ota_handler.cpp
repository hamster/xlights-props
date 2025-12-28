#include "ota_handler.h"
#include <WiFi.h>
#include <Update.h>
#include <esp_task_wdt.h>

// OTA update flag
bool otaInProgress = false;

// 4KB buffer for OTA writes (aligned with flash sector size)
#define OTA_BUFFER_SIZE 4096
static uint8_t otaBuffer[OTA_BUFFER_SIZE];
static size_t otaBufferIndex = 0;

void handleOTAUpdate() {
  static size_t totalBytesWritten = 0;
  static size_t totalBytesReceived = 0;
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Set flag to disable other handlers during OTA
    otaInProgress = true;
    totalBytesWritten = 0;
    totalBytesReceived = 0;
    otaBufferIndex = 0;

    Serial.println("OTA Update Starting...");
    Serial.print("Filename: ");
    Serial.println(upload.filename);
    Serial.println("Using 4KB buffered writes for optimal flash performance");

    // Feed watchdog before starting
    esp_task_wdt_reset();

    // Maximize WiFi stability during upload
    WiFi.setSleep(WIFI_PS_NONE);  // Completely disable WiFi power save
    WiFi.setAutoReconnect(false);  // Disable auto-reconnect during upload
    WiFi.setTxPower(WIFI_POWER_19_5dBm);  // Set to maximum TX power

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      otaInProgress = false;
      WiFi.setSleep(true);
      WiFi.setAutoReconnect(true);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // Feed watchdog during upload to prevent timeout
    esp_task_wdt_reset();

    // Buffer the incoming data
    totalBytesReceived += upload.currentSize;

    // Debug: Log every chunk received
    Serial.print("Received chunk: ");
    Serial.print(upload.currentSize);
    Serial.print(" bytes, total: ");
    Serial.println(totalBytesReceived);
    size_t bytesToCopy = upload.currentSize;
    size_t sourceOffset = 0;

    while (bytesToCopy > 0) {
      // Calculate how much space is left in the buffer
      size_t spaceInBuffer = OTA_BUFFER_SIZE - otaBufferIndex;
      size_t copySize = (bytesToCopy < spaceInBuffer) ? bytesToCopy : spaceInBuffer;

      // Copy data into the buffer
      memcpy(otaBuffer + otaBufferIndex, upload.buf + sourceOffset, copySize);
      otaBufferIndex += copySize;
      sourceOffset += copySize;
      bytesToCopy -= copySize;

      // If buffer is full (4KB), write it to flash
      if (otaBufferIndex >= OTA_BUFFER_SIZE) {
        // Allow WiFi stack time to process before flash write
        yield();

        // Write the full 4KB buffer to flash
        size_t written = Update.write(otaBuffer, OTA_BUFFER_SIZE);
        if (written != OTA_BUFFER_SIZE) {
          Serial.print("Write failed: expected ");
          Serial.print(OTA_BUFFER_SIZE);
          Serial.print(" bytes, wrote ");
          Serial.println(written);
          Update.printError(Serial);
        } else {
          totalBytesWritten += written;
          // Print progress every 64KB
          if (totalBytesWritten % 65536 < OTA_BUFFER_SIZE) {
            Serial.print("Written: ");
            Serial.print(totalBytesWritten);
            Serial.print(" bytes (");
            Serial.print((totalBytesWritten * 100) / totalBytesReceived);
            Serial.println("%)");
            esp_task_wdt_reset();
          }
        }

        // Reset buffer
        otaBufferIndex = 0;

        // Feed watchdog after write
        esp_task_wdt_reset();

        // Allow WiFi stack time to recover after flash write
        yield();
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    // Write any remaining data in the buffer (partial final chunk)
    if (otaBufferIndex > 0) {
      Serial.print("Writing final ");
      Serial.print(otaBufferIndex);
      Serial.println(" bytes...");

      yield();

      size_t written = Update.write(otaBuffer, otaBufferIndex);
      if (written != otaBufferIndex) {
        Serial.print("Final write failed: expected ");
        Serial.print(otaBufferIndex);
        Serial.print(" bytes, wrote ");
        Serial.println(written);
        Update.printError(Serial);
      } else {
        totalBytesWritten += written;
      }

      otaBufferIndex = 0;
    }

    // Clear flag when upload completes
    otaInProgress = false;

    // Restore WiFi settings
    WiFi.setSleep(true);
    WiFi.setAutoReconnect(true);

    Serial.print("Upload finished. Total bytes received: ");
    Serial.print(totalBytesReceived);
    Serial.print(", written: ");
    Serial.println(totalBytesWritten);

    if (Update.end(true)) {
      Serial.println("OTA Update Success!");
      Serial.print("Total size: ");
      Serial.println(upload.totalSize);
    } else {
      Serial.println("Update.end() failed:");
      Update.printError(Serial);
    }
    totalBytesWritten = 0;
    totalBytesReceived = 0;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    // Clear flag if upload is aborted
    Serial.println("\n!!! OTA UPLOAD ABORTED !!!");
    Serial.print("Received: ");
    Serial.print(totalBytesReceived);
    Serial.print(" bytes, Written: ");
    Serial.print(totalBytesWritten);
    Serial.println(" bytes");
    Serial.print("WiFi Status: ");
    Serial.println(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    Serial.print("WiFi RSSI: ");
    Serial.println(WiFi.RSSI());

    otaInProgress = false;
    totalBytesWritten = 0;
    totalBytesReceived = 0;
    otaBufferIndex = 0;

    // Restore WiFi settings
    WiFi.setSleep(true);
    WiFi.setAutoReconnect(true);

    Update.end();
    Serial.println("Update cleanup complete");
  }
}

void handleOTAUpdateComplete() {
  if (Update.hasError()) {
    String error = "Update Failed: ";
    error += Update.errorString();
    server.send(500, "text/plain", error);
    Serial.println(error);
  } else {
    server.send(200, "text/plain", "Update Successful! Rebooting...");
    Serial.println("Update completed successfully, rebooting...");
    delay(1000);
    ESP.restart();
  }
}
