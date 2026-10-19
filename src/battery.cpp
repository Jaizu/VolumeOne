#include "battery.h"

#include <initguid.h>  // defines the DEVPKEY_ values instead of only declaring them
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <cmath>
#include <map>
#include <mutex>
#include <thread>

// How the levels are read. The protocols are the ones Halo Battery
// (github.com/HeyOkay/HaloBattery, MIT) documents in docs/protocols.md, which in
// turn come from HeadsetControl, Solaar and LennardKittner/HyperHeadset. Every
// request only reads: nothing that changes a setting on a device is ever sent.
//
// * Bluetooth: DEVPKEY_Bluetooth_Battery on the device's PnP nodes, the level
//   Windows shows in Settings > Bluetooth & devices.
// * Logitech HID++ 2.0 headsets (G533, G535, G633/635, G733, G933/935, G PRO,
//   PRO X, PRO X 2 on HID++): the headset's long collection (ff43:0202, the G535
//   000c:0001), device index 0xFF. Root feature 0 finds a battery feature:
//   0x1004 unified battery, 0x1000 battery status, 0x1001 voltage or 0x1F20 ADC.
// * Logitech Centurion (G PRO X 2 LIGHTSPEED 0AF7 with report 0x51, G522 0B18
//   with the addressed prefix 50 23): vendor collection ffa0:0001. The receiver's
//   feature set gives the bridge feature (0x0003), through which the headset's
//   feature set gives battery feature 0x0104: percent, _, charge state.
// * SteelSeries Arctis Nova 7 / Nova 5 / 7+ / GameBuds: 00 b0 on collection
//   ffc0, the reply layouts per model as HeadsetControl documents them.
// * HyperX Cloud II Wireless (06 ff bb <cmd> on ff90:0303), Cloud III Wireless
//   and Cloud II Core Wireless (66 <cmd> on ff13:0001), Cloud Alpha 2 (50 02 on
//   ff13:ff00).

