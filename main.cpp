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
            printf("Exiting DBMa mode... ");
            if (device && this->command(0, 'DBMa', "\x00") == 0)
                printf("OK\n");
            else
                printf("Failed\n");
            // IODestroyPlugInInterface will release all interfaces obtained from the plugin
            // This includes the device interface obtained via QueryInterface
            IODestroyPlugInInterface(plugin);
            plugin = nullptr;
            device = nullptr;
        }
    }

    std::string readRegister(uint64_t chipAddr, uint8_t dataAddr, int flags = 0)
    {
        if (!device) {
            throw failure("readRegister failed: device not initialized");
        }
        std::string ret;
        ret.resize(64);
        uint64_t rlen = 0;
        IOReturn x = (*device)->Read(device, chipAddr, dataAddr, &ret[0], 64, flags, &rlen);
        if (x != 0)
            throw failure("readRegister failed");
        return ret;
    }

    void writeRegister(uint64_t chipAddr, uint8_t dataAddr, std::string value)
    {
        if (!device) {
            throw failure("writeRegister failed: device not initialized");
        }
        IOReturn x = (*device)->Write(device, chipAddr, dataAddr, &value[0], value.length(), 0);
        if (x != 0)
            throw failure("writeRegister failed");
    }

    int command(uint64_t chipAddr, uint32_t cmd, std::string args = "")
    {
        if (!device) {
            throw failure("command failed: device not initialized");
        }
        if (args.length())
            (*device)->Write(device, chipAddr, 9, args.data(), args.length(), 0);
        auto ret = (*device)->Command(device, chipAddr, cmd, 0);
        if (ret)
            return -1;
        auto res = this->readRegister(chipAddr, 9);
        return res[0] & 0xfu;
    }
};

uint32_t GetUnlockKey()
{
    CFMutableDictionaryRef matching = IOServiceMatching("IOPlatformExpertDevice");
    if (!matching)
        throw failure("IOServiceMatching failed (IOPED)");

    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    if (!service) {
        CFRelease(matching);
        throw failure("IOServiceGetMatchingService failed (IOPED)");
    }

    IOObjectDeleter deviceDel(service);

    io_name_t deviceName;
    if (IORegistryEntryGetName(service, deviceName) != kIOReturnSuccess) {
        throw failure("IORegistryEntryGetName failed (IOPED)");
    }

    printf("Mac type: %s\n", deviceName);

    return (deviceName[0] << 24) | (deviceName[1] << 16) | (deviceName[2] << 8) | deviceName[3];
}

std::vector<std::unique_ptr<HPMPluginInstance>> FindDevices()
{
    std::vector<std::unique_ptr<HPMPluginInstance>> devices;
    const int MAX_RETRIES = 5;
    const int RETRY_DELAY_MS = 1000;

    printf("Looking for HPM devices...\n");

    CFMutableDictionaryRef matching = IOServiceMatching("AppleHPM");
    if (!matching)
        throw failure("IOServiceMatching failed");

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        io_iterator_t iter = 0;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) != kIOReturnSuccess) {
            CFRelease(matching);
            throw failure("IOServiceGetMatchingServices failed");
        }

        IOObjectDeleter iterDel(iter);

        io_service_t device;
        while ((device = IOIteratorNext(iter))) {
            IOObjectDeleter deviceDel(device);
            io_string_t pathName;

            if (IORegistryEntryGetPath(device, kIOServicePlane, pathName) != kIOReturnSuccess) {
                continue;
            }

            printf("Found: %s\n", pathName);
            
            try {
                auto instance = std::make_unique<HPMPluginInstance>(device);

                // Give device time to initialize
                usleep(RETRY_DELAY_MS * 1000);

                auto status = instance->readRegister(0, 0x3f);
                printf("Device status: 0x%02x\n", status[0]);

                // Accept all HPM devices, not just those with active connections
                printf("Found HPM device\n");
                devices.push_back(std::move(instance));

            } catch (const failure& e) {
                printf("Error initializing device: %s\n", e.what());
            }
        }

        if (!devices.empty()) {
            CFRelease(matching);
            return devices;
        }
        
        printf("No suitable device found, waiting before retry...\n");
        usleep(RETRY_DELAY_MS * 1000);
    }

    CFRelease(matching);
    throw failure("No suitable devices found after multiple attempts.");
}

