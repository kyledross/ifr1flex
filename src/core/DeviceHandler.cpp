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

#include "DeviceHandler.h"
#include "Logger.h"
#include <cctype>
#include <format>

DeviceHandler::DeviceHandler(IHardwareManager& hw, EventProcessor& eventProc, OutputProcessor& outputProc, SettingsManager& settings, IXPlaneSDK& sdk, bool startThread) 
    : m_hw(hw), m_eventProc(eventProc), m_outputProc(outputProc), m_settings(settings), m_sdk(sdk), m_modeDisplay(sdk, settings) {
    for (auto& state : m_buttonStates) {
        state.currentlyHeld = false;
        state.pressStartTime = 0.0f;
        state.longPressDetected = false;
    }
    m_clickSoundPath = m_sdk.GetSystemPath() + "Resources/sounds/systems/click.wav";
    m_clickSoundExists = m_sdk.FileExists(m_clickSoundPath);

    m_running = true;
    if (startThread) {
        m_thread = std::thread([this] { WorkerThread(); });
    }
}

DeviceHandler::~DeviceHandler() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_hw.IsConnected()) {
        m_hw.Disconnect();
    }
}

void DeviceHandler::Update(const nlohmann::json& config, float currentTime) {
    bool currentlyConnected = m_isConnected;

    if (currentlyConnected && !m_lastConnectedState) {
        IFR1_LOG_INFO(m_sdk, "Device connected.");
        ClearLEDs();
    } else if (!currentlyConnected && m_lastConnectedState) {
        IFR1_LOG_ERROR(m_sdk, "Device disconnected.");
    }
    m_lastConnectedState = currentlyConnected;

    if (!currentlyConnected) {
        m_lastModeString.clear();
        return;
    }

    // Process all available reports from the queue
    while (auto event = m_inputQueue.Pop()) {
        ProcessReport(*event, config, currentTime);
    }

    // Process long presses even if no new HID report (timer based)
    // Only check buttons that are actually currently held
    for (int i : m_heldButtons) {
        if (!m_buttonStates[i].longPressDetected) {
            if (currentTime - m_buttonStates[i].pressStartTime >= 0.3f) {
                m_buttonStates[i].longPressDetected = true;

                auto btn = static_cast<IFR1::Button>(i);
                IFR1_LOG_VERBOSE(m_sdk, "Button {} long-press", GetControlString(btn, m_currentMode));
                if (btn == IFR1::Button::INNER_KNOB) {
                    if (m_clickSoundExists) {
                        m_sdk.PlaySound(m_clickSoundPath);
                    }
                    m_shifted = !m_shifted;
                } else {
                    m_eventProc.ProcessEvent(config, GetModeString(m_currentMode, m_shifted), GetControlString(btn, m_currentMode), "long-press");
                }
            }
        }
    }

    std::string currentModeStr = GetModeString(m_currentMode, m_shifted);
    if (currentModeStr != m_lastModeString) {
        std::string osdPosition = m_settings.GetString("osd-position", "disabled");
        if (!currentModeStr.empty() && osdPosition != "disabled") {
            std::string displayStr = currentModeStr;
            
            auto it = m_modeDescriptions.find(currentModeStr);
            if (it != m_modeDescriptions.end()) {
                displayStr = it->second;
            }
            
            for (auto& c : displayStr) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            m_modeDisplay.ShowMessage(displayStr, currentTime);
        }
        m_lastModeString = currentModeStr;
    }

    m_modeDisplay.Update(currentTime);
    m_eventProc.ProcessQueue();
}

void DeviceHandler::ParseModeDescriptions(const nlohmann::json& config) {
    m_modeDescriptions.clear();
    if (config.contains("modes")) {
        for (auto it = config["modes"].begin(); it != config["modes"].end(); ++it) {
            if (it.value().contains("description") && it.value()["description"].is_string()) {
                m_modeDescriptions[it.key()] = it.value()["description"].get<std::string>();
            }
        }
    }
}

