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

#pragma once
#include "IHardwareManager.h"
#include "IFR1Protocol.h"
#include "EventProcessor.h"
#include "OutputProcessor.h"
#include "XPlaneSDK.h"
#include "ThreadSafeQueue.h"
#include "ModeDisplay.h"
#include "SettingsManager.h"
#include <array>
#include <thread>
#include <atomic>
#include <vector>

class DeviceHandler {
public:
    DeviceHandler(IHardwareManager& hw, EventProcessor& eventProc, OutputProcessor& outputProc, SettingsManager& settings, IXPlaneSDK& sdk, bool startThread = true);
    ~DeviceHandler();

    /**
     * @brief Polls the device and processes any new data.
     * @param config The current aircraft configuration.
     * @param currentTime Current time in seconds.
     */
    void Update(const nlohmann::json& config, float currentTime);

    /**
     * @brief Parses and caches the mode display descriptions from the config.
     * @param config The full aircraft configuration JSON.
     */
    void ParseModeDescriptions(const nlohmann::json& config);

    /**
     * @brief Updates the LEDs on the device.
     * @param currentTime Current time in seconds.
     */
    void UpdateLEDs(float currentTime);

    /**
     * @brief Turns off all LEDs and clears the flash bit.
     */
    void ClearLEDs();

    /**
     * @brief Performs one iteration of the hardware communication logic.
     * Used by the worker thread or manually in tests.
     */
    void ProcessHardware();

    /**
     * @brief Logs diagnostics produced by the hardware worker thread.
     * Must be called from X-Plane's main thread.
     */
    void FlushWorkerDiagnostics();

private:
    void ProcessReport(const IFR1::HardwareEvent& event, const nlohmann::json& config, float currentTime);
    static std::string GetModeString(IFR1::Mode mode, bool shifted);
    static std::string GetControlString(IFR1::Button button, IFR1::Mode mode);
    void HandleKnobs(const IFR1::HardwareEvent& event, const nlohmann::json& config) const;
    void HandleButtons(const IFR1::HardwareEvent& event, const nlohmann::json& config, float currentTime);
    void QueueWorkerDiagnostic(std::string message);
    
    void WorkerThread();
    IFR1::HardwareEvent ParseReport(const uint8_t* data);

    IHardwareManager& m_hw;
    EventProcessor& m_eventProc;
    OutputProcessor& m_outputProc;
    SettingsManager& m_settings;
    IXPlaneSDK& m_sdk;

    std::unordered_map<std::string, std::string> m_modeDescriptions;

    // Threading
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    ThreadSafeQueue<IFR1::HardwareEvent> m_inputQueue;
    ThreadSafeQueue<uint8_t> m_outputQueue;
    ThreadSafeQueue<std::string> m_workerDiagnostics;
    std::atomic<bool> m_isConnected{false};

    // State
    IFR1::Mode m_currentMode = IFR1::Mode::COM1;
    bool m_shifted = false;
    uint8_t m_lastLedBits = 0;
    bool m_lastConnectedState = false;
    
    // Button state tracking
    struct ButtonState {
        bool currentlyHeld = false;
        float pressStartTime = 0.0f;
        bool longPressDetected = false;
    };
    std::array<ButtonState, 12> m_buttonStates;
    std::vector<int> m_heldButtons;
    
    // Last raw report to detect changes
    std::array<uint8_t, IFR1::HID_REPORT_SIZE> m_lastReport{};

    std::string m_clickSoundPath;
    bool m_clickSoundExists = false;

    ModeDisplay m_modeDisplay;
    std::string m_lastModeString;
};
