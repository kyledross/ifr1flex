#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <fstream>
#include "DeviceHandler.h"
#include "SettingsManager.h"
#include "IHardwareManager.h"
#include "XPlaneSDK.h"
#include "IFR1Protocol.h"

namespace {

class MockHardwareManager : public IHardwareManager {
public:
    MOCK_METHOD(bool, Connect, (uint16_t vendorId, uint16_t productId), (override));
    MOCK_METHOD(void, Disconnect, (), (override));
    MOCK_METHOD(bool, IsConnected, (), (const, override));
    MOCK_METHOD(int, Read, (uint8_t* data, size_t length, int timeoutMs), (override));
    MOCK_METHOD(int, Write, (const uint8_t* data, size_t length), (override));
};

class MockXPlaneSDK : public IXPlaneSDK {
public:
    MockXPlaneSDK() {
        ON_CALL(*this, Log(::testing::_, ::testing::_)).WillByDefault(::testing::Return());
        ON_CALL(*this, GetLogLevel()).WillByDefault(::testing::Return(LogLevel::Info));
        ON_CALL(*this, FileExists(::testing::_)).WillByDefault(::testing::Return(true));
    }

    MOCK_METHOD(void*, FindDataRef, (const char* name), (override));
    MOCK_METHOD(int, GetDataRefTypes, (void* dataRef), (override));
    MOCK_METHOD(int, GetDatai, (void* dataRef), (override));
    MOCK_METHOD(void, SetDatai, (void* dataRef, int value), (override));
    MOCK_METHOD(float, GetDataf, (void* dataRef), (override));
    MOCK_METHOD(void, SetDataf, (void* dataRef, float value), (override));
    MOCK_METHOD(int, GetDataiArray, (void* dataRef, int index), (override));
    MOCK_METHOD(void, SetDataiArray, (void* dataRef, int value, int index), (override));
    MOCK_METHOD(float, GetDatafArray, (void* dataRef, int index), (override));
    MOCK_METHOD(void, SetDatafArray, (void* dataRef, float value, int index), (override));
    MOCK_METHOD(int, GetDatab, (void* dataRef, void* outData, int offset, int maxLength), (override));
    MOCK_METHOD(void*, FindCommand, (const char* name), (override));
    MOCK_METHOD(void, CommandOnce, (void* commandRef), (override));
    MOCK_METHOD(void, CommandBegin, (void* commandRef), (override));
    MOCK_METHOD(void, CommandEnd, (void* commandRef), (override));
    MOCK_METHOD(void, Log, (LogLevel level, const char* string), (override));
    MOCK_METHOD(void, SetLogLevel, (LogLevel level), (override));
    MOCK_METHOD(LogLevel, GetLogLevel, (), (const, override));
    MOCK_METHOD(float, GetElapsedTime, (), (override));
    MOCK_METHOD(std::string, GetSystemPath, (), (override));
    MOCK_METHOD(bool, FileExists, (const std::string& path), (override));
    MOCK_METHOD(void, PlaySound, (const std::string& path), (override));
    MOCK_METHOD(void, DrawString, (const float color[4], int x, int y, const char* string), (override));
    MOCK_METHOD(void, DrawRectangle, (const float color[4], int l, int t, int r, int b), (override));
    MOCK_METHOD(void, DrawRectangleOutline, (const float color[4], int l, int t, int r, int b), (override));
    MOCK_METHOD(int, MeasureString, (const char* string), (override));
    MOCK_METHOD(int, GetFontHeight, (), (override));
    MOCK_METHOD(void, GetScreenSize, (int* outWidth, int* outHeight), (override));
    MOCK_METHOD(void*, CreateWindowEx, (const WindowCreateParams& params), (override));
    MOCK_METHOD(void, DestroyWindow, (void* windowId), (override));
    MOCK_METHOD(void, SetWindowVisible, (void* windowId, int visible), (override));
    MOCK_METHOD(void, SetWindowGeometry, (void* windowId, int left, int top, int right, int bottom), (override));
    MOCK_METHOD(void, GetWindowGeometry, (void* windowId, int* outLeft, int* outTop, int* outRight, int* outBottom), (override));
    MOCK_METHOD(void, GetScreenBoundsGlobal, (int* outLeft, int* outTop, int* outRight, int* outBottom), (override));
};

using ::testing::Return;
using ::testing::_;
using ::testing::SetArgPointee;
using ::testing::DoAll;

TEST(DeviceHandlerTest, Update_ConnectsWhenDisconnected) {
    MockHardwareManager mockHw;
    MockXPlaneSDK mockSdk;
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    SettingsManager settings("test_settings.json");
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);
    
