// Kiểm thử EtherCatMasterService trên bus mô phỏng + EsiCatalog trên thư mục ESI thật.
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>

#include "Base/Async.h"
#include "Base/Text.h"
#include "Data/SystemData.h"
#include "EtherCat/EtherCat.h"
#include "TestKit.h"

using namespace ecm;

namespace
{
    bool WaitUntil(const std::function<bool()>& pred, int timeoutMs)
    {
        base::Stopwatch sw;
        while (!pred())
        {
            if (sw.ElapsedMilliseconds() > timeoutMs) return false;
            base::Delay(5);
        }
        return true;
    }

    /// Bus mô phỏng báo WrongDriver cho một vị trí (bus mô phỏng gốc không tự sinh được nhánh này)
    /// và cho phép bơm Fault + mã lỗi để thử OnAlarm / FaultReset.
    class TestBus : public SimulatedEtherCatBus
    {
    public:
        std::atomic<int> WrongAt{ -1 };
        std::atomic<int> FaultAt{ -1 };

        std::vector<AxisConnectResult> ConnectAllAxis(const std::string& adapter, const std::vector<AxisConfig>& axes,
                                                      const std::vector<IoModuleConfig>& io, std::vector<AxisConnectResult>& ioResults) override
        {
            auto results = SimulatedEtherCatBus::ConnectAllAxis(adapter, axes, io, ioResults);
            bool wrong = false;
            for (auto& r : results)
                if (r.SlavePosition == WrongAt) { r.Status = AxisConnectStatus::WrongDriver; r.Message = "wrong driver"; r.ActualProductCode = 0x999; wrong = true; }
            if (wrong) Close();
            return results;
        }
        void SetControlword(int pos, uint16_t cw) override
        {
            if (pos == FaultAt && (cw & CiA402Registers::Controlword::FaultReset) != 0) FaultAt = -1;
            SimulatedEtherCatBus::SetControlword(pos, cw);
        }
        uint16_t GetStatusword(int pos) override
        {
            return pos == FaultAt ? CiA402Registers::Statusword::Fault : SimulatedEtherCatBus::GetStatusword(pos);
        }
        uint16_t GetErrorCode(int pos) override { return pos == FaultAt ? uint16_t(0x7500) : uint16_t(0); }
    };

    EtherCatConfig MakeConfig()
    {
        EtherCatConfig cfg;
        cfg.NetworkAdapter = "sim";
        for (int i = 0; i < 4; i++)
        {
            AxisConfig a;
            a.Name = "AX" + std::to_string(i);
            a.SlavePosition = i;
            a.ExpectedVendorId = LeadshineDrives::VENDOR_ID;
            a.ExpectedProductCode = i == 3 ? 0 : LeadshineDrives::CL3B_EC507_PRODUCT_CODE;   // slot 3 chưa cấu hình
            cfg.Axes.push_back(a);
        }
        cfg.Axes[0].SoftwareLimitPositiveUnits = 500.0;
        IoModuleConfig io;
        io.Name = "IO_1";
        io.SlavePosition = 9;
        io.ExpectedVendorId = LeadshineDrives::VENDOR_ID;
        io.ExpectedProductCode = LeadshineDrives::R1EC_PRODUCT_CODE;
        cfg.IoModules.push_back(io);
        return cfg;
    }