namespace {

// {104EA319-6EE2-4701-BD47-8DDBF425BBE5}, 2: DEVPKEY_Bluetooth_Battery (a byte, percent)
const DEVPROPKEY kBluetoothBattery = {{0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2};
// Internal devices without a container of their own all share this one.
const wchar_t kNoContainer[] = L"{00000000-0000-0000-ffff-ffffffffffff}";

const DWORD kPollMs = 30000;
const DWORD kMinGapMs = 5000;  // between two polls asked for with Refresh()

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring GuidString(const GUID& g) {
    wchar_t buf[40];
    StringFromGUID2(g, buf, 40);
    return Lower(buf);
}

std::wstring DevNodeContainer(DEVINST dn) {
    GUID g;
    DEVPROPTYPE type;
    ULONG size = sizeof(g);
    if (CM_Get_DevNode_PropertyW(dn, &DEVPKEY_Device_ContainerId, &type, (PBYTE)&g, &size, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_GUID)
        return {};
    std::wstring s = GuidString(g);
    return s == kNoContainer ? std::wstring() : s;
}

// ---------------------------------------------------------------------------
// HID access

class HidDev {
public:
    ~HidDev() { Close(); }

    bool Open(const std::wstring& path) {
        h_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            h_ = nullptr;
            return false;
        }
        PHIDP_PREPARSED_DATA pp;
        if (HidD_GetPreparsedData(h_, &pp)) {
            HidP_GetCaps(pp, &caps_);
            HidD_FreePreparsedData(pp);
        }
        readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return caps_.InputReportByteLength > 0;
    }

    void Close() {
        if (h_) {
            if (pending_) {
                CancelIoEx(h_, &read_);
                DWORD n;
                GetOverlappedResult(h_, &read_, &n, TRUE);
                pending_ = false;
            }
            CloseHandle(h_);
            h_ = nullptr;
        }
        if (readEvent_) CloseHandle(readEvent_);
        if (writeEvent_) CloseHandle(writeEvent_);
        readEvent_ = writeEvent_ = nullptr;
    }

    // An output report; data[0] is the report id. Padded to the report length.
    bool Write(const std::vector<BYTE>& data) {
        if (!caps_.OutputReportByteLength || data.size() > caps_.OutputReportByteLength) return false;
        std::vector<BYTE> buf(caps_.OutputReportByteLength);
        std::copy(data.begin(), data.end(), buf.begin());
        OVERLAPPED o{};
        o.hEvent = writeEvent_;
        ResetEvent(writeEvent_);
        DWORD n = 0;
        if (!WriteFile(h_, buf.data(), (DWORD)buf.size(), nullptr, &o)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            if (WaitForSingleObject(writeEvent_, 1000) != WAIT_OBJECT_0) {
                CancelIoEx(h_, &o);
                GetOverlappedResult(h_, &o, &n, TRUE);
                return false;
            }
        }
        return GetOverlappedResult(h_, &o, &n, FALSE) != 0;
    }

    // A feature report, for dongles that take their requests only that way.
    bool SetFeature(const std::vector<BYTE>& data) {
        if (!caps_.FeatureReportByteLength || data.size() > caps_.FeatureReportByteLength) return false;
        std::vector<BYTE> buf(caps_.FeatureReportByteLength);
        std::copy(data.begin(), data.end(), buf.begin());
        return HidD_SetFeature(h_, buf.data(), (ULONG)buf.size()) != 0;
    }

    // The next input report (report id first), or empty after `ms`.
    std::vector<BYTE> Read(DWORD ms) {
        if (!pending_) {
            buf_.assign(caps_.InputReportByteLength, 0);
            read_ = {};
            read_.hEvent = readEvent_;
            ResetEvent(readEvent_);
            if (!ReadFile(h_, buf_.data(), (DWORD)buf_.size(), nullptr, &read_) && GetLastError() != ERROR_IO_PENDING)
                return {};
            pending_ = true;
        }
        if (WaitForSingleObject(readEvent_, ms) != WAIT_OBJECT_0) return {};
        pending_ = false;
        DWORD n = 0;
        if (!GetOverlappedResult(h_, &read_, &n, FALSE)) return {};
        return std::vector<BYTE>(buf_.begin(), buf_.begin() + n);
    }

    // Reads until `match` accepts a report or `ms` have passed.
    template <class F> bool ReadUntil(DWORD ms, F match) {
        ULONGLONG end = GetTickCount64() + ms;
        for (;;) {
            ULONGLONG now = GetTickCount64();
            if (now >= end) return false;
            auto r = Read((DWORD)(end - now));
            if (!r.empty() && match(r)) return true;
        }
    }

    // Drops reports that arrived before our request.
    void Drain() {
        for (int i = 0; i < 64 && !Read(0).empty(); i++) {
        }
    }

private:
    HANDLE h_ = nullptr;
    HIDP_CAPS caps_{};
    HANDLE readEvent_ = nullptr, writeEvent_ = nullptr;
    OVERLAPPED read_{};
    std::vector<BYTE> buf_;
    bool pending_ = false;
};

// Data the way hidapi returns it: without the leading report id 0 of a device
// that has no report ids.
std::vector<BYTE> WithoutZeroId(std::vector<BYTE> r) {
    if (!r.empty() && r[0] == 0) r.erase(r.begin());
    return r;
}

// ---------------------------------------------------------------------------
// Logitech HID++ 2.0 headsets

const WORD kUnified = 0x1004, kStatus = 0x1000, kVoltage = 0x1001, kAdc = 0x1F20;

int VoltageToPercent(int mv) {
    // Li-ion discharge curve used by Solaar.
    static const int curve[][2] = {{4186, 100}, {4067, 90}, {3989, 80}, {3922, 70}, {3859, 60}, {3811, 50}, {3778, 40},
                                   {3751, 30},  {3717, 20}, {3671, 10}, {3646, 5},  {3579, 2},  {3500, 0}};
    if (mv >= curve[0][0]) return 100;
    for (int i = 0; i + 1 < (int)std::size(curve); i++) {
        int hiMv = curve[i][0], hiP = curve[i][1], loMv = curve[i + 1][0], loP = curve[i + 1][1];
        if (mv >= loMv) return (int)std::lround(loP + (double)(mv - loMv) * (hiP - loP) / (hiMv - loMv));
    }
    return 0;
}

bool ParseHidppBattery(WORD feature, const BYTE* p, BatteryLevel* out) {
    if (feature == kUnified || feature == kStatus) {
        bool charging = p[2] >= 1 && p[2] <= 4;
        if (p[0] > 0 && p[0] <= 100) {
            *out = {p[0], charging};
            return true;
        }
        if (feature == kUnified && p[0] == 0) {  // only the level flags
            int level = p[1] & 8 ? 90 : p[1] & 4 ? 50 : p[1] & 2 ? 20 : p[1] & 1 ? 5 : -1;
            if (level >= 0) {
                *out = {level, charging};
                return true;
            }
        }
        return false;
    }
    int mv = p[0] << 8 | p[1];
    if (feature == kAdc && !(p[2] & 0x01)) return false;  // headset not connected
    if (mv < 2500) return false;
    *out = {VoltageToPercent(mv), feature == kVoltage ? (p[2] & 0x80) != 0 : (p[2] & 0x02) != 0};
    return true;
}

struct Hidpp {
    HidDev& dev;
    int swid = 0;

    // The 16 parameter bytes of the reply; false on an error reply or no reply.
    bool Request(BYTE feature, BYTE function, std::vector<BYTE> params, BYTE* out) {
        swid = (swid + 1) % 6;
        BYTE fn = (BYTE)(function << 4 | (0x0A + swid));
        std::vector<BYTE> req = {0x11, 0xFF, feature, fn};
        req.insert(req.end(), params.begin(), params.end());
        if (!dev.Write(req)) return false;
        bool ok = false;
        dev.ReadUntil(600, [&](const std::vector<BYTE>& r) {
            if (r.size() < 6 || r[0] != 0x11 || r[1] != 0xFF) return false;
            if (r[2] == 0xFF && r[3] == feature && r[4] == fn) return true;  // error reply
            if (r[2] != feature || r[3] != fn) return false;
            std::fill(out, out + 16, 0);
            std::copy(r.begin() + 4, r.begin() + std::min<size_t>(r.size(), 20), out);
            ok = true;
            return true;
        });
        return ok;
    }
};

bool ReadHidpp(HidDev& dev, BatteryLevel* out) {
    Hidpp h{dev};
    static const std::pair<WORD, BYTE> features[] = {{kUnified, 1}, {kStatus, 0}, {kVoltage, 0}, {kAdc, 0}};
    for (auto [feature, function] : features) {
        BYTE p[16];
        if (!h.Request(0, 0, {(BYTE)(feature >> 8), (BYTE)feature}, p) || p[0] == 0) continue;
        BYTE index = p[0];
        if (h.Request(index, function, {}, p) && ParseHidppBattery(feature, p, out)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Logitech Centurion

struct Centurion {
    HidDev& dev;
    std::vector<BYTE> prefix;  // {51} or {50, 23}
    enum Result { Ok, Failed, Offline };
    static const BYTE kSwid = 0x01;

    std::vector<BYTE> Frame(const std::vector<BYTE>& payload) const {
        std::vector<BYTE> f = prefix;
        f.push_back((BYTE)(payload.size() + 1));
        f.push_back(0);  // flags
        f.insert(f.end(), payload.begin(), payload.end());
        return f;
    }

    bool Payload(const std::vector<BYTE>& r, std::vector<BYTE>* p) const {
        size_t n = prefix.size();
        if (r.size() < n + 3 || !std::equal(prefix.begin(), prefix.end(), r.begin())) return false;
        size_t len = r[n];
        if (len <= 1 || n + 1 + len > r.size()) return false;
        p->assign(r.begin() + n + 2, r.begin() + n + 1 + len);
        return true;
    }

    // A request to the receiver itself.
    Result Direct(BYTE index, BYTE function, std::vector<BYTE> params, std::vector<BYTE>* data) {
        BYTE fn = (BYTE)((function & 0xF0) | kSwid);
        std::vector<BYTE> payload = {index, fn};
        payload.insert(payload.end(), params.begin(), params.end());
        if (!dev.Write(Frame(payload))) return Failed;
        Result res = Failed;
        dev.ReadUntil(1500, [&](const std::vector<BYTE>& r) {
            std::vector<BYTE> p;
            if (!Payload(r, &p) || p.size() < 2) return false;
            if (p[0] == 0xFF && p.size() >= 3 && p[1] == index && p[2] == fn) return true;  // error
            if (p[0] != index || p[1] != fn) return false;
            data->assign(p.begin() + 2, p.end());
            res = Ok;
            return true;
        });
        return res;
    }

    // A request to the headset, through the receiver's bridge feature.
    Result Bridge(BYTE bridge, BYTE index, BYTE function, std::vector<BYTE> params, std::vector<BYTE>* data) {
        BYTE fn = (BYTE)((function & 0xF0) | kSwid);
        std::vector<BYTE> sub = {0x00, index, fn};
        sub.insert(sub.end(), params.begin(), params.end());
        std::vector<BYTE> payload = {bridge, 0x10 | kSwid, (BYTE)((sub.size() >> 8) & 0x0F), (BYTE)sub.size()};
        payload.insert(payload.end(), sub.begin(), sub.end());
        if (!dev.Write(Frame(payload))) return Failed;
        bool acked = false;
        Result res = Failed;
        bool done = dev.ReadUntil(1500, [&](const std::vector<BYTE>& r) {
            std::vector<BYTE> p;
            if (!Payload(r, &p) || p.size() < 2 || p[0] != bridge || (p[1] >> 4) != 1) return false;
            if ((p[1] & 0x0F) == kSwid) {  // the receiver took the message
                acked = true;
                return false;
            }
            if ((p[1] & 0x0F) != 0 || p.size() < 7 || p[4] != 0x00) return false;
            if (p[5] == 0xFF && p[6] == index) return true;  // the headset refused
            if (p[5] != index || p[6] != fn) return false;
            data->assign(p.begin() + 7, p.end());
            res = Ok;
            return true;
        });
        if (!done && acked) return Offline;
        return res;
    }

    // Index of feature `id`: of the receiver (bridge < 0) or of the headset.
    int FeatureIndex(int bridge, WORD id) {
        std::vector<BYTE> d;
        std::vector<BYTE> ids = {0x00, 0x01};  // FeatureSet (0x0001) through Root
        Result r = bridge < 0 ? Direct(0, 0x00, ids, &d) : Bridge((BYTE)bridge, 0, 0x00, ids, &d);
        if (r != Ok || d.empty() || d[0] == 0) return r == Offline ? -2 : -1;
        BYTE fs = d[0];
        r = bridge < 0 ? Direct(fs, 0x00, {}, &d) : Bridge((BYTE)bridge, fs, 0x00, {}, &d);
        if (r != Ok || d.empty()) return r == Offline ? -2 : -1;
        int count = d[0];
        for (int n = 0; n < count; n++) {
            r = bridge < 0 ? Direct(fs, 0x10, {(BYTE)n}, &d) : Bridge((BYTE)bridge, fs, 0x10, {(BYTE)n}, &d);
            if (r != Ok) return r == Offline ? -2 : -1;
            if (d.size() >= 3 && (d[1] << 8 | d[2]) == id) return n;
        }
        return -1;
    }
};

struct CenturionCache {
    int bridge = -1, battery = -1;
};

bool ReadCenturion(HidDev& dev, const std::vector<BYTE>& prefix, CenturionCache& cache, BatteryLevel* out) {
    Centurion c{dev, prefix};
    if (cache.bridge < 0) cache.bridge = c.FeatureIndex(-1, 0x0003);
    if (cache.bridge < 0) return false;
    if (cache.battery < 0) {
        int i = c.FeatureIndex(cache.bridge, 0x0104);
        if (i == -2) return false;  // headset off: try again next time
        if (i < 0) {
            cache = {};
            return false;
        }
        cache.battery = i;
    }
    std::vector<BYTE> d;
    auto r = c.Bridge((BYTE)cache.bridge, (BYTE)cache.battery, 0x00, {}, &d);
    if (r == Centurion::Failed) cache = {};  // discover again next time
    if (r != Centurion::Ok || d.empty() || d[0] > 100) return false;
    *out = {d[0], d.size() >= 3 && d[2] >= 1 && d[2] <= 3};
    return true;
}

// ---------------------------------------------------------------------------
// SteelSeries

enum class Steel { Nova7, Nova7Discrete, Nova5, Arctis7Plus, GameBuds };

bool ParseSteel(Steel kind, const std::vector<BYTE>& r, BatteryLevel* out, bool* answered) {
    if (r.empty() || r[0] != 0xB0) return false;
    *answered = true;
    switch (kind) {
        case Steel::Nova7:
        case Steel::Nova7Discrete:
            // The link byte must say "connected": the battery byte keeps its old value while the headset is off.
            if (r.size() < 4 || r[1] != 0x03 || r[3] == 0x00) return false;
            *out = {kind == Steel::Nova7 ? std::min<int>(r[2], 100) : std::min<int>(r[2], 4) * 25, r[3] == 1 || r[3] == 2};
            return true;
        case Steel::Nova5:
            if (r.size() < 5 || r[1] == 0x02) return false;
            *out = {std::min<int>(r[3], 100), r[4] == 0x01};
            return true;
        case Steel::Arctis7Plus:
            if (r.size() < 4 || r[1] == 0x01) return false;
            *out = {std::min<int>(r[2], 4) * 25, r[3] == 0x01};
            return true;
        case Steel::GameBuds: {
            if (r.size() < 7) return false;
            int level = -1;
            for (int i = 0; i < 2; i++)  // the lower level of the buds out of the case
                if (r[3 + i] == 0x03) level = level < 0 ? r[5 + i] : std::min<int>(level, r[5 + i]);
            if (level < 0) return false;
            *out = {std::min(level, 100), false};
            return true;
        }
    }
    return false;
}

bool ReadSteel(HidDev& dev, Steel kind, BatteryLevel* out) {
    dev.Drain();
    if (!dev.Write({0x00, 0xB0})) return false;
    bool ok = false;
    dev.ReadUntil(1000, [&](const std::vector<BYTE>& raw) {
        bool answered = false;
        ok = ParseSteel(kind, WithoutZeroId(raw), out, &answered);
        return answered;
    });
    return ok;
}

// ---------------------------------------------------------------------------
// HyperX

bool ReadCloud2(HidDev& dev, BatteryLevel* out) {
    auto query = [&](BYTE cmd, int index, int* value) {
        dev.Drain();
        if (!dev.Write({0x06, 0xFF, 0xBB, cmd, 0x00})) return false;
        return dev.ReadUntil(1000, [&](const std::vector<BYTE>& r) {
            if ((int)r.size() <= index || r[0] != 0x06 || r[1] != 0xFF || r[2] != 0xBB || r[3] != cmd) return false;
            *value = r[index];
            return true;
        });
    };
    int level = 0, charging = 0;
    if (!query(0x02, 7, &level) || level > 100) return false;
    query(0x03, 4, &charging);
    *out = {level, charging == 1};
    return true;
}

bool ReadCloud3(HidDev& dev, BatteryLevel* out) {
    auto query = [&](BYTE cmd, BYTE response, std::vector<BYTE>* reply) {
        dev.Drain();
        std::vector<BYTE> req = {0x66, cmd};
        // Some HyperX dongles take these packets as feature reports only.
        if (!dev.Write(req) && !dev.SetFeature(req)) return false;
        return dev.ReadUntil(1200, [&](const std::vector<BYTE>& r) {
            if (r.size() < 5 || r[0] != 0x66 || (r[1] != cmd && r[1] != response)) return false;
            *reply = r;
            return true;
        });
    };
    std::vector<BYTE> r;
    if (!query(0x89, 0x0D, &r) || (r[2] == 0 && r[3] == 0) || r[4] > 100) return false;
    int level = r[4];
    bool charging = query(0x8A, 0x0C, &r) && (r[2] == 1 || r[2] == 2);
    *out = {level, charging};
    return true;
}

bool ReadAlpha2(HidDev& dev, BatteryLevel* out) {
    dev.Drain();  // with NGENUITY open the station sends a stream of other frames
    if (!dev.Write({0x50, 0x02, 0x00, 0x00})) return false;
    bool ok = false;
    dev.ReadUntil(1500, [&](const std::vector<BYTE>& r) {
        if (r.size() < 7 || r[0] != 0x51 || r[1] != 0x02) return false;
        if (r[2] <= 100) {
            *out = {r[2], (r[6] & 0x80) != 0};
            ok = true;
        }
        return true;
    });
    return ok;
}

// ---------------------------------------------------------------------------
// Known devices

enum class Proto { Hidpp, Centurion51, Centurion50, SteelSeries, Cloud2, Cloud3, Alpha2 };

struct Known {
    WORD vid, pid;
    Proto proto;
    WORD page, usage;  // the collection to talk to; usage 0 = any on that page
    Steel steel = Steel::Nova7;
};

const Known kKnown[] = {
    // Logitech HID++ headsets (HeadsetControl's list)
    {0x046D, 0x0A66, Proto::Hidpp, 0xFF43, 0x0202},  // G533
    {0x046D, 0x0AC4, Proto::Hidpp, 0x000C, 0x0001},  // G535
    {0x046D, 0x0A5C, Proto::Hidpp, 0xFF43, 0x0202},  // G633
    {0x046D, 0x0A89, Proto::Hidpp, 0xFF43, 0x0202},  // G635
    {0x046D, 0x0A5B, Proto::Hidpp, 0xFF43, 0x0202},  // G933
    {0x046D, 0x0A87, Proto::Hidpp, 0xFF43, 0x0202},  // G935
    {0x046D, 0x0AB5, Proto::Hidpp, 0xFF43, 0x0202},  // G733
    {0x046D, 0x0AFE, Proto::Hidpp, 0xFF43, 0x0202},  // G733
    {0x046D, 0x0B1F, Proto::Hidpp, 0xFF43, 0x0202},  // G733
    {0x046D, 0x0AA7, Proto::Hidpp, 0xFF43, 0x0202},  // G PRO
    {0x046D, 0x0AAA, Proto::Hidpp, 0xFF43, 0x0202},  // G PRO X
    {0x046D, 0x0ABA, Proto::Hidpp, 0xFF43, 0x0202},  // G PRO X
    {0x046D, 0x0AFB, Proto::Hidpp, 0xFF43, 0x0202},  // G PRO X 2
    {0x046D, 0x0AFC, Proto::Hidpp, 0xFF43, 0x0202},  // G PRO X 2
    // Logitech Centurion
    {0x046D, 0x0AF7, Proto::Centurion51, 0xFFA0, 0x0001},  // G PRO X 2 LIGHTSPEED
    {0x046D, 0x0B18, Proto::Centurion50, 0xFFA0, 0x0001},  // G522 LIGHTSPEED
    // SteelSeries
    {0x1038, 0x22A1, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7},          // Arctis Nova 7
    {0x1038, 0x2202, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7Discrete},  // Arctis Nova 7
    {0x1038, 0x227E, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7},          // Arctis Nova 7 Gen 2
    {0x1038, 0x2206, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7Discrete},  // Arctis Nova 7x
    {0x1038, 0x2258, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7},          // Arctis Nova 7x
    {0x1038, 0x229E, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7},          // Arctis Nova 7x
    {0x1038, 0x22AD, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7},          // Arctis Nova 7x
    {0x1038, 0x22A4, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7Discrete},  // Arctis Nova 7X
    {0x1038, 0x22A5, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7},          // Arctis Nova 7X
    {0x1038, 0x223A, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7Discrete},  // Arctis Nova 7 Diablo IV
    {0x1038, 0x22A9, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7},          // Arctis Nova 7 Diablo IV
    {0x1038, 0x227A, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7Discrete},  // Arctis Nova 7 WoW Edition
    {0x1038, 0x220A, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7Discrete},  // Arctis Nova 7P
    {0x1038, 0x22A7, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7},          // Arctis Nova 7P
    {0x1038, 0x2298, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova7},          // Arctis Nova 7P
    {0x1038, 0x2232, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova5},          // Arctis Nova 5
    {0x1038, 0x2253, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova5},          // Arctis Nova 5X
    {0x1038, 0x2269, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova5},          // Arctis Nova 3P Wireless
    {0x1038, 0x226D, Proto::SteelSeries, 0xFFC0, 0, Steel::Nova5},          // Arctis Nova 3X Wireless
    {0x1038, 0x220E, Proto::SteelSeries, 0xFFC0, 0, Steel::Arctis7Plus},    // Arctis 7+
    {0x1038, 0x2212, Proto::SteelSeries, 0xFFC0, 0, Steel::Arctis7Plus},    // Arctis 7+ PS5
    {0x1038, 0x2216, Proto::SteelSeries, 0xFFC0, 0, Steel::Arctis7Plus},    // Arctis 7+ Xbox
    {0x1038, 0x2236, Proto::SteelSeries, 0xFFC0, 0, Steel::Arctis7Plus},    // Arctis 7+ Destiny
    {0x1038, 0x230A, Proto::SteelSeries, 0xFFC0, 0, Steel::GameBuds},       // Arctis GameBuds
    {0x1038, 0x2317, Proto::SteelSeries, 0xFFC0, 0, Steel::GameBuds},       // Arctis GameBuds X
    // HyperX
    {0x03F0, 0x0696, Proto::Cloud2, 0xFF90, 0x0303},  // Cloud II Wireless
    {0x03F0, 0x018B, Proto::Cloud2, 0xFF90, 0x0303},  // Cloud II Wireless
    {0x03F0, 0x05B7, Proto::Cloud3, 0xFF13, 0x0001},  // Cloud III Wireless
    {0x03F0, 0x0C9D, Proto::Cloud3, 0xFF13, 0x0001},  // Cloud III Wireless
    {0x03F0, 0x0995, Proto::Cloud3, 0xFF13, 0x0001},  // Cloud II Core Wireless
    {0x03F0, 0x0795, Proto::Cloud3, 0xFF13, 0x0001},  // Cloud II Core Wireless
    {0x03F0, 0x08BE, Proto::Alpha2, 0xFF13, 0xFF00},  // Cloud Alpha 2
};

const Known* FindKnown(WORD vid, WORD pid) {
    for (const Known& k : kKnown)
        if (k.vid == vid && k.pid == pid) return &k;
    return nullptr;
}

// "...vid_046d&pid_0af7..." in a USB HID path
bool PathIds(const std::wstring& lowerPath, WORD* vid, WORD* pid) {
    size_t v = lowerPath.find(L"vid_"), p = lowerPath.find(L"pid_");
    if (v == std::wstring::npos || p == std::wstring::npos) return false;
    *vid = (WORD)wcstoul(lowerPath.substr(v + 4, 4).c_str(), nullptr, 16);
    *pid = (WORD)wcstoul(lowerPath.substr(p + 4, 4).c_str(), nullptr, 16);
    return true;
}

// ---------------------------------------------------------------------------
// Polling

using Levels = std::map<std::wstring, BatteryLevel>;  // container id -> level

std::map<std::wstring, CenturionCache> g_centurion;  // by HID path (worker thread only)

void PollHid(Levels& levels) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &iface); i++) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
        std::vector<BYTE> buf(size);
        auto detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data();
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA info{sizeof(info)};
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, &info)) continue;
        std::wstring path = detail->DevicePath;
        WORD vid, pid;
        if (!PathIds(Lower(path), &vid, &pid)) continue;
        const Known* k = FindKnown(vid, pid);
        if (!k) continue;
        std::wstring container = DevNodeContainer(info.DevInst);
        if (container.empty() || levels.count(container)) continue;

