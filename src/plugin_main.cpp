/*
 *   Copyright 2026 Kyle D. Ross
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>

#include "XPLMPlugin.h"
#include "XPLMDisplay.h"
#include "XPLMProcessing.h"
#include "XPLMMenus.h"

#include "ConfigManager.h"
#include "EventProcessor.h"
#include "OutputProcessor.h"
#include "HIDManager.h"
#include "DeviceHandler.h"
#include "XPlaneSDK.h"
#include "core/SettingsManager.h"
#include "ui/AboutWindow.h"
#include "ui/SettingsWindow.h"
#include "ui/QuickReferenceWindow.h"
#include "core/Logger.h"

namespace fs = std::filesystem;

// Global state
static std::unique_ptr<IXPlaneSDK> gSDK;
static std::unique_ptr<ConfigManager> gConfigManager;
static std::unique_ptr<EventProcessor> gEventProcessor;
static std::unique_ptr<OutputProcessor> gOutputProcessor;
static std::unique_ptr<HIDManager> gHIDManager;
static std::unique_ptr<SettingsManager> gSettingsManager;
static std::unique_ptr<DeviceHandler> gDeviceHandler;

static nlohmann::json gCurrentConfig;
static std::string gCurrentAircraftPath;
static void* gAcfPathRef = nullptr;

static XPLMFlightLoopID gFlightLoop = nullptr;

// Menu and UI
static XPLMMenuID gSubMenu = nullptr;
static int gSubMenuIndex = -1;

// Menu handler
static void MenuHandler(void* /*inMenuRef*/, void* inItemRef) {
    auto item = reinterpret_cast<intptr_t>(inItemRef);
    if (item == 1) {
        ui::about::Show();
    } else if (item == 2) {
        ui::quick_ref::Show(gCurrentConfig);
    } else if (item == 3) {
        ui::settings::Show(*gSettingsManager, *gSDK);
    }
}

// ReSharper disable once CppDFAConstantFunctionResult
static float FlightLoopCallback(float /*inElapsedSinceLastCall*/, float /*inElapsedTimeSinceLastFlightLoop*/, int inCounter, void* /*inRefcon*/) {
    if (!gSDK || !gDeviceHandler) return -1.0f;

    try {
        float now = gSDK->GetElapsedTime();
        gDeviceHandler->FlushWorkerDiagnostics();

        // 1. Aircraft detection every 20 frames (or if first time)
        static int lastDetectionCounter = -1;
        if (lastDetectionCounter == -1 || inCounter >= lastDetectionCounter + 20 || inCounter < lastDetectionCounter) {
            lastDetectionCounter = inCounter;

            if (!gAcfPathRef) {
                gAcfPathRef = gSDK->FindDataRef("sim/aircraft/view/acf_relative_path");
            }

            if (gAcfPathRef) {
                char path[512];
                int bytes = gSDK->GetDatab(gAcfPathRef, path, 0, sizeof(path) - 1);
                if (bytes > 0) {
                    path[bytes] = '\0';
                    std::string currentPath(path);
                    if (currentPath != gCurrentAircraftPath) {
                        IFR1_LOG_INFO(*gSDK, "Aircraft changed to {}", currentPath);
                        gCurrentAircraftPath = currentPath;
                        gDeviceHandler->ClearLEDs();
                        gCurrentConfig = gConfigManager->GetConfigForAircraft(currentPath, *gSDK);

                        if (!gCurrentConfig.empty() && !gCurrentConfig.contains("output")) {
                            IFR1_LOG_ERROR(*gSDK, "Loaded config '{}' is missing 'output' section!",
                                           gCurrentConfig.value("name", "unknown"));
                        }

                        gDeviceHandler->ParseModeDescriptions(gCurrentConfig);
                        gOutputProcessor->ParseOutputConfig(gCurrentConfig);
                        gEventProcessor->PrepareConfig(gCurrentConfig);

                        // Update log level based on config
                        if (gCurrentConfig.value("debug", false)) {
                            gSDK->SetLogLevel(LogLevel::Verbose);
                        } else {
                            gSDK->SetLogLevel(LogLevel::Info);
                        }

                        if (gCurrentConfig.empty()) {
                            IFR1_LOG_INFO(*gSDK, "No configuration found for this aircraft.");
                        } else {
                            std::string configName = gCurrentConfig.value("name", "Unknown");
                            if (gCurrentConfig.value("fallback", false)) {
                                IFR1_LOG_INFO(*gSDK, "Using fallback configuration: {}", configName);
                            } else {
                                IFR1_LOG_INFO(*gSDK, "Configuration loaded: {}", configName);
                            }
                        }
                    }
                } else {
                    if (!gCurrentAircraftPath.empty()) {
                        IFR1_LOG_INFO(*gSDK, "No aircraft detected.");
                        gCurrentAircraftPath.clear();
                        gCurrentConfig = nlohmann::json();
                        gEventProcessor->PrepareConfig(gCurrentConfig);
                        gDeviceHandler->ClearLEDs();
                    }
                }
            } else {
                IFR1_LOG_ERROR(*gSDK, "Could not find 'sim/aircraft/view/acf_relative_path' dataref.");
            }
        }

        if (gCurrentAircraftPath.empty()) {
            return -1.0f;
        }

        // 2. Update hardware input
        gDeviceHandler->Update(gCurrentConfig, now);

        // 3. Update LEDs
        gDeviceHandler->UpdateLEDs(now);

    } catch (const std::exception& e) {
        IFR1_LOG_ERROR(*gSDK, "Unhandled exception in flight loop: {}", e.what());
    } catch (...) {
        IFR1_LOG_ERROR(*gSDK, "Unknown exception in flight loop");
    }

    return -1.0f; // Run every frame
}