    EXPECT_CALL(mockHw, IsConnected())
        .WillOnce(Return(false))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(mockHw, Connect(IFR1::VENDOR_ID, IFR1::PRODUCT_ID)).WillOnce(Return(true));
    
    // ClearLEDs will be called on connect, pushing to output queue
    // ProcessHardware will then pop from output queue and call Write
    EXPECT_CALL(mockHw, Write(_, 2)).WillOnce(Return(2));
    
    // Read will be called in ProcessHardware
    EXPECT_CALL(mockHw, Read(_, _, _)).WillRepeatedly(Return(0));

    nlohmann::json config;
    handler.ProcessHardware(); // Connects
    handler.Update(config, 0.0f); // Detects connection, calls ClearLEDs (pushes to output queue)
    handler.ProcessHardware(); // Pops from output queue and writes to hardware
}

TEST(DeviceHandlerTest, Update_UsesModeDescriptionForDisplay) {
    MockHardwareManager mockHw;
    ::testing::NiceMock<MockXPlaneSDK> mockSdk;
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    
    SettingsManager settings("test_mode_desc_settings.json");
    settings.SetString("osd-position", "lower-left");
    
    EXPECT_CALL(mockSdk, CreateWindowEx(::testing::_)).WillOnce(::testing::Return(reinterpret_cast<void*>(0x1234)));
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);
    
    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(::testing::Return(true));
    EXPECT_CALL(mockHw, Read(::testing::_, ::testing::_, ::testing::_)).WillRepeatedly(::testing::Return(0));
    handler.ProcessHardware(); // Sets m_isConnected to true
    
    nlohmann::json config = {
        {"modes", {
            {"com1", {
                {"description", "Communication Radio 1"}
            }}
        }}
    };
    
    // We expect MeasureString to be called with the UPPERCASE description
    // It's called twice: once in ShowMessage/UpdatePosition and once in Update when checking position
    EXPECT_CALL(mockSdk, MeasureString(::testing::StrEq("COMMUNICATION RADIO 1")))
        .WillRepeatedly(::testing::Return(100));
    EXPECT_CALL(mockSdk, GetFontHeight())
        .WillRepeatedly(::testing::Return(20));
    EXPECT_CALL(mockSdk, GetScreenBoundsGlobal(::testing::_, ::testing::_, ::testing::_, ::testing::_))
        .WillRepeatedly(::testing::Return());
    
    // Trigger a mode change to com1 (default is com1, and m_lastModeString was cleared in Update when not connected)
    handler.ParseModeDescriptions(config);
    outputProc.ParseOutputConfig(config);
    handler.Update(config, 1.0f);
}

TEST(DeviceHandlerTest, Update_UsesModeNameIfDescriptionMissing) {
    MockHardwareManager mockHw;
    ::testing::NiceMock<MockXPlaneSDK> mockSdk;
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    
    SettingsManager settings("test_mode_name_settings.json");
    settings.SetString("osd-position", "lower-left");
    
    EXPECT_CALL(mockSdk, CreateWindowEx(::testing::_)).WillOnce(::testing::Return(reinterpret_cast<void*>(0x1234)));
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);
    
    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(::testing::Return(true));
    EXPECT_CALL(mockHw, Read(::testing::_, ::testing::_, ::testing::_)).WillRepeatedly(::testing::Return(0));
    handler.ProcessHardware(); // Sets m_isConnected to true
    
    nlohmann::json config = {
        {"modes", {
            {"com1", {
                // No description
            }}
        }}
    };
    
    // We expect MeasureString to be called with the UPPERCASE mode name
    // It's called twice: once in ShowMessage/UpdatePosition and once in Update when checking position
    EXPECT_CALL(mockSdk, MeasureString(::testing::StrEq("COM1")))
        .WillRepeatedly(::testing::Return(40));
    EXPECT_CALL(mockSdk, GetFontHeight())
        .WillRepeatedly(::testing::Return(20));
    EXPECT_CALL(mockSdk, GetScreenBoundsGlobal(::testing::_, ::testing::_, ::testing::_, ::testing::_))
        .WillRepeatedly(::testing::Return());
    
    handler.ParseModeDescriptions(config);
    outputProc.ParseOutputConfig(config);
    handler.Update(config, 1.0f);
}

