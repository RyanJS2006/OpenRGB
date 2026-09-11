/*---------------------------------------------------------*\
| razer-direct.cpp                                          |
|                                                           |
|   Hardware-free Razer frame and mode regression tests     |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <cassert>
#include <iostream>
#include <mutex>
#include <vector>
#include "RGBController_Razer.h"
#include "RazerDevices.h"
#include "LogManager.h"

static std::mutex reports_mutex;
static std::vector<razer_report> reports;
static int write_result = sizeof(razer_report);
static unsigned int errors = 0;

class TestRazer : public RGBController_Razer
{
public:
    using RGBController_Razer::RGBController_Razer;
    using RGBController::colors;
    using RGBController::leds;
    using RGBController::modes;
    using RGBController::zones;
    using RGBController::active_mode;
};

/* Only the HID transport and application logger are substituted. */
LogManager::LogManager() {}
LogManager::~LogManager() {}
LogManager* LogManager::get()
{
    static LogManager logger;
    return &logger;
}
void LogManager::LogEntry(const char*, int, unsigned int level, const char*, ...)
{
    if(level == LL_ERROR) errors++;
}

extern "C" int HID_API_CALL hid_send_feature_report(hid_device*, const unsigned char* data, size_t size)
{
    assert(size == sizeof(razer_report));
    std::lock_guard<std::mutex> lock(reports_mutex);
    razer_report report;
    memcpy(&report, data, size);
    reports.push_back(report);
    return write_result;
}
extern "C" int HID_API_CALL hid_get_feature_report(hid_device*, unsigned char* data, size_t size)
{
    memset(data, 0, size);
    return (int)size;
}
extern "C" void HID_API_CALL hid_close(hid_device*) {}

static std::vector<razer_report> TakeReports()
{
    std::lock_guard<std::mutex> lock(reports_mutex);
    std::vector<razer_report> result;
    result.swap(reports);
    return result;
}

static void WaitForReports(unsigned int count)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while(std::chrono::steady_clock::now() < deadline)
    {
        {
            std::lock_guard<std::mutex> lock(reports_mutex);
            if(reports.size() >= count) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "Timed out waiting for the device call thread");
}

static void CheckFrame(const std::vector<razer_report>& frame, const std::vector<RGBColor>& colors,
                       unsigned char transaction, unsigned int rows, unsigned int cols)
{
    if(frame.size() != rows + 1)
        std::cerr << "Expected " << rows + 1 << " frame reports, received " << frame.size() << "\n";
    assert(frame.size() == rows + 1);
    for(unsigned int row = 0; row < rows; row++)
    {
        const razer_report& report = frame[row];
        assert(report.transaction_id.id == transaction);
        assert(report.command_class == 0x0F && report.command_id.id == 0x03);
        assert(report.data_size == 5 + cols * 3);
        assert(report.arguments[0] == 0 && report.arguments[1] == 0);
        assert(report.arguments[2] == row && report.arguments[3] == 0);
        assert(report.arguments[4] == cols - 1);
        for(unsigned int col = 0; col < cols; col++)
        {
            RGBColor color = colors[row * cols + col];
            assert(report.arguments[5 + col * 3] == RGBGetRValue(color));
            assert(report.arguments[6 + col * 3] == RGBGetGValue(color));
            assert(report.arguments[7 + col * 3] == RGBGetBValue(color));
        }
        for(unsigned int i = report.data_size; i < sizeof(report.arguments); i++)
            assert(report.arguments[i] == 0);
    }
    const razer_report& apply = frame.back();
    assert(apply.command_class == 0x0F && apply.command_id.id == 0x02);
    assert(apply.arguments[0] == RAZER_STORAGE_NO_SAVE && apply.arguments[2] == 0x08);
    for(const razer_report& report : frame)
    {
        const unsigned char* bytes = (const unsigned char*)&report;
        unsigned char crc = 0;
        for(unsigned int i = 3; i < 89; i++) crc ^= bytes[i];
        assert(report.crc == crc);
    }
}

