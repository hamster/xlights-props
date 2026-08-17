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

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      otaInProgress = false;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {

    // Buffer the incoming data
    totalBytesReceived += upload.currentSize;
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

      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    // Write any remaining data in the buffer (partial final chunk)
    if (otaBufferIndex > 0) {
      Serial.print("Writing final ");
      Serial.print(otaBufferIndex);
      Serial.println(" bytes...");

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

    otaInProgress = false;
    totalBytesWritten = 0;
    totalBytesReceived = 0;
    otaBufferIndex = 0;

    Update.end();
    Serial.println("Update cleanup complete");
  }
}

void handleOTAUpdateComplete() {
  if (Update.hasError()) {
    String error = "{\"success\":false,\"message\":\"Update Failed: ";
    error += Update.errorString();
    error += "\"}";
    server.send(500, "application/json", error);
    Serial.print("Update failed: ");
    Serial.println(Update.errorString());
  } else {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Update Successful! Rebooting...\"}");
    Serial.println("Update completed successfully, rebooting...");
    delay(1000);
    ESP.restart();
  }
}