PLUGIN_API int XPluginStart(char * outName, char * outSig, char * outDesc) {
    std::strcpy(outName, "IFR-1 Plugin");
    std::strcpy(outSig, "com.kyleross.ifr1flex");
    std::strcpy(outDesc, "Flexible IFR-1 interface.");

    gSDK = CreateXPlaneSDK();

    char pluginPath[512];
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, pluginPath, nullptr, nullptr);
    fs::path p(pluginPath);

    gSettingsManager = std::make_unique<SettingsManager>(p.parent_path() / "settings.json");
    gSettingsManager->Load(*gSDK);

    gConfigManager = std::make_unique<ConfigManager>();
    gEventProcessor = std::make_unique<EventProcessor>(*gSDK);
    gOutputProcessor = std::make_unique<OutputProcessor>(*gSDK);
    gHIDManager = std::make_unique<HIDManager>();
    gDeviceHandler = std::make_unique<DeviceHandler>(*gHIDManager, *gEventProcessor, *gOutputProcessor, *gSettingsManager, *gSDK);

    // Config directory discovery:
    // 1. Check parent folder of the plugin binary (e.g. plugins/ifr1flex/64/lin.xpl -> plugins/ifr1flex/configs)
    // 2. Check the plugin binary folder itself (e.g. plugins/ifr1flex/lin.xpl -> plugins/ifr1flex/configs)
    
    fs::path configDir;
    fs::path p1 = p.parent_path().parent_path() / "configs";
    fs::path p2 = p.parent_path() / "configs";

    if (fs::exists(p1) && fs::is_directory(p1)) {
        configDir = p1;
    } else if (fs::exists(p2) && fs::is_directory(p2)) {
        configDir = p2;
    }
    
    size_t loaded = 0;
    if (!configDir.empty()) {
        loaded = gConfigManager->LoadConfigs(configDir.string(), *gSDK);
    }
    
    char msg[1024];
    if (loaded == 0) {
        if (configDir.empty()) {
            std::snprintf(msg, sizeof(msg), "ERROR: Could not find 'configs' directory. Tried:\n  1. %s\n  2. %s\n", 
                         p1.string().c_str(), p2.string().c_str());
            IFR1_LOG_ERROR(*gSDK, "{}", msg);
        } else {
            std::snprintf(msg, sizeof(msg), "WARNING: No configuration files found in %s\n", configDir.string().c_str());
            IFR1_LOG_INFO(*gSDK, "{}", msg);
        }
    } else {
        std::snprintf(msg, sizeof(msg), "Loaded %zu configurations from %s\n", loaded, configDir.string().c_str());
        IFR1_LOG_INFO(*gSDK, "{}", msg);
    }

    // Create Plugins menu: "IFR-1" -> "About..."
    if (XPLMMenuID pluginsMenu = XPLMFindPluginsMenu()) {
        gSubMenuIndex = XPLMAppendMenuItem(pluginsMenu, "IFR-1 Controller", nullptr, 0);
        gSubMenu = XPLMCreateMenu("IFR-1 Controller", pluginsMenu, gSubMenuIndex, MenuHandler, nullptr);
        if (gSubMenu) {
            XPLMAppendMenuItem(gSubMenu, "Settings...", reinterpret_cast<void*>(3), 0);
            XPLMAppendMenuItem(gSubMenu, "Quick Reference...", reinterpret_cast<void*>(2), 0);
            XPLMAppendMenuSeparator(gSubMenu);
            XPLMAppendMenuItem(gSubMenu, "About...", reinterpret_cast<void*>(1), 0);
        }
    }

    return 1;
}