TEST(DeviceHandlerTest, Update_ProcessesKnobRotation) {
    MockHardwareManager mockHw;
    MockXPlaneSDK mockSdk;
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    SettingsManager settings("test_settings.json");
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);
    
    nlohmann::json config = {
        {"modes", {
            {"com1", {
                {"outer-knob", {
                    {"rotate-clockwise", {
                        {"actions", {
                            {{"type", "command"}, {"value", "test_cmd"}}
                        }}
                    }}
                }}
            }}
        }}
    };
    
    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(Return(true));
    
    // Simulate HID report with outer knob rotation = 1
    // data[5] is outer knob, data[7] is mode
    uint8_t report[IFR1::HID_REPORT_SIZE] = {0, 0, 0, 0, 0, 1, 0, 0, 0}; 
    
    EXPECT_CALL(mockHw, Read(_, _, _))
        .WillOnce([&](uint8_t* buf, size_t len, int timeout) {
            std::memcpy(buf, report, IFR1::HID_REPORT_SIZE);
            return IFR1::HID_REPORT_SIZE;
        })
        .WillOnce(Return(0)); // Second call returns 0 to stop loop
    
    void* dummyCmd = reinterpret_cast<void*>(0x1234);
    EXPECT_CALL(mockSdk, FindCommand(::testing::StrEq("test_cmd"))).WillOnce(Return(dummyCmd));
    EXPECT_CALL(mockSdk, CommandOnce(dummyCmd));
    
    handler.ProcessHardware(); // Read report
    handler.Update(config, 0.0f);
}