    void SimTests()
    {
        auto bus = std::make_shared<TestBus>();
        EtherCatMasterService svc(bus);
        std::mutex logMutex;
        std::vector<std::string> logs;
        std::atomic<int> connEvents{ 0 }, alarmCode{ 0 }, alarmCount{ 0 };
        svc.OnLog += [&](std::string m) { std::lock_guard<std::mutex> l(logMutex); logs.push_back(std::move(m)); };
        svc.OnConnectionChanged += [&](bool) { ++connEvents; };
        svc.OnAlarm += [&](int, uint16_t code) { alarmCode = code; ++alarmCount; };
        auto hasLog = [&](const std::string& part)
        {
            std::lock_guard<std::mutex> l(logMutex);
            for (auto& m : logs) if (base::Contains(m, part)) return true;
            return false;
        };

        // Config rỗng: không đụng bus, không kết nối.
        std::vector<AxisConnectResult> ioResults;
        CHECK(svc.ConnectAllAxis(EtherCatConfig(), ioResults).empty());
        CHECK(!svc.IsConnected() && connEvents == 1);
        CHECK(hasLog("nothing to connect"));
        {
            std::lock_guard<std::mutex> l(logMutex);
            const std::string& m = logs.front();   // "[HH:mm:ss.fff] message"
            CHECK(m.size() > 15 && m[0] == '[' && m[3] == ':' && m[6] == ':' && m[9] == '.' && m[13] == ']' && m[14] == ' ');
        }

        EtherCatConfig cfg = MakeConfig();

        // Sai driver ở vị trí 1 → không kết nối, vẫn trả đủ kết quả từng slot.
        bus->WrongAt = 1;
        auto results = svc.ConnectAllAxis(cfg, ioResults);
        CHECK(results.size() == 3 && ioResults.size() == 1);
        CHECK(results.size() == 3 && results[1].Status == AxisConnectStatus::WrongDriver && !results[1].IsOk() && results[0].IsOk());
        CHECK(!svc.IsConnected() && bus->SlaveCount() == 0);
        CHECK(hasLog("1 of 4 configured slot(s) failed validation"));
        bus->WrongAt = -1;

        // Slave vắng (NoResponse) ở module I/O.
        bus->SetSimulateMissingAt(9, true);
        results = svc.ConnectAllAxis(cfg, ioResults);
        CHECK(ioResults.size() == 1 && ioResults[0].Status == AxisConnectStatus::NoResponse && !svc.IsConnected());
        bus->SetSimulateMissingAt(9, false);

        // Kết nối đầy đủ.
        results = svc.ConnectAllAxis(cfg, ioResults);
        CHECK(svc.IsConnected());
        CHECK(results.size() == 3 && ioResults.size() == 1 && ioResults[0].InputBitCount == 8);
        CHECK(svc.Axes().size() == 3 && svc.IoModules().size() == 1 && svc.Bus()->SlaveCount() == 4);
        CHECK(svc.GetAxisState(3) == nullptr && svc.GetAxisState(0) != nullptr);
        CHECK(hasLog("Software limit (positive) -> 500 units (500000 counts)"));
        CHECK(hasLog("3 axis(es) and 1 I/O module(s) validated OK, connected on 'sim'."));
        auto io = svc.GetIoModuleState(9);
        CHECK(io && io->InputCount == 8 && io->OutputCount == 8 && io->Slots.size() == 1 && io->Slots[0]->InputCount == 8);
        auto a0 = svc.GetAxisState(0), a1 = svc.GetAxisState(1), a2 = svc.GetAxisState(2);
        if (!a0 || !a1 || !a2 || !io) { CHECK_MSG(false, "connect failed, skipping motion checks"); return; }
        CHECK(WaitUntil([&] { return a0->SwitchOnDisabled.load(); }, 1000));

        // Chưa Servo ON thì không cho chạy.
        svc.MoveAbsolute(0, 5, 50, 0.1, 0.1);
        CHECK(hasLog("[Axis 0] Cannot move: OperationEnabled=False, Fault=False"));

        // Servo ON → Operation Enabled.
        for (int i = 0; i < 3; i++) svc.ServoOn(i);
        CHECK(a0->OperationEnabled && a1->OperationEnabled && a2->OperationEnabled && a0->CanMove());
        CHECK(hasLog("[Axis 0] Servo ON (Operation Enabled)"));
        CHECK(CiA402Registers::DecodeState(a0->Statusword) == "Operation enabled");

        // MoveAbsolute + WaitForMotionComplete.
        svc.MoveAbsolute(0, 10.0, 50.0, 0.1, 0.1);
        CHECK(svc.WaitForMotionComplete(0, 5000));
        CHECK_NEAR(a0->ActualPositionUnits.load(), 10.0, 0.001);
        CHECK(a0->ActualPositionRaw == 10000 && a0->ModeOfOperationDisplay == 1);
        CHECK(hasLog("[Axis 0] MoveAbsolute target=10.000 vel=50.000 tAcc=0.10s tDec=0.10s"));

        // MoveRelative (-4 từ 10 → 6). TargetReached của lượt trước còn dư tới vòng poll kế — như C#.
        svc.MoveRelative(0, -4.0, 50.0, 0.1, 0.1);
        CHECK(WaitUntil([&] { return !a0->TargetReached || std::fabs(a0->ActualPositionUnits - 6.0) < 0.001; }, 1000));
        CHECK(svc.WaitForMotionComplete(0, 5000));
        CHECK_NEAR(a0->ActualPositionUnits.load(), 6.0, 0.001);

        // MoveLinear hai trục.
        CHECK(svc.MoveLinear(1, 3.0, 2, 4.0, 50.0, 0.1, 0.1) == false);
        CHECK(WaitUntil([&] { return std::fabs(a1->ActualPositionUnits - 3.0) < 0.001 && std::fabs(a2->ActualPositionUnits - 4.0) < 0.001; }, 5000));
        CHECK(svc.MoveLinear(1, 3.0, 2, 4.0, 50.0, 0.1, 0.1) == true);   // quãng đường ~0 → bỏ qua

        // Jog + Stop.
        double before = a1->ActualPositionUnits;
        svc.Jog(1, +1, 20.0);
        CHECK(WaitUntil([&] { return a1->ActualPositionUnits > before + 1.0; }, 3000));
        svc.Stop(1);
        base::Delay(60);
        double stopped = a1->ActualPositionUnits;
        base::Delay(120);
        CHECK(a1->ActualPositionUnits.load() == stopped && stopped < 100.0);
        svc.Jog(1, 0, 20.0);
        CHECK(hasLog("Jog: direction must be -1 or +1, got 0"));

        // StartHome → homing attained (bit12), không dính TargetReached.
        CHECK(!a2->SetpointAckOrHomingAttained || a2->TargetReached);
        svc.StartHome(2);
        CHECK(WaitUntil([&] { return a2->ModeOfOperationDisplay == 6 && a2->SetpointAckOrHomingAttained && !a2->TargetReached; }, 2000));
        CHECK(hasLog("[Axis 2] Homing started (method 1, vel=10.000)"));

        // Fault bơm vào → OnAlarm một lần, WaitForMotionComplete bỏ; FaultReset xoá, Servo ON lại được.
        bus->FaultAt = 2;
        CHECK(WaitUntil([&] { return a2->Fault.load() && a2->ErrorCode == 0x7500; }, 2000));
        base::Delay(50);
        CHECK(alarmCode == 0x7500 && alarmCount == 1 && !a2->CanMove());
        CHECK(svc.WaitForMotionComplete(2, 1000) == false);
        svc.FaultReset(2);
        CHECK(WaitUntil([&] { return !a2->Fault && a2->SwitchOnDisabled && a2->ErrorCode == 0; }, 2000));
        svc.ServoOn(2);
        CHECK(a2->OperationEnabled.load());

        // Digital I/O.
        bus->SetSimulateInputs(9, 0xA5);
        CHECK(WaitUntil([&] { return io->Inputs == 0xA5 && io->Slots[0]->InputRegister == 0xA5; }, 1000));
        svc.SetDigitalOutput(9, 3, true);
        svc.SetDigitalOutput(9, 0, true);
        svc.SetDigitalOutput(9, 3, false);
        CHECK(io->Outputs == 0x1 && bus->GetSimulatedOutputs(9) == 0x1);
        svc.SetSlotOutput(9, 0, 5, true);
        CHECK(io->Slots[0]->OutputRegister == 0x20 && bus->GetSimulatedOutputs(9) == 0x20);
        CHECK(svc.GetDetectedModuleIdents(9).empty() && svc.GetCyclicStats().SampleCount == 0);

        // Disconnect: dừng + join luồng polling rồi đóng bus.
        svc.Disconnect();
        CHECK(!svc.IsConnected() && !a0->IsConnected && bus->SlaveCount() == 0);
        CHECK(hasLog("Cyclic exchange stopped") && hasLog("Disconnected"));
        CHECK(!hasLog("Cyclic exchange error"));

        // Connect() kiểu cũ (fail-fast) trên bus mô phỏng mặc định.
        EtherCatMasterService plain;
        CHECK(plain.Connect(cfg) && plain.Axes().size() == 4 && plain.Bus()->SlaveCount() == 4);
        plain.ServoOn(3);
        CHECK(plain.GetAxisState(3)->OperationEnabled.load());
    }