PLUGIN_API void XPluginStop(void) {
    gDeviceHandler.reset();
    gHIDManager.reset();
    if (gFlightLoop) {
        XPLMDestroyFlightLoop(gFlightLoop);
        gFlightLoop = nullptr;
    }
    
    // Destroy UI/menu if still present
    ui::about::Close();
    ui::quick_ref::Close();
    ui::settings::Close();
    if (gSubMenu) {
        XPLMDestroyMenu(gSubMenu);
        gSubMenu = nullptr;
        gSubMenuIndex = -1;
    }

    gOutputProcessor.reset();
    gEventProcessor.reset();
    gConfigManager.reset();
    gSDK.reset();
}

PLUGIN_API void XPluginDisable(void) {
    if (gDeviceHandler) {
        gDeviceHandler->ClearLEDs();
        gDeviceHandler.reset();
    }
    if (gHIDManager) {
        gHIDManager.reset();
    }

    if (gFlightLoop) {
        XPLMDestroyFlightLoop(gFlightLoop);
        gFlightLoop = nullptr;
    }

    // Ensure any modal is closed when disabling
    ui::about::Close();
    ui::quick_ref::Close();
    ui::settings::Close();
}

PLUGIN_API int XPluginEnable(void) {
    if (!gHIDManager) {
        gHIDManager = std::make_unique<HIDManager>();
    }
    if (!gDeviceHandler) {
        gDeviceHandler = std::make_unique<DeviceHandler>(*gHIDManager, *gEventProcessor, *gOutputProcessor, *gSettingsManager, *gSDK);
    }

    gCurrentAircraftPath.clear();
    gAcfPathRef = nullptr; // Force fresh dataref lookup on re-enable

    if (gDeviceHandler) {
        gDeviceHandler->ClearLEDs();
    }

    XPLMCreateFlightLoop_t params;
    params.structSize = sizeof(XPLMCreateFlightLoop_t);
    params.phase = xplm_FlightLoop_Phase_BeforeFlightModel;
    params.callbackFunc = FlightLoopCallback;
    params.refcon = nullptr;

    gFlightLoop = XPLMCreateFlightLoop(&params);
    XPLMScheduleFlightLoop(gFlightLoop, -1.0f, 1);

    return 1;
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFromWho, int inMessage, void *inParam) {
}