        // Only the collection the protocol talks to.
        HANDLE probe = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (probe == INVALID_HANDLE_VALUE) continue;
        HIDP_CAPS caps{};
        PHIDP_PREPARSED_DATA pp;
        if (HidD_GetPreparsedData(probe, &pp)) {
            HidP_GetCaps(pp, &caps);
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(probe);
        if (caps.UsagePage != k->page || (k->usage && caps.Usage != k->usage)) continue;

        HidDev dev;
        if (!dev.Open(path)) continue;
        BatteryLevel level;
        bool ok = false;
        switch (k->proto) {
            case Proto::Hidpp: ok = ReadHidpp(dev, &level); break;
            case Proto::Centurion51: ok = ReadCenturion(dev, {0x51}, g_centurion[path], &level); break;
            case Proto::Centurion50: ok = ReadCenturion(dev, {0x50, 0x23}, g_centurion[path], &level); break;
            case Proto::SteelSeries: ok = ReadSteel(dev, k->steel, &level); break;
            case Proto::Cloud2: ok = ReadCloud2(dev, &level); break;
            case Proto::Cloud3: ok = ReadCloud3(dev, &level); break;
            case Proto::Alpha2: ok = ReadAlpha2(dev, &level); break;
        }
        if (ok) levels[container] = level;
    }
    SetupDiDestroyDeviceInfoList(set);
}