    /// Driver hai trục (Welcon WE2x): một slave, hai địa chỉ trục. Kênh 0 giữ đúng số vị trí slave, kênh 1 dời các
    /// chỉ số profile 0x6000–0x67FF thêm 0x800; hai trục cùng slave có trạng thái và chuyển động riêng.
    void TwoAxisSlaveTests()
    {
        CHECK(AxisAddress::Make(4, 0) == 4 && AxisAddress::Slave(AxisAddress::Make(4, 1)) == 4 && AxisAddress::Channel(AxisAddress::Make(4, 1)) == 1);
        CHECK(AxisAddress::ProfileIndex(AxisAddress::Make(4, 1), 0x6040) == 0x6840 && AxisAddress::ProfileIndex(AxisAddress::Make(4, 1), 0x607A) == 0x687A);
        CHECK(AxisAddress::ProfileIndex(AxisAddress::Make(4, 1), 0x1C12) == 0x1C12 && AxisAddress::ProfileIndex(AxisAddress::Make(4, 1), 0x22E1) == 0x22E1);
        CHECK(AxisAddress::ProfileIndex(4, 0x6098) == 0x6098 && AxisAddress::Text(4) == "4" && AxisAddress::Text(AxisAddress::Make(4, 1)) == "4.1");

        auto bus = std::make_shared<SimulatedEtherCatBus>();
        EtherCatMasterService svc(bus);
        std::mutex logMutex;
        std::vector<std::string> logs;
        svc.OnLog += [&](std::string m) { std::lock_guard<std::mutex> l(logMutex); logs.push_back(std::move(m)); };
        auto hasLog = [&](const std::string& part)
        {
            std::lock_guard<std::mutex> l(logMutex);
            for (auto& m : logs) if (base::Contains(m, part)) return true;
            return false;
        };

        EtherCatConfig cfg;
        cfg.NetworkAdapter = "sim";
        auto add = [&](const char* name, int slave, int channel, uint32_t vendor, uint32_t product)
        {
            AxisConfig a;
            a.Name = name;
            a.SlavePosition = slave;
            a.AxisChannel = channel;
            a.ExpectedVendorId = vendor;
            a.ExpectedProductCode = product;
            cfg.Axes.push_back(a);
        };
        add("X", 0, 0, LeadshineDrives::VENDOR_ID, LeadshineDrives::L5N_PRODUCT_CODE);
        add("Z1", 1, 0, 0x0BA9, 0x20000001);
        add("A1", 1, 1, 0x0BA9, 0x20000001);

        std::vector<AxisConnectResult> ioResults;
        auto results = svc.ConnectAllAxis(cfg, ioResults);
        CHECK(results.size() == 3 && svc.IsConnected() && bus->SlaveCount() == 2);
        CHECK(results.size() == 3 && results[1].SlavePosition == 1 && results[1].AxisChannel == 0
              && results[2].SlavePosition == 1 && results[2].AxisChannel == 1 && results[2].Address() == AxisAddress::Make(1, 1));
        CHECK(results.size() == 3 && base::Contains(results[2].ToString(), "[1.1] A1: Ok"));

        const int z1 = AxisAddress::Make(1, 0), a1 = AxisAddress::Make(1, 1);
        auto sz = svc.GetAxisState(z1), sa = svc.GetAxisState(a1);
        CHECK(sz && sa && sz != sa && sz->Name == "Z1" && sa->Name == "A1" && sa->SlavePosition == 1 && sa->Address == a1);
        if (!sz || !sa) return;

        svc.ServoOn(z1);
        svc.ServoOn(a1);
        CHECK(sz->OperationEnabled.load() && sa->OperationEnabled.load());
        svc.MoveAbsolute(z1, 2.0, 1000, 0.01, 0.01);
        svc.MoveAbsolute(a1, -3.0, 1000, 0.01, 0.01);
        CHECK(WaitUntil([&] { return std::fabs(sz->ActualPositionUnits.load() - 2.0) < 1e-3 && std::fabs(sa->ActualPositionUnits.load() + 3.0) < 1e-3; }, 3000));
        CHECK(std::fabs(svc.GetAxisState(0)->ActualPositionUnits.load()) < 1e-9);
        svc.ServoOff(a1);
        CHECK(WaitUntil([&] { return !sa->OperationEnabled.load(); }, 1000) && sz->OperationEnabled.load());
        CHECK(hasLog("[Axis 1.1] Servo OFF") && hasLog("[Axis 1] Servo ON"));
        svc.Disconnect();
    }

