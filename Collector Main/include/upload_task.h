#pragma once
#include "webinterface.h"

// ============================================================
//  upload_task.h  — FreeRTOS task that batches and POSTs
//                   device data to the HTTP API.
// ============================================================

// Task entry point — pass to xTaskCreate()
void uploadTask(void* param);
void uploadInverterData(void* param);