TEST(DeviceHandlerTest, Update_ProcessesShortPress) {
    MockHardwareManager mockHw;
    MockXPlaneSDK mockSdk;
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    SettingsManager settings("test_settings.json");
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);
    
    nlohmann::json config = {
        {"modes", {
            {"com1", {
                {"swap", {
                    {"short-press", {
                        {"actions", {
                            {{"type", "command"}, {"value", "swap_cmd"}}
                        }}
                    }}
                }}
            }}
        }}
    };
    
    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(Return(true));
    
    // Frame 1: Button pressed
    uint8_t reportPressed[IFR1::HID_REPORT_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    reportPressed[2] |= (1 << (IFR1::BitPosition::SWAP - 1));
    
    EXPECT_CALL(mockHw, Read(_, _, _))
        .WillOnce([&](uint8_t* buf, size_t len, int t) {
            std::memcpy(buf, reportPressed, IFR1::HID_REPORT_SIZE);
            return IFR1::HID_REPORT_SIZE;
        })
        .WillOnce(Return(0));
    
    handler.ProcessHardware(); // Read pressed
    handler.Update(config, 0.0f);
    
    // Frame 2: Button released
    uint8_t reportReleased[IFR1::HID_REPORT_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_CALL(mockHw, Read(_, _, _))
        .WillOnce([&](uint8_t* buf, size_t len, int t) {
            std::memcpy(buf, reportReleased, IFR1::HID_REPORT_SIZE);
            return IFR1::HID_REPORT_SIZE;
        })
        .WillOnce(Return(0));
    
    void* dummyCmd = reinterpret_cast<void*>(0x1234);
    EXPECT_CALL(mockSdk, FindCommand(::testing::StrEq("swap_cmd"))).WillOnce(Return(dummyCmd));
    EXPECT_CALL(mockSdk, CommandOnce(dummyCmd));
    
    handler.ProcessHardware(); // Read released
    handler.Update(config, 0.1f);
}

TEST(DeviceHandlerTest, Update_ResetsShiftedOnModeChange) {
    MockHardwareManager mockHw;
    MockXPlaneSDK mockSdk;
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    SettingsManager settings("test_settings.json");
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);
    
    nlohmann::json config = {
        {"modes", {
            {"com1", {
                {"outer-knob", {
                    {"rotate-clockwise", {
                        {"actions", {
                            {{"type", "command"}, {"value", "com1_cmd"}}
                        }}
                    }}
                }}
            }},
            {"hdg", {
                {"outer-knob", {
                    {"rotate-clockwise", {
                        {"actions", {
                            {{"type", "command"}, {"value", "hdg_cmd"}}
                        }}
                    }}
                }}
            }},
            {"com2", {
                {"outer-knob", {
                    {"rotate-clockwise", {
                        {"actions", {
                            {{"type", "command"}, {"value", "com2_cmd"}}
                        }}
                    }}
                }}
            }}
        }}
    };
    
    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(Return(true));
    
    // 1. Set mode to COM1 and long press INNER_KNOB to shift
    uint8_t reportShiftPressed[IFR1::HID_REPORT_SIZE] = {0, 0, 0x02, 0, 0, 0, 0, 0, 0}; 
    EXPECT_CALL(mockHw, Read(_, _, _))
        .WillOnce([&](uint8_t* buf, size_t len, int t) {
            std::memcpy(buf, reportShiftPressed, IFR1::HID_REPORT_SIZE);
            return IFR1::HID_REPORT_SIZE;
        })
        .WillOnce(Return(0));
    handler.ProcessHardware();
    handler.Update(config, 0.0f);
    
    // Trigger long press (0.5s)
    EXPECT_CALL(mockHw, Read(_, _, _)).WillRepeatedly(Return(0));
    handler.ProcessHardware();
    handler.Update(config, 0.6f); 
    
    // Now shifted should be true. Verify by rotating outer knob (should trigger hdg_cmd)
    uint8_t reportRotate[IFR1::HID_REPORT_SIZE] = {0, 0, 0, 0, 0, 1, 0, 0, 0}; 
    EXPECT_CALL(mockHw, Read(_, _, _))
        .WillOnce([&](uint8_t* buf, size_t len, int t) {
            std::memcpy(buf, reportRotate, IFR1::HID_REPORT_SIZE);
            return IFR1::HID_REPORT_SIZE;
        })
        .WillOnce(Return(0));
    
    void* hdgCmd = reinterpret_cast<void*>(0x1);
    EXPECT_CALL(mockSdk, FindCommand(::testing::StrEq("hdg_cmd"))).WillOnce(Return(hdgCmd));
    EXPECT_CALL(mockSdk, CommandOnce(hdgCmd));
    handler.ProcessHardware();
    handler.Update(config, 0.7f);

    // 2. Change mode to COM2
    uint8_t reportModeChange[IFR1::HID_REPORT_SIZE] = {0, 0, 0, 0, 0, 0, 0, static_cast<uint8_t>(IFR1::Mode::COM2), 0};
    EXPECT_CALL(mockHw, Read(_, _, _))
        .WillOnce([&](uint8_t* buf, size_t len, int t) {
            std::memcpy(buf, reportModeChange, IFR1::HID_REPORT_SIZE);
            return IFR1::HID_REPORT_SIZE;
        })
        .WillOnce(Return(0));
    handler.ProcessHardware();
    handler.Update(config, 0.8f);

    // 3. Rotate outer knob again. It should trigger com2_cmd, NOT its shifted version (which would be baro)
    uint8_t reportRotate2[IFR1::HID_REPORT_SIZE] = {0, 0, 0, 0, 0, 1, 0, static_cast<uint8_t>(IFR1::Mode::COM2), 0}; 
    EXPECT_CALL(mockHw, Read(_, _, _))
        .WillOnce([&](uint8_t* buf, size_t len, int t) {
            std::memcpy(buf, reportRotate2, IFR1::HID_REPORT_SIZE);
            return IFR1::HID_REPORT_SIZE;
        })
        .WillOnce(Return(0));

    void* com2Cmd = reinterpret_cast<void*>(0x2);
    EXPECT_CALL(mockSdk, FindCommand(::testing::StrEq("com2_cmd"))).WillOnce(Return(com2Cmd));
    EXPECT_CALL(mockSdk, CommandOnce(com2Cmd));
    
    handler.ProcessHardware();
    handler.Update(config, 0.9f);
}

TEST(DeviceHandlerTest, ClearLEDs_SendsZeroReportAndResetsState) {
    MockHardwareManager mockHw;
    MockXPlaneSDK mockSdk;
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    SettingsManager settings("test_settings.json");
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);
    
    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(Return(true));
    
    // Expect a write with HID_LED_REPORT_ID and 0
    EXPECT_CALL(mockHw, Write(_, 2))
        .WillOnce([](const uint8_t* data, size_t len) {
            EXPECT_EQ(data[0], IFR1::HID_LED_REPORT_ID);
            EXPECT_EQ(data[1], 0);
            return 2;
        });
    
    handler.ClearLEDs();
    handler.ProcessHardware();
}

