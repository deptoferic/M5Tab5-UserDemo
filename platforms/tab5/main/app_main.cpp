/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <app.h>
#include <hal/hal.h>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "link/link_tab5.h"

// Defined in console_tab5.cpp. File scope: `extern "C"` is not permitted at
// block scope in C++.
extern "C" void console_tab5_start(void);

extern "C" void app_main(void)
{
    // Tab5 Sensor Hub, Phase 1c: bring the Schema A link up before the UI.
    // It runs in its own task and writes app/link/link_state.h; nothing here
    // renders it. Per that project's CLAUDE.md §6, rendering READS that state
    // and never touches this transport.
    link_tab5_start();

    // Console on USB-Serial-JTAG: rtc / rtcset / link. The RX8130 drifts and
    // the factory demo can only set it through a touch panel, which is no use
    // for setting it accurately from a host clock.
    console_tab5_start();

    // 应用层初始化回调
    app::InitCallback_t callback;

    callback.onHalInjection = []() {
        // 注入桌面平台的硬件抽象
        hal::Inject(std::make_unique<HalEsp32>());
    };

    // 应用层启动
    app::Init(callback);
    while (!app::IsDone()) {
        app::Update();
        vTaskDelay(1);
    }
    app::Destroy();
}