    void EsiTests()
    {
        // ESI Welcon nằm sẵn trong cây nguồn (DriverInfo\Welcon): đọc được định danh WE2x mà cấu hình mặc định dùng.
        {
            auto welconDir = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "DriverInfo" / "Welcon";
            auto welcon = EsiCatalog::LoadFromFolder(base::PathToU8(welconDir));
            bool we2x = false;
            for (auto& d : welcon)
                if (d.VendorId == 0x0BA9 && d.ProductCode == 0x20000001) we2x = true;
            CHECK_MSG(we2x, base::PathToU8(welconDir));
        }
        // ESI Panasonic MINAS A6BE (DriverInfo\Panasonic, mã hoá iso-8859-1): có MADLN15BE — mã mặc định của X/Y/Z.
        {
            auto panaDir = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "DriverInfo" / "Panasonic";
            auto pana = EsiCatalog::LoadFromFolder(base::PathToU8(panaDir));
            auto def = scs::SystemData::CreateDefault();
            auto x = def->GetAxis("X");
            const EsiDeviceInfo* a6 = nullptr;
            for (auto& d : pana)
                if (d.VendorId == x->ExpectedVendorId && d.ProductCode == x->ExpectedProductCode) a6 = &d;
            CHECK_MSG(pana.size() >= 40 && a6 && a6->RevisionNo == x->ExpectedRevision
                      && a6->DisplayName.find("MADLN15BE") != std::string::npos,
                      std::format("{} devices, {}", pana.size(), a6 ? a6->ToString() : std::string("MADLN15BE not found")));
            auto sms = EsiCatalog::LoadSyncManagersFromFolder(base::PathToU8(panaDir));
            int a6Sm = 0;
            for (auto& s : sms) if (s.VendorId == 0x066F && s.ProductCode == 0x60380005) ++a6Sm;
            CHECK(a6Sm == 4);
        }

        const std::string dir = "C:\\Users\\Tuan\\OneDrive\\Tuan\\Tuan\\Projects\\EtherCATMaster\\ESI";
        if (!std::filesystem::is_directory(base::U8Path(dir)))
        {
            std::printf("  NOTE: ESI folder not found, skipping EsiCatalog checks\n");
            return;
        }
        auto devices = EsiCatalog::LoadFromFolder(dir);
        auto modules = EsiCatalog::LoadModulesFromFolder(dir);
        auto sms = EsiCatalog::LoadSyncManagersFromFolder(dir);
        auto vendors = EsiCatalog::LoadVendorsFromFolder(dir);
        std::printf("  ESI: %zu devices, %zu modules, %zu sync managers, %zu vendors\n", devices.size(), modules.size(), sms.size(), vendors.size());

        const EsiDeviceInfo* cl3b = nullptr; const EsiDeviceInfo* r1ec = nullptr; const EsiDeviceInfo* l5n = nullptr;
        for (auto& d : devices)
        {
            if (d.VendorId != LeadshineDrives::VENDOR_ID) continue;
            if (d.ProductCode == LeadshineDrives::CL3B_EC507_PRODUCT_CODE && !cl3b) cl3b = &d;
            if (d.ProductCode == LeadshineDrives::R1EC_PRODUCT_CODE && !r1ec) r1ec = &d;
            if (d.ProductCode == LeadshineDrives::L5N_PRODUCT_CODE && !l5n) l5n = &d;
        }
        CHECK(cl3b && cl3b->RevisionNo == LeadshineDrives::CL3B_EC507_REVISION && !cl3b->DisplayName.empty() && cl3b->SourceFile == "CL3B-EC_V1.08 XML.xml");
        CHECK(l5n && l5n->RevisionNo == 1 && l5n->SourceFile == "L5N_V1.01.xml" && !l5n->VendorName.empty());
        CHECK(r1ec && r1ec->RevisionNo == LeadshineDrives::R1EC_REVISION && r1ec->SourceFile == "R1EC-v1.4.0.0.xml");
        if (cl3b) std::printf("  CL3B: %s | group '%s'\n", cl3b->ToString().c_str(), cl3b->GroupType.c_str());
        bool fastech = false;
        for (auto& v : vendors) if (v.VendorId != LeadshineDrives::VENDOR_ID && !v.Drives.empty()) fastech = true;
        CHECK(fastech && vendors.size() >= 2);

        // Module R1EC: 20 module, bề rộng khác 0, SC-1600 = 16 DI với PDO mặc định 0x1A00 (Sm=3).
        int r1ecModules = 0, zeroWidth = 0;
        const EsiModuleInfo* sc1600 = nullptr;
        for (auto& m : modules)
        {
            if (m.CouplerProductCode != LeadshineDrives::R1EC_PRODUCT_CODE) continue;
            ++r1ecModules;
            if (m.InputBits + m.OutputBits == 0) ++zeroWidth;
            if (m.ModuleIdent == 0x71100025) sc1600 = &m;
        }
        CHECK(r1ecModules == 20 && zeroWidth == 0);
        CHECK(sc1600 && sc1600->TypeName == "SC-1600" && sc1600->DisplayName == "SC-1600(16 channels DI module)" && sc1600->ModuleClass == "Digital In");
        CHECK(sc1600 && sc1600->InputBits == 16 && sc1600->OutputBits == 0 && !sc1600->OutputPdo && !sc1600->OutputPdoAlt);
        CHECK(sc1600 && sc1600->InputPdo && sc1600->InputPdo->Index == 0x1A00 && sc1600->InputPdo->SyncManager == 3
              && sc1600->InputPdo->IsFixed && sc1600->InputPdo->Entries.size() == 16 && sc1600->InputPdo->Entries[1].Index == 0x6000
              && sc1600->InputPdo->Entries[1].SubIndex == 2 && sc1600->InputPdo->Entries[1].BitLength == 1);
        CHECK(sc1600 && sc1600->InputPdoAlt && sc1600->InputPdoAlt->Index == 0x1A01 && sc1600->InputPdoAlt->SyncManager == -1
              && sc1600->InputPdoAlt->IsFixed && sc1600->InputPdoAlt->TotalBits() == 16);

        // SyncManager của R1EC: 4 cái theo thứ tự khai báo.
        std::vector<EsiSyncManagerInfo> r1ecSm;
        for (auto& s : sms)
            if (s.VendorId == LeadshineDrives::VENDOR_ID && s.ProductCode == LeadshineDrives::R1EC_PRODUCT_CODE) r1ecSm.push_back(s);
        CHECK(r1ecSm.size() == 4);
        if (r1ecSm.size() == 4)
        {
            CHECK(r1ecSm[0].StartAddress == 0x1000 && r1ecSm[0].ControlByte == 0x26 && r1ecSm[0].Purpose == "MBoxOut");
            CHECK(r1ecSm[2].Index == 2 && r1ecSm[2].StartAddress == 0x1800 && r1ecSm[2].ControlByte == 0x64 && r1ecSm[2].Enable && r1ecSm[2].Purpose == "Outputs");
            CHECK(r1ecSm[3].Index == 3 && r1ecSm[3].StartAddress == 0x1C00 && r1ecSm[3].ControlByte == 0x20 && r1ecSm[3].Purpose == "Inputs");
        }
        CHECK(EsiCatalog::LoadFromFolder(dir + "\\does-not-exist").empty());
    }
}

int RunEtherCatSimTests()
{
    testkit::Group g("EtherCatSim");
    SimTests();
    TwoAxisSlaveTests();
    EsiTests();
    return g.Result();
}