TEST(DeviceHandlerTest, UpdateLEDs_PushesToQueueAndWrites) {
    MockHardwareManager mockHw;
    MockXPlaneSDK mockSdk;
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    SettingsManager settings("test_settings.json");
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);

    nlohmann::json config = {
        {"output", {
            {"ap", {
                {"conditions", nlohmann::json::array({
                    {{"dataref", "sim/test/ap"}, {"min", 1}, {"max", 1}, {"mode", "solid"}}
                })}
            }}
        }}
    };

    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(Return(true));
    
    // 1. Initially LEDs are off
    EXPECT_CALL(mockSdk, FindDataRef(::testing::StrEq("sim/test/ap"))).WillRepeatedly(Return(reinterpret_cast<void*>(0x1)));
    EXPECT_CALL(mockSdk, GetDataRefTypes(_)).WillRepeatedly(Return(static_cast<int>(DataRefType::Int)));
    EXPECT_CALL(mockSdk, GetDatai(_)).WillRepeatedly(Return(0));

    // Force connection state in handler
    handler.ProcessHardware(); 
    
    // Should be connected now.
    // Set lastLedBits to 0
    handler.ClearLEDs();
    EXPECT_CALL(mockHw, Write(_, 2)).WillOnce(Return(2)); // From ClearLEDs
    handler.ProcessHardware();

    // 2. Change dataref to turn on AP LED
    EXPECT_CALL(mockSdk, GetDatai(_)).WillRepeatedly(Return(1));
    
    // UpdateLEDs should detect change and push to queue
    handler.ParseModeDescriptions(config);
    outputProc.ParseOutputConfig(config);
    handler.UpdateLEDs(1.0f);
    
    // ProcessHardware should pop and write
    EXPECT_CALL(mockHw, Write(::testing::Pointee(IFR1::HID_LED_REPORT_ID), 2))
        .WillOnce([](const uint8_t* data, size_t len) {
            EXPECT_EQ(data[1], IFR1::LEDMask::AP);
            return 2;
        });
    
    handler.ProcessHardware();
}

TEST(DeviceHandlerTest, Update_OtherButtonLongPressDoesNotPlaySound) {
    MockHardwareManager mockHw;
    MockXPlaneSDK mockSdk;
    
    EXPECT_CALL(mockSdk, GetSystemPath()).WillRepeatedly(Return("/xplane/"));
    
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    SettingsManager settings("test_settings.json");
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);
    
    nlohmann::json config = {
        {"modes", {
            {"com1", {
                {"swap", {
                    {"long-press", {
                        {"actions", {
                            {{"type", "command"}, {"value", "long_cmd"}}
                        }}
                    }}
                }}
            }}
        }}
    };
    
    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(Return(true));
    
    // 1. Press button (swap is bit 0 of byte 2)
    uint8_t pressReport[IFR1::HID_REPORT_SIZE] = {0, 0, 0x01, 0, 0, 0, 0, 0, 0}; 
    EXPECT_CALL(mockHw, Read(_, _, _))
        .WillOnce([&](uint8_t* buf, size_t len, int timeout) {
            std::memcpy(buf, pressReport, IFR1::HID_REPORT_SIZE);
            return IFR1::HID_REPORT_SIZE;
        })
        .WillOnce(Return(0));
    
    handler.ProcessHardware();
    handler.Update(config, 0.0f);
    
    // 2. Advance time for long press
    EXPECT_CALL(mockHw, Read(_, _, _)).WillRepeatedly(Return(0));
    
    void* dummyCmd = reinterpret_cast<void*>(0x1234);
    EXPECT_CALL(mockSdk, FindCommand(::testing::StrEq("long_cmd"))).WillOnce(Return(dummyCmd));
    EXPECT_CALL(mockSdk, CommandOnce(dummyCmd));
    
    // Should NOT play sound
    EXPECT_CALL(mockSdk, PlaySound(_)).Times(0);
    
    handler.ProcessHardware();
    handler.Update(config, 0.4f);
}

