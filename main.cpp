/* SPDX-License-Identifier: Apache-2.0 */

#include "AppleHPMLib.h"
#include "ssops.h"
#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

struct failure : public std::runtime_error {
    failure(const char *x) : std::runtime_error(x)
    {
    }
};

struct IOObjectDeleter {
    io_object_t arg;

    IOObjectDeleter(io_object_t arg) : arg(arg)
    {
    }

    ~IOObjectDeleter()
    {
        if (arg != 0) {
            IOObjectRelease(arg);
        }
    }
};

struct HPMPluginInstance {
    IOCFPlugInInterface **plugin = nullptr;
    AppleHPMLib **device = nullptr;

    HPMPluginInstance(io_service_t service)
    {
        SInt32 score;
        IOReturn ret = IOCreatePlugInInterfaceForService(service, kAppleHPMLibType,
                                                         kIOCFPlugInInterfaceID, &plugin, &score);
        if (ret != kIOReturnSuccess)
            throw failure("IOCreatePlugInInterfaceForService failed");

        HRESULT res = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kAppleHPMLibInterface),
                                                (LPVOID *)&device);
        if (res != S_OK) {
            IODestroyPlugInInterface(plugin);
            plugin = nullptr;
            throw failure("QueryInterface failed");
        }
    }

    ~HPMPluginInstance()
    {
        if (plugin) {
            // Silently exit DBMa mode; ignore errors during teardown.
            if (device)
                this->command(0, 'DBMa', "\x00");
            // IODestroyPlugInInterface releases all interfaces obtained from the plugin,
            // including the device interface obtained via QueryInterface.
            IODestroyPlugInInterface(plugin);
            plugin = nullptr;
            device = nullptr;
        }
    }

    std::string readRegister(uint64_t chipAddr, uint8_t dataAddr, int flags = 0)
    {
        if (!device)
            throw failure("readRegister failed: device not initialized");

        std::string ret;
        ret.resize(64);
        uint64_t rlen = 0;
        IOReturn x = (*device)->Read(device, chipAddr, dataAddr, &ret[0], 64, flags, &rlen);
        if (x != 0)
            throw failure("readRegister failed");
        ret.resize(rlen); // trim to actual bytes read from device
        return ret;
    }

    int command(uint64_t chipAddr, uint32_t cmd, std::string args = "")
    {
        if (!device)
            throw failure("command failed: device not initialized");

        if (args.length())
            (*device)->Write(device, chipAddr, 9, args.data(), args.length(), 0);

        auto ret = (*device)->Command(device, chipAddr, cmd, 0);
        if (ret)
            return -1;

        auto res = this->readRegister(chipAddr, 9);
        if (res.empty())
            return -1; // guard: device returned zero bytes
        return res[0] & 0xfu;
    }
};

uint32_t GetUnlockKey()
{
    CFMutableDictionaryRef matching = IOServiceMatching("IOPlatformExpertDevice");
    if (!matching)
        throw failure("IOServiceMatching failed (IOPED)");

    // IOServiceGetMatchingService always consumes `matching` — do NOT CFRelease it afterward.
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    if (!service)
        throw failure("IOServiceGetMatchingService failed (IOPED)");

    IOObjectDeleter deviceDel(service);

    io_name_t deviceName;
    if (IORegistryEntryGetName(service, deviceName) != kIOReturnSuccess)
        throw failure("IORegistryEntryGetName failed (IOPED)");

    return ((uint8_t)deviceName[0] << 24) | ((uint8_t)deviceName[1] << 16) |
           ((uint8_t)deviceName[2] << 8)  |  (uint8_t)deviceName[3];
}