void PollBluetooth(Levels& levels) {
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, nullptr, CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS || !len) return;
    std::vector<wchar_t> ids(len);
    if (CM_Get_Device_ID_ListW(nullptr, ids.data(), len, CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS) return;
    for (const wchar_t* id = ids.data(); *id; id += wcslen(id) + 1) {
        if (_wcsnicmp(id, L"BTH", 3) != 0) continue;  // BTHENUM, BTHLE, BTHLEDEVICE
        DEVINST dn;
        if (CM_Locate_DevNodeW(&dn, (DEVINSTID_W)id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
        BYTE percent = 0;
        DEVPROPTYPE type;
        ULONG size = sizeof(percent);
        if (CM_Get_DevNode_PropertyW(dn, &kBluetoothBattery, &type, &percent, &size, 0) != CR_SUCCESS ||
            type != DEVPROP_TYPE_BYTE || percent > 100)
            continue;
        std::wstring container = DevNodeContainer(dn);
        if (!container.empty() && !levels.count(container)) levels[container] = {percent, false};
    }
}

std::mutex g_lock;
Levels g_levels;                                   // guarded by g_lock
std::map<std::wstring, std::wstring> g_endpoints;  // endpoint id -> container id (UI thread)
std::thread g_thread;
HANDLE g_wake = nullptr, g_stop = nullptr;
HWND g_notify = nullptr;

bool Same(const Levels& a, const Levels& b) {
    if (a.size() != b.size()) return false;
    for (auto& [k, v] : a) {
        auto it = b.find(k);
        if (it == b.end() || it->second.percent != v.percent || it->second.charging != v.charging) return false;
    }
    return true;
}

void Worker() {
    HANDLE handles[] = {g_stop, g_wake};
    for (;;) {
        Levels levels;
        PollHid(levels);
        PollBluetooth(levels);
        bool changed;
        {
            std::lock_guard<std::mutex> l(g_lock);
            changed = !Same(levels, g_levels);
            if (changed) g_levels = std::move(levels);
        }
        if (changed) PostMessageW(g_notify, WM_APP_BATTERY_CHANGED, 0, 0);
        DWORD w = WaitForMultipleObjects(2, handles, FALSE, kPollMs);
        if (w == WAIT_OBJECT_0) return;
        if (w == WAIT_OBJECT_0 + 1) {
            // Asked for early: a short pause, so a burst of device changes is one poll.
            if (WaitForSingleObject(g_stop, kMinGapMs) == WAIT_OBJECT_0) return;
            ResetEvent(g_wake);
        }
    }
}

}  // namespace

namespace Battery {

void Start(HWND notify) {
    if (g_thread.joinable()) return;
    g_notify = notify;
    g_wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = std::thread(Worker);
}

void Stop() {
    if (!g_thread.joinable()) return;
    SetEvent(g_stop);
    g_thread.join();
    CloseHandle(g_wake);
    CloseHandle(g_stop);
}

void Refresh() {
    if (g_wake) SetEvent(g_wake);
}

bool ForEndpoint(const std::wstring& endpointId, BatteryLevel* out) {
    auto it = g_endpoints.find(endpointId);
    if (it == g_endpoints.end()) {
        std::wstring container;
        DEVINST dn;
        std::wstring instance = L"SWD\\MMDEVAPI\\" + endpointId;
        if (CM_Locate_DevNodeW(&dn, (DEVINSTID_W)instance.c_str(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
            container = DevNodeContainer(dn);
        it = g_endpoints.emplace(endpointId, container).first;
    }
    if (it->second.empty()) return false;
    std::lock_guard<std::mutex> l(g_lock);
    auto level = g_levels.find(it->second);
    if (level == g_levels.end()) return false;
    *out = level->second;
    return true;
}

}  // namespace Battery