static void TestDevice(unsigned short pid, unsigned char transaction, unsigned int rows, unsigned int cols)
{
    std::cout << "Testing PID " << std::hex << pid << std::dec << std::endl;
    TestRazer rgb(new RazerController(NULL, NULL, "mock", pid, "mock"));
    TakeReports();
    assert(rgb.colors.size() == rows * cols && rgb.leds.size() == rows * cols);
    assert(rgb.modes[0].name == "Direct" && rgb.modes[0].color_mode == MODE_COLORS_PER_LED);
    assert(rgb.modes[0].flags & MODE_FLAG_HAS_PER_LED_COLOR);
    assert(rgb.modes[0].flags & MODE_FLAG_HAS_BRIGHTNESS);
    if(pid == RAZER_LAPTOP_COOLING_PAD_PID)
    {
        assert(rgb.zones.size() == 1 && rgb.zones[0].type == ZONE_TYPE_LINEAR);
        assert(rgb.zones[0].leds_count == 18 && rgb.zones[0].leds_min == 18 && rgb.zones[0].leds_max == 18);
        assert(rgb.zones[0].colors == rgb.colors.data());
    }
    for(unsigned int i = 0; i < rgb.colors.size(); i++)
        rgb.colors[i] = ToRGBColor(i * 11, 255 - i * 7, i * 3);
    rgb.DeviceUpdateLEDs();
    CheckFrame(TakeReports(), rgb.colors, transaction, rows, cols);
    rgb.UpdateZoneLEDs(0);
    CheckFrame(TakeReports(), rgb.colors, transaction, rows, cols);
    rgb.colors.back() = ToRGBColor(255, 0, 128);
    rgb.UpdateSingleLED((int)rgb.colors.size() - 1);
    CheckFrame(TakeReports(), rgb.colors, transaction, rows, cols);
    rgb.active_mode = 2; // Static
    rgb.DeviceUpdateMode();
    TakeReports();
    rgb.DeviceUpdateLEDs();
    rgb.UpdateZoneLEDs(0);
    rgb.UpdateSingleLED(0);
    assert(TakeReports().empty());
    rgb.active_mode = 0;
    rgb.DeviceUpdateMode();
    std::vector<razer_report> transition = TakeReports();
    assert(transition.back().command_id.id == 0x04); // Brightness only on mode updates
    transition.pop_back();
    CheckFrame(transition, rgb.colors, transaction, rows, cols);
    std::thread zone([&rgb]() { rgb.UpdateZoneLEDs(0); });
    std::thread single([&rgb]() { rgb.UpdateSingleLED(0); });
    zone.join();
    single.join();
    std::vector<razer_report> concurrent = TakeReports();
    assert(concurrent.size() == 2 * (rows + 1));
    CheckFrame({concurrent.begin(), concurrent.begin() + rows + 1}, rgb.colors, transaction, rows, cols);
    CheckFrame({concurrent.begin() + rows + 1, concurrent.end()}, rgb.colors, transaction, rows, cols);
    std::thread mode_update([&rgb]() { rgb.DeviceUpdateMode(); });
    std::thread frame_update([&rgb]() { rgb.UpdateZoneLEDs(0); });
    mode_update.join();
    frame_update.join();
    concurrent = TakeReports();
    assert(concurrent.size() == 2 * (rows + 1) + 1);
    CheckFrame({concurrent.begin(), concurrent.begin() + rows + 1}, rgb.colors, transaction, rows, cols);
    unsigned int brightness_index = concurrent[rows + 1].command_id.id == 0x04 ? rows + 1 : 2 * (rows + 1);
    assert(concurrent[brightness_index].command_id.id == 0x04);
    concurrent.erase(concurrent.begin() + brightness_index);
    CheckFrame({concurrent.begin() + rows + 1, concurrent.end()}, rgb.colors, transaction, rows, cols);
    bool starlight = false;
    for(unsigned int i = 0; i < rgb.modes.size(); i++)
    {
        if(rgb.modes[i].value != RAZER_MODE_STARLIGHT) continue;
        starlight = true;
        rgb.active_mode = i;
        for(unsigned int count = 0; count <= 2; count++)
        {
            rgb.modes[i].color_mode = count ? MODE_COLORS_MODE_SPECIFIC : MODE_COLORS_RANDOM;
            rgb.modes[i].colors.resize(count, ToRGBColor(17, 34, 51));
            for(unsigned int speed = 1; speed <= 3; speed++)
            {
                rgb.modes[i].speed = speed;
                rgb.DeviceUpdateMode();
                std::vector<razer_report> effect = TakeReports();
                assert(effect.size() == 2);
                assert(effect[0].arguments[0] == RAZER_STORAGE_NO_SAVE);
                assert(effect[0].arguments[2] == 7 && effect[0].arguments[4] == speed);
                assert(effect[0].arguments[5] == count && effect[0].data_size == 6 + count * 3);
            }
        }
    }
    assert(starlight == (pid == RAZER_LAPTOP_COOLING_PAD_PID));
}

int main()
{
    static_assert(RAZER_MODE_WAVE == 5 && RAZER_MODE_REACTIVE == 6, "Preserve existing mode values");
    TestDevice(RAZER_LAPTOP_COOLING_PAD_PID, 0x1F, 1, 18);
    TestDevice(RAZER_BASE_STATION_V2_CHROMA_PID, 0x1F, 1, 8);
    TestDevice(RAZER_LAPTOP_STAND_CHROMA_V2_PID, 0x1F, 1, 15);
    TestDevice(RAZER_NOMMO_PRO_PID, 0x3F, 2, 8);
    TestRazer rgb(new RazerController(NULL, NULL, "mock", RAZER_LAPTOP_COOLING_PAD_PID, "mock"));
    TakeReports();
    rgb.SetCustomMode();
    WaitForReports(3);
    TakeReports();
    rgb.SetColor(17, ToRGBColor(255, 0, 0));
    rgb.UpdateLEDs();
    WaitForReports(2);
    CheckFrame(TakeReports(), rgb.colors, 0x1F, 1, 18);
    std::cout << "PASS: topology, frame bytes/CRC, transitions, zone/single/async updates, concurrency, Starlight, neighboring devices\n";
}