void DeviceHandler::UpdateLEDs(float currentTime) {
    if (!m_isConnected) return;

    uint8_t ledBits = m_outputProc.EvaluateLEDs(currentTime);
    
    // Add mode flash bit if shifted
    if (m_shifted) {
        ledBits |= IFR1::LEDMask::MODE_FLASH;
    }

    if (ledBits != m_lastLedBits) {
        IFR1_LOG_VERBOSE(m_sdk, "LEDs being updated.  Bits: {}", ledBits);
        m_outputQueue.Push(ledBits);
        m_lastLedBits = ledBits;
    }
}

void DeviceHandler::FlushWorkerDiagnostics() {
    while (auto diagnostic = m_workerDiagnostics.Pop()) {
        IFR1_LOG_ERROR(m_sdk, "{}", *diagnostic);
    }
}

void DeviceHandler::ClearLEDs() {
    m_shifted = false;
    m_currentMode = IFR1::Mode::COM1;
    m_lastLedBits = 0;
    m_outputQueue.Push(0);
    m_heldButtons.clear();
    for (auto& state : m_buttonStates) {
        state.currentlyHeld = false;
        state.longPressDetected = false;
    }
}

void DeviceHandler::ProcessReport(const IFR1::HardwareEvent& event, const nlohmann::json& config, float currentTime) {
    if (event.mode != m_currentMode) {
        m_shifted = false;
        m_currentMode = event.mode;
    }

    HandleKnobs(event, config);
    HandleButtons(event, config, currentTime);
}

IFR1::HardwareEvent DeviceHandler::ParseReport(const uint8_t* data) {
    // data[1..3] buttons, data[5] outer knob, data[6] inner knob, data[7] mode

    IFR1::HardwareEvent event;
    event.outerKnobRotation = static_cast<int8_t>(data[5]);
    event.innerKnobRotation = static_cast<int8_t>(data[6]);

    // Validate the mode byte before casting; fall back to COM1 on unknown values
    const auto rawMode = data[7];
    if (rawMode <= static_cast<uint8_t>(IFR1::Mode::XPDR)) {
        event.mode = static_cast<IFR1::Mode>(rawMode);
    } else {
        QueueWorkerDiagnostic(
            std::format("HID report contained unknown mode byte 0x{:02X}; defaulting to COM1", rawMode));
        event.mode = IFR1::Mode::COM1;
    }

    auto checkBit = [](uint8_t val, uint8_t bit) {
        if (bit == 0) return false;
        return (val & (1u << (bit - 1))) != 0;
    };

    event.buttonStates[static_cast<int>(IFR1::Button::DIRECT)] = checkBit(data[1], IFR1::BitPosition::DIRECT);
    event.buttonStates[static_cast<int>(IFR1::Button::MENU)] = checkBit(data[1], IFR1::BitPosition::MENU);
    event.buttonStates[static_cast<int>(IFR1::Button::CLR)] = checkBit(data[1], IFR1::BitPosition::CLR);
    event.buttonStates[static_cast<int>(IFR1::Button::ENT)] = checkBit(data[1], IFR1::BitPosition::ENT);

    event.buttonStates[static_cast<int>(IFR1::Button::SWAP)] = checkBit(data[2], IFR1::BitPosition::SWAP);
    event.buttonStates[static_cast<int>(IFR1::Button::INNER_KNOB)] = checkBit(data[2], IFR1::BitPosition::INNER_KNOB);
    event.buttonStates[static_cast<int>(IFR1::Button::AP)] = checkBit(data[2], IFR1::BitPosition::AP);
    event.buttonStates[static_cast<int>(IFR1::Button::HDG)] = checkBit(data[2], IFR1::BitPosition::HDG);

    event.buttonStates[static_cast<int>(IFR1::Button::NAV)] = checkBit(data[3], IFR1::BitPosition::NAV);
    event.buttonStates[static_cast<int>(IFR1::Button::APR)] = checkBit(data[3], IFR1::BitPosition::APR);
    event.buttonStates[static_cast<int>(IFR1::Button::ALT)] = checkBit(data[3], IFR1::BitPosition::ALT);
    event.buttonStates[static_cast<int>(IFR1::Button::VS)] = checkBit(data[3], IFR1::BitPosition::VS);

    return event;
}