TEST(DeviceHandlerTest, Update_InnerKnobLongPressPlaysSound) {
    MockHardwareManager mockHw;
    MockXPlaneSDK mockSdk;
    
    EXPECT_CALL(mockSdk, FileExists(::testing::_)).WillRepeatedly(::testing::Return(true));
    EXPECT_CALL(mockSdk, GetSystemPath()).WillRepeatedly(Return("/xplane/"));
    
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    SettingsManager settings("test_settings.json");
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);
    
    nlohmann::json config = {
        {"modes", {
            {"com1", {}}
        }}
    };
    
    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(Return(true));
    
    // INNER_KNOB is bit 1 of byte 2 (0x02)
    uint8_t pressReport[IFR1::HID_REPORT_SIZE] = {0, 0, 0x02, 0, 0, 0, 0, 0, 0}; 
    EXPECT_CALL(mockHw, Read(_, _, _))
        .WillOnce([&](uint8_t* buf, size_t len, int timeout) {
            std::memcpy(buf, pressReport, IFR1::HID_REPORT_SIZE);
            return IFR1::HID_REPORT_SIZE;
        })
        .WillOnce(Return(0));
    
    handler.ProcessHardware();
    handler.Update(config, 0.0f);
    
    EXPECT_CALL(mockHw, Read(_, _, _)).WillRepeatedly(Return(0));
    // Should play sound for inner knob
    EXPECT_CALL(mockSdk, PlaySound(::testing::StrEq("/xplane/Resources/sounds/systems/click.wav")));
    
    handler.ProcessHardware();
    handler.Update(config, 0.4f);
}

TEST(DeviceHandlerTest, Update_InnerKnobLongPressDoesNotPlaySoundIfFileNotFound) {
    MockHardwareManager mockHw;
    MockXPlaneSDK mockSdk;
    
    EXPECT_CALL(mockSdk, GetSystemPath()).WillRepeatedly(Return("/xplane/"));
    EXPECT_CALL(mockSdk, FileExists(::testing::StrEq("/xplane/Resources/sounds/systems/click.wav")))
        .WillOnce(Return(false));
    
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    SettingsManager settings("test_settings.json");
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);
    
    nlohmann::json config = {
        {"modes", {
            {"com1", {}}
        }}
    };
    
    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(Return(true));
    
    // INNER_KNOB is bit 1 of byte 2 (0x02)
    uint8_t pressReport[IFR1::HID_REPORT_SIZE] = {0, 0, 0x02, 0, 0, 0, 0, 0, 0}; 
    EXPECT_CALL(mockHw, Read(_, _, _))
        .WillOnce([&](uint8_t* buf, size_t len, int timeout) {
            std::memcpy(buf, pressReport, IFR1::HID_REPORT_SIZE);
            return IFR1::HID_REPORT_SIZE;
        })
        .WillOnce(Return(0));
    
    handler.ProcessHardware();
    handler.Update(config, 0.0f);
    
    EXPECT_CALL(mockHw, Read(_, _, _)).WillRepeatedly(Return(0));
    // Should NOT play sound if file not found
    EXPECT_CALL(mockSdk, PlaySound(_)).Times(0);
    
    handler.ProcessHardware();
    handler.Update(config, 0.4f);
}

TEST(DeviceHandlerTest, FlushWorkerDiagnostics_LogsPartialHIDReadOnMainThread) {
    MockHardwareManager mockHw;
    MockXPlaneSDK mockSdk;
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    SettingsManager settings("test_settings.json");
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);

    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(Return(true));
    EXPECT_CALL(mockHw, Read(_, _, _))
        .WillOnce(Return(7))
        .WillOnce(Return(0));
    EXPECT_CALL(mockSdk, Log(_, _)).Times(0);

    handler.ProcessHardware();

    ::testing::Mock::VerifyAndClearExpectations(&mockSdk);
    EXPECT_CALL(mockSdk, Log(
        LogLevel::Error,
        ::testing::HasSubstr("Partial HID read (7 bytes); expected at least 8")))
        .Times(1);
    handler.FlushWorkerDiagnostics();
}