std::vector<std::unique_ptr<HPMPluginInstance>> FindDevices()
{
    std::vector<std::unique_ptr<HPMPluginInstance>> devices;
    const int MAX_RETRIES = 5;
    const int RETRY_DELAY_MS = 1000;

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        // IOServiceGetMatchingServices consumes the dictionary, so recreate it each iteration.
        CFMutableDictionaryRef matching = IOServiceMatching("AppleHPM");
        if (!matching)
            throw failure("IOServiceMatching failed");

        io_iterator_t iter = 0;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) != kIOReturnSuccess) {
            // matching already consumed by IOServiceGetMatchingServices — do NOT CFRelease it.
            throw failure("IOServiceGetMatchingServices failed");
        }
        // matching has been consumed — do NOT CFRelease it.

        IOObjectDeleter iterDel(iter);

        io_service_t device;
        while ((device = IOIteratorNext(iter))) {
            IOObjectDeleter deviceDel(device);
            io_string_t pathName;

            if (IORegistryEntryGetPath(device, kIOServicePlane, pathName) != kIOReturnSuccess)
                continue;

            try {
                auto instance = std::make_unique<HPMPluginInstance>(device);
                usleep(RETRY_DELAY_MS * 1000); // give device time to initialize
                devices.push_back(std::move(instance));
            } catch (const failure&) {
                // Skip devices that fail to initialize
            }
        }

        if (!devices.empty())
            return devices;

        usleep(RETRY_DELAY_MS * 1000);
    }

    throw failure("No HPM devices found.");
}

void UnlockAce(HPMPluginInstance &inst, int no, uint32_t key)
{
    std::stringstream args;
    put(args, key);
    if (inst.command(no, 'LOCK', args.str())) {
        // First attempt failed — try a reset then retry
        if (inst.command(no, 'Gaid'))
            throw failure("Failed to unlock device");
        if (inst.command(no, 'LOCK', args.str()))
            throw failure("Failed to unlock device");
    }
}

void DoVDM(HPMPluginInstance &inst, int no, std::vector<uint32_t> vdm)
{
    auto rs = inst.readRegister(no, 0x4d);
    if (rs.empty())
        throw failure("Failed to read VDM status register");
    uint8_t rxst = rs[0];

    std::stringstream args;
    put(args, (uint8_t)(((3 << 4) | vdm.size())));
    for (uint32_t i : vdm)
        put(args, i);

    if (inst.command(no, 'VDMs', args.str()))
        throw failure("Failed to send VDM");

    int i;
    for (i = 0; i < 16; i++) {
        usleep(10000); // 10ms between polls — avoid busy-wait
        rs = inst.readRegister(no, 0x4d);
        if (!rs.empty() && (uint8_t)rs[0] != rxst)
            break;
    }
    if (i >= 16)
        throw failure("No reply to VDM");

    uint32_t vdmhdr;
    std::stringstream reply;
    reply.str(rs);
    get(reply, rxst);
    get(reply, vdmhdr);

    if (vdmhdr != (vdm[0] | 0x40))
        throw failure("VDM reply header mismatch");
}

void DoDFU(HPMPluginInstance &inst, int no)
{
    std::vector<uint32_t> dfu{0x5ac8012, 0x106, 0x80010000};
    DoVDM(inst, no, dfu);
}

int main(int argc, char **argv)
{
    printf("Apple Silicon DFU Tool\n\n");

    try {
        uint32_t key = GetUnlockKey();
        auto devices = FindDevices();
        int dfuCount = 0;

        for (auto& inst : devices) {
            for (int no = 0; no < 5; ++no) {
                try {
                    // Skip ports with no active connection
                    auto t = inst->readRegister(no, 0x3f);
                    if (t.empty() || !(t[0] & 1))
                        continue;

                    // Enter DBMa mode if not already in it
                    auto res = inst->readRegister(no, 0x03);
                    auto np = res.find('\0');
                    if (np != std::string::npos) res.erase(np);

                    if (res != "DBMa") {
                        UnlockAce(*inst, no, key);
                        if (inst->command(no, 'DBMa', "\x01"))
                            throw failure("Failed to enter DBMa mode");

                        res = inst->readRegister(no, 0x03);
                        auto np2 = res.find('\0');
                        if (np2 != std::string::npos) res.erase(np2);
                        if (res != "DBMa")
                            throw failure("Failed to enter DBMa mode");
                    }

                    DoDFU(*inst, no);
                    printf("[Port %d] DFU triggered successfully.\n", no);
                    dfuCount++;

                } catch (const failure& e) {
                    printf("[Port %d] Failed: %s\n", no, e.what());
                }
            }
        }

        printf("\nDone: %d port(s) put into DFU mode.\n", dfuCount);

    } catch (const failure& e) {
        printf("Error: %s\n", e.what());
        return -1;
    }

    return 0;
}