void DeviceHandler::QueueWorkerDiagnostic(std::string message) {
    m_workerDiagnostics.Push(std::move(message));
}

void DeviceHandler::HandleKnobs(const IFR1::HardwareEvent& event, const nlohmann::json& config) const
{
    if (event.outerKnobRotation != 0) {
        std::string action = (event.outerKnobRotation > 0) ? "rotate-clockwise" : "rotate-counterclockwise";
        IFR1_LOG_VERBOSE(m_sdk, "Outer knob {}", action);
        for (int i = 0; i < std::abs(event.outerKnobRotation); ++i) {
            m_eventProc.ProcessEvent(config, GetModeString(m_currentMode, m_shifted), "outer-knob", action);
        }
    }
    if (event.innerKnobRotation != 0) {
        std::string action = (event.innerKnobRotation > 0) ? "rotate-clockwise" : "rotate-counterclockwise";
        IFR1_LOG_VERBOSE(m_sdk, "Inner knob {}", action);
        for (int i = 0; i < std::abs(event.innerKnobRotation); ++i) {
            m_eventProc.ProcessEvent(config, GetModeString(m_currentMode, m_shifted), "inner-knob", action);
        }
    }
}

void DeviceHandler::HandleButtons(const IFR1::HardwareEvent& event, const nlohmann::json& config, float currentTime) {
    for (int i = 0; i < 12; ++i) {
        bool current = event.buttonStates[i];
        bool last = m_buttonStates[i].currentlyHeld;

        if (current && !last) {
            // Pressed
            IFR1_LOG_VERBOSE(m_sdk, "Button {} pressed", GetControlString(static_cast<IFR1::Button>(i), m_currentMode));
            m_buttonStates[i].currentlyHeld = true;
            m_buttonStates[i].pressStartTime = currentTime;
            m_buttonStates[i].longPressDetected = false;
            m_heldButtons.push_back(i);
        } else if (!current && last) {
            // Released
            IFR1_LOG_VERBOSE(m_sdk, "Button {} released", GetControlString(static_cast<IFR1::Button>(i), m_currentMode));
            if (!m_buttonStates[i].longPressDetected) {
                // Short press
                m_eventProc.ProcessEvent(config, GetModeString(m_currentMode, m_shifted), GetControlString(static_cast<IFR1::Button>(i), m_currentMode), "short-press");
            }
            m_buttonStates[i].currentlyHeld = false;
            m_buttonStates[i].longPressDetected = false;
            std::erase(m_heldButtons, i);
        }
    }
}

std::string DeviceHandler::GetModeString(IFR1::Mode mode, bool shifted) {
    if (!shifted) {
        switch (mode) {
            case IFR1::Mode::COM1: return "com1";
            case IFR1::Mode::COM2: return "com2";
            case IFR1::Mode::NAV1: return "nav1";
            case IFR1::Mode::NAV2: return "nav2";
            case IFR1::Mode::FMS1: return "fms1";
            case IFR1::Mode::FMS2: return "fms2";
            case IFR1::Mode::AP: return "ap";
            case IFR1::Mode::XPDR: return "xpdr";
        }
    } else {
        switch (mode) {
            case IFR1::Mode::COM1: return "hdg";
            case IFR1::Mode::COM2: return "baro";
            case IFR1::Mode::NAV1: return "crs1";
            case IFR1::Mode::NAV2: return "crs2";
            case IFR1::Mode::FMS1: return "fms1-alt";
            case IFR1::Mode::FMS2: return "fms2-alt";
            case IFR1::Mode::AP: return "ap-alt";
            case IFR1::Mode::XPDR: return "xpdr-mode";
        }
    }
    return "";
}