void UnlockAce(HPMPluginInstance &inst, int no, uint32_t key)
{
    printf("Unlocking... ");
    std::stringstream args;
    put(args, key);
    if (inst.command(no, 'LOCK', args.str())) {
        printf(" Failed.\n");
        printf("Trying to reset... ");
        if (inst.command(no, 'Gaid')) {
            printf("Failed.\n");
            throw failure("Failed to unlock device");
        }
        printf("OK.\nUnlocking... ");
        if (inst.command(no, 'LOCK', args.str())) {
            printf(" Failed.\n");
            throw failure("Failed to unlock device");
        }
    }

    printf("OK\n");
}

void DoVDM(HPMPluginInstance &inst, int no, std::vector<uint32_t> vdm)
{

    auto rs = inst.readRegister(no, 0x4d);
    uint8_t rxst = rs[0];

    std::stringstream args;
    put(args, (uint8_t)(((3 << 4) | vdm.size())));
    for (uint32_t i : vdm)
        put(args, i);

    if (inst.command(no, 'VDMs', args.str()))
        throw failure("Failed to send VDM\n");

    int i;
    for (i = 0; i < 16; i++) {
        rs = inst.readRegister(no, 0x4d);
        if ((uint8_t)rs[0] != rxst)
            break;
    }
    if (i >= 16)
        throw failure("Did not get a reply to VDM\n");

    uint32_t vdmhdr;
    std::stringstream reply;
    reply.str(rs);
    get(reply, rxst);
    get(reply, vdmhdr);

    if (vdmhdr != (vdm[0] | 0x40)) {
        printf("VDM failed (reply: 0x%08x)\n", vdmhdr);
        throw failure("VDM failed");
    }
}

int DoDFU(HPMPluginInstance &inst, int no)
{
    printf("Rebooting target into DFU mode... ");

    std::vector<uint32_t> dfu{0x5ac8012, 0x106, 0x80010000};
    DoVDM(inst, no, dfu);

    printf("OK\n");
    return 0;
}

int main2(int argc, char **argv)
{
    printf("Apple Silicon DFU Tool\n");
    printf("This tool puts connected Apple Silicon devices into DFU mode.\n\n");

    uint32_t key = GetUnlockKey();

    try {
        auto devices = FindDevices();

        for (auto& inst : devices) {
            try {
                for (int no = 0; no < 5; ++no) {
                    printf("\n=== Port %d ===\n", no);

                    // Read the port status
                    auto t = inst->readRegister(no, 0x3f);
                    std::string type = (t[0] & 1) ? ((t[0] & 2) == 0 ? "Source" : "Sink") : "None";
                    printf("Connection: %s\n", type.c_str());

                    // Check status and enter DBMa mode if needed
                    auto res = inst->readRegister(no, 0x03);
                    res.erase(res.find('\0'));
                    printf("Status: %s\n", res.c_str());

                    if (res != "DBMa") {
                        UnlockAce(*inst, no, key);
                        printf("Entering DBMa mode... ");

                        if (inst->command(no, 'DBMa', "\x01"))
                            throw failure("Failed to enter DBMa mode");

                        res = inst->readRegister(no, 0x03);
                        res.erase(res.find('\0'));
                        printf("Status: %s\n", res.c_str());
                        if (res != "DBMa")
                            throw failure("Failed to enter DBMa mode");
                    }

                    // Perform DFU operation
                    DoDFU(*inst, no);
                }

            } catch (const failure& e) {
                printf("Error processing device: %s\n", e.what());
            }
        }
    } catch (failure& e) {
        printf("Error during device processing: %s\n", e.what());
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    // This makes sure we call the HPMPluginInstance destructor.
    try {
        return main2(argc, argv);
    } catch (failure e) {
        printf("%s\n", e.what());
        return -1;
    }
}
