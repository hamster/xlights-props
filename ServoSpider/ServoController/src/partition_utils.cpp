#include "partition_utils.h"
#include <esp_partition.h>
#include <esp_ota_ops.h>

void printPartitionTable() {
  Serial.println("\n=== ESP32 Partition Table ===");

  esp_partition_iterator_t pi = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);

  if (pi != NULL) {
    Serial.printf("%-16s | %4s | %-10s | %-10s | %10s\n",
                  "Label", "Type", "SubType", "Address", "Size");
    Serial.println("--------------------------------------------------------------------------------");

    do {
      const esp_partition_t* p = esp_partition_get(pi);

      String type = "Unknown";
      if (p->type == ESP_PARTITION_TYPE_APP) type = "APP";
      else if (p->type == ESP_PARTITION_TYPE_DATA) type = "DATA";

      String subtype = "Unknown";
      if (p->type == ESP_PARTITION_TYPE_APP) {
        if (p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) subtype = "Factory";
        else if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) subtype = "OTA_0";
        else if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) subtype = "OTA_1";
        else if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_2) subtype = "OTA_2";
        else if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_3) subtype = "OTA_3";
      } else if (p->type == ESP_PARTITION_TYPE_DATA) {
        if (p->subtype == ESP_PARTITION_SUBTYPE_DATA_OTA) subtype = "OTA_DATA";
        else if (p->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS) subtype = "NVS";
        else if (p->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS) subtype = "SPIFFS";
        else if (p->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT) subtype = "FAT";
      }

      Serial.printf("%-16s | %4s | %-10s | 0x%08x | %7d KB\n",
                    p->label,
                    type.c_str(),
                    subtype.c_str(),
                    p->address,
                    p->size / 1024);
    } while (pi = esp_partition_next(pi));

    esp_partition_iterator_release(pi);
  }

  Serial.println("================================================================================");
}

void printOTAInfo() {
  Serial.println("\n=== OTA Partition Status ===");

  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();

  if (running) {
    Serial.printf("Running partition:  %-16s (0x%08x, %d KB)\n",
                  running->label, running->address, running->size / 1024);
  } else {
    Serial.println("Running partition: Unknown");
  }

  if (boot) {
    Serial.printf("Boot partition:     %-16s (0x%08x, %d KB)\n",
                  boot->label, boot->address, boot->size / 1024);
  } else {
    Serial.println("Boot partition: Unknown");
  }

  // Check if there's an OTA update partition available
  const esp_partition_t* update = esp_ota_get_next_update_partition(NULL);
  if (update) {
    Serial.printf("Next OTA target:    %-16s (0x%08x, %d KB available)\n",
                  update->label, update->address, update->size / 1024);
  } else {
    Serial.println("Next OTA target: None available");
  }

  Serial.println("============================\n");
}