std::string DeviceHandler::GetControlString(IFR1::Button button, IFR1::Mode mode) {
    if (mode == IFR1::Mode::FMS1 || mode == IFR1::Mode::FMS2) {
        switch (button) {
            case IFR1::Button::AP: return "cdi";
            case IFR1::Button::HDG: return "obs";
            case IFR1::Button::NAV: return "msg";
            case IFR1::Button::APR: return "fpl";
            case IFR1::Button::ALT: return "vnav";
            case IFR1::Button::VS: return "proc";
            default: break;
        }
    }

    switch (button) {
        case IFR1::Button::DIRECT: return "direct-to";
        case IFR1::Button::MENU: return "menu";
        case IFR1::Button::CLR: return "clr";
        case IFR1::Button::ENT: return "ent";
        case IFR1::Button::SWAP: return "swap";
        case IFR1::Button::AP: return "ap";
        case IFR1::Button::HDG: return "hdg";
        case IFR1::Button::NAV: return "nav";
        case IFR1::Button::APR: return "apr";
        case IFR1::Button::ALT: return "alt";
        case IFR1::Button::VS: return "vs";
        case IFR1::Button::INNER_KNOB: return "inner-knob-button";
        default: return "";
    }
}

void DeviceHandler::ProcessHardware() {
    uint8_t readBuffer[IFR1::HID_REPORT_SIZE + 1]{};

    bool currentlyConnected = m_hw.IsConnected();
    if (!currentlyConnected) {
        m_isConnected = false;
        // Don't try to connect if we are shutting down
        if (!m_running || !m_hw.Connect(IFR1::VENDOR_ID, IFR1::PRODUCT_ID)) {
            return;
        }
        m_isConnected = true;
        // Clear queues on fresh connection to avoid stale reports/updates
        m_inputQueue.Clear();
        m_outputQueue.Clear();
        m_lastReport.fill(0);
    } else {
        m_isConnected = true;
    }

    // 1. Read from device
    int bytesRead = m_hw.Read(readBuffer, IFR1::HID_REPORT_SIZE, 10); // 10ms timeout
    int reportsRead = 0;
    while (bytesRead > 0) {
        // Require at least 8 bytes so all fields accessed by ParseReport are valid
        if (bytesRead >= 8) {
            m_inputQueue.Push(ParseReport(readBuffer));
        } else {
            QueueWorkerDiagnostic(
                std::format("Partial HID read ({} bytes); expected at least 8 — report discarded", bytesRead));
        }
        if (++reportsRead >= 10) break;
        // Clear the buffer before the next read to avoid stale bytes from a previous partial read
        std::fill(std::begin(readBuffer), std::end(readBuffer), uint8_t{0});
        bytesRead = m_hw.Read(readBuffer, IFR1::HID_REPORT_SIZE, 0); // No timeout for subsequent reads
    }

    if (bytesRead < 0) {
        m_hw.Disconnect();
        m_isConnected = false;
        return;
    }

    // 2. Write to device
    while (auto ledBits = m_outputQueue.Pop()) {
        uint8_t report[2] = { IFR1::HID_LED_REPORT_ID, *ledBits };
        if (m_hw.Write(report, 2) < 0) {
            m_hw.Disconnect();
            m_isConnected = false;
            break;
        }
    }
}

void DeviceHandler::WorkerThread() {
    while (m_running || (m_isConnected && !m_outputQueue.IsEmpty())) {
        bool wasConnected = m_isConnected;
        ProcessHardware();
        
        // Small sleep to prevent tight loop if Read is non-blocking or returns immediately
        // or if we're waiting for reconnection
        if (!m_isConnected && !wasConnected && m_running) {
             std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } else {
             std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}