TEST(DeviceHandlerTest, FlushWorkerDiagnostics_LogsUnknownModeOnMainThread) {
    MockHardwareManager mockHw;
    MockXPlaneSDK mockSdk;
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    SettingsManager settings("test_settings.json");
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);

    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(Return(true));
    uint8_t report[IFR1::HID_REPORT_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0xFF, 0};
    EXPECT_CALL(mockHw, Read(_, _, _))
        .WillOnce([&](uint8_t* buffer, size_t, int) {
            std::memcpy(buffer, report, IFR1::HID_REPORT_SIZE);
            return IFR1::HID_REPORT_SIZE;
        })
        .WillOnce(Return(0));
    EXPECT_CALL(mockSdk, Log(_, _)).Times(0);

    handler.ProcessHardware();

    ::testing::Mock::VerifyAndClearExpectations(&mockSdk);
    EXPECT_CALL(mockSdk, Log(
        LogLevel::Error,
        ::testing::HasSubstr("HID report contained unknown mode byte 0xFF; defaulting to COM1")))
        .Times(1);
    handler.FlushWorkerDiagnostics();
}

TEST(DeviceHandlerTest, Update_UsesNewButtonNamesInFMSMode) {
    MockHardwareManager mockHw;
    MockXPlaneSDK mockSdk;
    EventProcessor eventProc(mockSdk);
    OutputProcessor outputProc(mockSdk);
    SettingsManager settings("test_settings.json");
    DeviceHandler handler(mockHw, eventProc, outputProc, settings, mockSdk, false);
    
    nlohmann::json config = {
        {"modes", {
            {"fms1", {
                {"cdi", {{"short-press", {{"actions", {{{"type", "command"}, {"value", "cdi_cmd"}}}}}}}},
                {"obs", {{"short-press", {{"actions", {{{"type", "command"}, {"value", "obs_cmd"}}}}}}}},
                {"msg", {{"short-press", {{"actions", {{{"type", "command"}, {"value", "msg_cmd"}}}}}}}},
                {"fpl", {{"short-press", {{"actions", {{{"type", "command"}, {"value", "fpl_cmd"}}}}}}}},
                {"vnav", {{"short-press", {{"actions", {{{"type", "command"}, {"value", "vnav_cmd"}}}}}}}},
                {"proc", {{"short-press", {{"actions", {{{"type", "command"}, {"value", "proc_cmd"}}}}}}}}
            }}
        }}
    };
    
    EXPECT_CALL(mockHw, IsConnected()).WillRepeatedly(Return(true));
    
    auto testButton = [&](uint8_t byteIndex, uint8_t bitPos, const std::string& expectedCmd) {
        // Press
        uint8_t reportPressed[IFR1::HID_REPORT_SIZE] = {0, 0, 0, 0, 0, 0, 0, static_cast<uint8_t>(IFR1::Mode::FMS1), 0};
        reportPressed[byteIndex] |= (1 << (bitPos - 1));
        
        EXPECT_CALL(mockHw, Read(_, _, _))
            .WillOnce([&](uint8_t* buf, size_t len, int t) {
                std::memcpy(buf, reportPressed, IFR1::HID_REPORT_SIZE);
                return IFR1::HID_REPORT_SIZE;
            })
            .WillOnce(Return(0));
        
        handler.ProcessHardware();
        handler.Update(config, 0.0f);
        
        // Release
        uint8_t reportReleased[IFR1::HID_REPORT_SIZE] = {0, 0, 0, 0, 0, 0, 0, static_cast<uint8_t>(IFR1::Mode::FMS1), 0};
        EXPECT_CALL(mockHw, Read(_, _, _))
            .WillOnce([&](uint8_t* buf, size_t len, int t) {
                std::memcpy(buf, reportReleased, IFR1::HID_REPORT_SIZE);
                return IFR1::HID_REPORT_SIZE;
            })
            .WillOnce(Return(0));
        
        void* dummyCmd = reinterpret_cast<void*>(0x1234);
        EXPECT_CALL(mockSdk, FindCommand(::testing::StrEq(expectedCmd))).WillOnce(Return(dummyCmd));
        EXPECT_CALL(mockSdk, CommandOnce(dummyCmd));
        
        handler.ProcessHardware();
        handler.Update(config, 0.1f);
    };

    testButton(2, IFR1::BitPosition::AP, "cdi_cmd");
    testButton(2, IFR1::BitPosition::HDG, "obs_cmd");
    testButton(3, IFR1::BitPosition::NAV, "msg_cmd");
    testButton(3, IFR1::BitPosition::APR, "fpl_cmd");
    testButton(3, IFR1::BitPosition::ALT, "vnav_cmd");
    testButton(3, IFR1::BitPosition::VS, "proc_cmd");
}
}
