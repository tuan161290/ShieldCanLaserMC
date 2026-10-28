#include "EtherCat/Soem/SoemEtherCatBus.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <stdexcept>

#include "Base/Text.h"
#include "EtherCat/Soem/ProcessImage.h"

// Sau mọi header chuẩn C++ — xem ghi chú trong SoemInclude.h.
#include "EtherCat/Soem/SoemInclude.h"
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

// Định nghĩa trong ThirdParty/soem/src/ec_config.c (bản vá của thư viện EtherCatMaster): số bit
// ecx_map_coe_soe cộng thêm vào kết quả nó tự đọc được qua SDO cho một slave, TRƯỚC khi ecx_map_sm
// định cỡ SyncManager của ESC từ Ibits/Obits và slave chuyển sang SAFE-OP. Vá bằng FPWR sau khi đã
// chuyển trạng thái từng được thử và xác nhận KHÔNG giữ được — firmware R1EC tự đặt lại cỡ nó đã đồng
// ý lúc chuyển trạng thái, nên cỡ phải đúng ngay từ đầu.
extern "C" uint16 ecx_userExtraIbits[EC_MAXSLAVE + 1];
extern "C" uint16 ecx_userExtraObits[EC_MAXSLAVE + 1];

namespace ecm
{
    namespace
    {
        // ══════════════════════════════════════════════════════════════
        //  Chỉ số đối tượng (đồng bộ với ecm::CiA402Registers)
        // ══════════════════════════════════════════════════════════════
        const uint16 IDX_CONTROLWORD          = 0x6040;
        const uint16 IDX_TARGET_POSITION      = 0x607A;
        const uint16 IDX_ERROR_CODE           = 0x603F;
        const uint16 IDX_STATUSWORD           = 0x6041;
        const uint16 IDX_MODE_DISPLAY         = 0x6061;
        const uint16 IDX_POSITION_ACTUAL      = 0x6064;
        const uint16 IDX_MODES_OF_OPERATION   = 0x6060; // SINT,  1 byte  — chỉ SDO, không nằm trong PDO mặc định
        const uint16 IDX_PROFILE_VELOCITY     = 0x6081; // UDINT, 4 byte  — chỉ SDO
        const uint16 IDX_PROFILE_ACCELERATION = 0x6083; // UDINT, 4 byte  — chỉ SDO
        const uint16 IDX_PROFILE_DECELERATION = 0x6084; // UDINT, 4 byte  — chỉ SDO
        const uint16 IDX_VELOCITY_ACTUAL      = 0x606C; // DINT,  4 byte  — chỉ SDO
        const uint16 IDX_TORQUE_ACTUAL        = 0x6077; // INT,   2 byte  — chỉ SDO
        const uint16 IDX_ENCODER_RESOLUTION   = 0x608F; // UDINT x2 (sub1 increments, sub2 motor revs) — chỉ ScanSlaves đọc

        // Gốc của các đối tượng PDO assign (0x1C10 + chỉ số SM: 0x1C12 cho SM2/ngõ ra, 0x1C13 cho
        // SM3/ngõ vào) — đối tượng CoE bắt buộc theo chuẩn EtherCAT.
        const uint16 SDO_PDO_ASSIGN_BASE = 0x1C10;

        // Modular Device Profile (ETG.5001): dải chỉ số của bộ ghép I/O số (Leadshine R1EC…). Từ chẩn
        // đoán của chính bộ ghép (0xF100:01 Device status) được map vào CÙNG SM3/TxPdo với các kênh DI
        // và đứng TRƯỚC chúng, nên phải lọc theo chỉ số chứ không được coi nó là entry 0.
        const uint16 MDP_INPUT_INDEX_MIN  = 0x6000;
        const uint16 MDP_INPUT_INDEX_MAX  = 0x6FFF;
        const uint16 MDP_OUTPUT_INDEX_MIN = 0x7000;
        const uint16 MDP_OUTPUT_INDEX_MAX = 0x7FFF;
        const uint16 IDX_DEVICE_STATUS    = 0xF100;
        // 0xF100:01 là DINT 32 bit (TxPdo 0x1BFA trong ESI của R1EC).
        const int DEVICE_STATUS_BYTES = 4;

        const int IOMAP_SIZE = 4096;
        const int MAX_SLAVES = EC_MAXSLAVE;
        // TxPdo SM3 mặc định của R1EC đã 65 entry (1 Device status + 2×32 bit DI, mỗi bit một entry),
        // nên con số này phải đủ cho module I/O tệ nhất chứ không chỉ một trục.
        const int MAX_PDO_ENTRIES = 256;

        // GetActualVelocity/GetActualTorque là giá trị hiển thị "được thì tốt", đọc lặp lại suốt lúc
        // bus mở. Một câu trả lời SDO bị lỡ không được phép treo vòng quét trạng thái tới
        // EC_TIMEOUTRXM (700 ms) — hết 20 ms thì trả giá trị 0 của lần này.
        const int EC_TIMEOUT_BESTEFFORT_POLL = 20000;

        struct NotSupported : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        /// <summary>Một đối tượng được map vào Rx/TxPdo của slave, kèm offset byte/bit đã tính.</summary>
        struct PdoEntry
        {
            uint16 index;
            uint8 subIndex;
            int byteOffset;
            int bitOffset;
            /// <summary>Độ rộng bit lấy thẳng từ byte thấp của entry mapping. Cần vì một module I/O số
            /// có thể map kênh thành N BOOL 1 bit HOẶC vài word 16 bit — số kênh chỉ suy ra được bằng
            /// cách cộng độ rộng, không phải đếm entry.</summary>
            uint8 bitLen;
        };

        /// <summary>Offset byte của các đối tượng EtherCatMasterService đọc/ghi mỗi chu kỳ, tính từ
        /// đầu vùng ngõ ra/ngõ vào của chính slave đó.</summary>
        struct PdoLayout
        {
            int outControlword;
            int outTargetPosition;
            int inErrorCode;
            int inStatusword;
            int inModeDisplay;
            int inPosition;
            /// <summary>Offset của 0x6060 Modes of Operation khi driver map nó vào RxPdo (PDO mặc định của
            /// Welcon WE2x có). Khi đã map thì PHẢI ghi qua PDO: giá trị PDO đè lên SDO mỗi chu kỳ, và vùng ngõ ra
            /// còn 0 thì driver đứng ở mode 0. -1 = không map (Leadshine) → ghi SDO như trước.</summary>
            int outModeOfOperation = -1;
        };

        /// <summary>Offset byte/bit + số kênh DI/DO của một module I/O số, tính từ đầu vùng ngõ
        /// vào/ngõ ra của slave. inputBitCount/outputBitCount = 0 với module chỉ có một chiều.</summary>
        struct IoLayout
        {
            int inputByteOffset;
            int inputBitOffset;
            int inputBitCount;
            int outputByteOffset;
            int outputBitOffset;
            int outputBitCount;
            /// <summary>Offset của từ chẩn đoán 0xF100:01 của bộ ghép; -1 nếu slave không map.</summary>
            int deviceStatusByteOffset;
        };

        // ══════════════════════════════════════════════════════════════
        //  Hook cấu hình PRE-OP → SAFE-OP (PO2SOconfig của SOEM)
        //
        //  Bộ ghép modular như Leadshine R1EC phải được báo qua CoE rằng rack đang cắm module nào
        //  (0xF030 Configured Module Ident List) và PDO nào của chúng vào SyncManager nào
        //  (0x1C12/0x1C13) — ESI của nó khai cả hai SM process-data với DefaultSize="0", nên không
        //  có gì được map cho tới khi master nói. Làm đúng việc đó cần đủ cả ba điều kiện:
        //    * slave đang ở PRE-OP     — 0xF030 bị từ chối (read-only) ở SAFE-OP,
        //    * mailbox CoE đã chạy     — SOEM chỉ dựng nó bên trong ecx_config_map_group,
        //    * xảy ra TRƯỚC khi SOEM đo cỡ process image — nếu không IOmap được dựng cho một bộ ghép
        //      chưa cấu hình và mọi byte ngõ vào mãi bằng 0.
        //  Không điểm nào trong code gọi thoả cả ba (mọi thứ tự hiển nhiên đã thử trên phần cứng
        //  thật, cái nào cũng hỏng một điều), riêng hook này thì có: ec_config.c gọi nó ở PRE-OP, sau
        //  khi mailbox đã lên, ngay trước "Find IO mapping in slave".
        //
        //  Hook là callback C trơn nên dữ liệu nó cần nằm ở biến mức file. Mỗi lúc chỉ có MỘT bus SOEM
        //  hoạt động trong tiến trình, nên dùng chung được.
        // ══════════════════════════════════════════════════════════════
        const int MAX_SLOTS = 16;

        // Hook tìm thấy/làm gì — đọc lại sau config_map_group để ghi log.
        unsigned int s_hookIdents[MAX_SLAVES + 1][MAX_SLOTS];
        int s_hookIdentCount[MAX_SLAVES + 1];
        char s_hookDiag[8192];
        int s_hookDiagLen;

        // Độ rộng RIÊNG của từng slot (không cộng dồn) — để ApplyModularCouplerIo xếp các slot nối
        // nhau trong vùng SM của bộ ghép và cấp cho GetIoSlotInputRegister/SetIoSlotOutputRegister
        // một cửa sổ theo từng module.
        int s_hookSlotInBits[MAX_SLAVES + 1][MAX_SLOTS];
        int s_hookSlotOutBits[MAX_SLAVES + 1][MAX_SLOTS];

        // Offset byte register của từng slot, tính từ đầu vùng ngõ vào/ngõ ra của bộ ghép. Ngõ vào
        // bắt đầu sau từ trạng thái của bộ ghép; ngõ ra bắt đầu từ 0 (SM2 không có từ trạng thái).
        int s_slotInByteOffset[MAX_SLAVES + 1][MAX_SLOTS];
        int s_slotOutByteOffset[MAX_SLAVES + 1][MAX_SLOTS];

        // ── Catalog module từ ESI (SetModuleCatalog): ident → module đó góp bao nhiêu bit vào/ra.
        // Không hỏi thiết bị được: cả từ điển đối tượng của R1EC chỉ có 13 đối tượng (đã kiểm sống
        // bằng dịch vụ SDO Information) và không có gì theo từng slot.
        const int MODCAT_MAX = 128;
        const int MODPDO_MAX_ENTRIES = 40;

        /// <summary>Mapping PDO của một module như ESI khai cho slot 0. ESI của R1EC ghi
        /// PdoUpload="false" PdoConfig="true": mapping không đọc ra được nhưng GHI VÀO được — và chưa
        /// ghi thì PDO rỗng, không góp gì vào process image dù SyncManager gán gì. Vậy đây không chỉ
        /// là thông tin định cỡ mà là dữ liệu của một bước cấu hình bắt buộc.</summary>
        struct ModPdo
        {
            int index;                               // chỉ số đối tượng PDO cho slot 0, vd. 0x1A00
            bool isFixed;                            // ESI Fixed="true": mapping nằm cứng trong firmware,
                                                     // KHÔNG được ghi (ghi quá subindex cao nhất của R1EC
                                                     // bị SDO abort 0x06090011 "Subindex does not exist")
            int entryCount;
            unsigned short entryIndex[MODPDO_MAX_ENTRIES];
            unsigned char entrySub[MODPDO_MAX_ENTRIES];
            unsigned char entryBits[MODPDO_MAX_ENTRIES];
        };

        unsigned int s_modIdent[MODCAT_MAX];
        int s_modInBits[MODCAT_MAX];
        int s_modOutBits[MODCAT_MAX];
        ModPdo s_modInPdo[MODCAT_MAX];
        ModPdo s_modOutPdo[MODCAT_MAX];
        ModPdo s_modInPdoAlt[MODCAT_MAX];
        ModPdo s_modOutPdoAlt[MODCAT_MAX];
        int s_modCatCount = 0;

        // SyncManager do ESI khai, cho slave mà EEPROM không có mục SII SyncManager cho các SM
        // process-data. SOEM chỉ lập trình một SM khi StartAddr khác 0 và chỉ điền trường đó từ SII,
        // nên với slave như vậy SM2/SM3 không bao giờ được ghi và slave từ chối SAFE-OP với AL status
        // 0x1E. Cỡ đủ cho cả thư mục DriverInfo: tràn bảng này là âm thầm rơi mất entry của hãng đứng
        // sau — đúng cách một lần tra cứu cho thiết bị nằm cuối danh sách từng trả về rỗng.
        const int SMCAT_MAX = 1024;
        struct SmCatalogEntry
        {
            unsigned int vendorId;
            unsigned int productCode;
            int index;
            int startAddress;
            int controlByte;
            bool enable;
        };
        SmCatalogEntry s_smCat[SMCAT_MAX];
        int s_smCatCount = 0;

        // Hook tính ra cho từng bộ ghép, ApplyModularCouplerIo dùng sau đó.
        int s_couplerInBytes[MAX_SLAVES + 1];
        int s_couplerOutBytes[MAX_SLAVES + 1];
        int s_couplerStatusBytes[MAX_SLAVES + 1];

        /// <summary>
        /// Bộ ghép modular đang trong giai đoạn bring-up. `active` chỉ đúng từ lúc
        /// ApplyModularCouplerIo sửa xong SyncManager tới lúc vòng PDO khởi động: ConnectAllAxis dựa
        /// vào nó để nhận kết quả định cỡ từ ESI thay cho phép đo CoE (luôn ra 0 với thiết bị này).
        ///
        /// <para>Bản C++/CLI còn định dùng cấu trúc này để đọc/ghi thẳng vùng SM của bộ ghép bằng
        /// FPRD/FPWR mỗi chu kỳ, nhưng đường đó chưa từng chạy: lúc khởi động timer nó xoá sạch cờ
        /// `active`, và dữ liệu module vẫn đi qua IOmap bình thường nhờ bản vá ecx_userExtraIbits. Bản
        /// này giữ đúng hành vi đã chạy trên máy thật — xoá cờ khi vòng PDO khởi động, I/O của bộ ghép
        /// đọc từ IOmap.</para>
        /// </summary>
        struct CouplerSlave
        {
            bool active;
            uint16 configadr;
        };
        CouplerSlave s_coupler[MAX_SLAVES + 1];

        void HookLog(const char* fmt, ...)
        {
            if (s_hookDiagLen >= (int)sizeof(s_hookDiag) - 1) return;
            va_list args;
            va_start(args, fmt);
            int n = vsnprintf(s_hookDiag + s_hookDiagLen, sizeof(s_hookDiag) - s_hookDiagLen, fmt, args);
            va_end(args);
            if (n > 0) s_hookDiagLen += n;
            if (s_hookDiagLen > (int)sizeof(s_hookDiag) - 1) s_hookDiagLen = (int)sizeof(s_hookDiag) - 1;
        }

        int LookupModuleIndex(unsigned int ident)
        {
            for (int i = 0; i < s_modCatCount; i++)
                if (s_modIdent[i] == ident) return i;
            return -1;
        }

        bool LookupModuleWidth(unsigned int ident, int* inBits, int* outBits)
        {
            int i = LookupModuleIndex(ident);
            if (i < 0) return false;
            *inBits = s_modInBits[i];
            *outBits = s_modOutBits[i];
            return true;
        }

        const SmCatalogEntry* LookupSyncManager(unsigned int vendorId, unsigned int productCode, int index)
        {
            for (int i = 0; i < s_smCatCount; i++)
            {
                if (s_smCat[i].index != index) continue;
                if (s_smCat[i].vendorId != vendorId || s_smCat[i].productCode != productCode) continue;
                return &s_smCat[i];
            }
            return nullptr;
        }

        /// <summary>Tải mapping của một PDO xuống slave: xoá số entry, ghi từng entry dạng
        /// (index &lt;&lt; 16) | (sub &lt;&lt; 8) | bitlength, rồi ghi lại số entry. Chỉ số đối tượng
        /// được dịch theo SlotIndexIncrement để slot N trỏ vào đối tượng dữ liệu của riêng nó. Trả số
        /// entry slave chấp nhận.</summary>
        int DownloadPdoMapping(ecx_contextt* context, uint16 slave, uint16 pdoIndex, const ModPdo* pdo, int slotIndexOffset)
        {
            uint8 zero = 0;
            if (ecx_SDOwrite(context, slave, pdoIndex, 0x00, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM) <= 0)
                return -1; // slave không cho xoá mapping

            int written = 0;
            int firstFailSub = -1;
            uint32 firstFailValue = 0;
            for (int e = 0; e < pdo->entryCount; e++)
            {
                uint32 value = ((uint32)(pdo->entryIndex[e] + slotIndexOffset) << 16)
                             | ((uint32)pdo->entrySub[e] << 8)
                             | (uint32)pdo->entryBits[e];
                if (ecx_SDOwrite(context, slave, pdoIndex, (uint8)(written + 1), FALSE, sizeof(value), &value, EC_TIMEOUTRXM) > 0)
                    written++;
                else if (firstFailSub < 0)
                {
                    firstFailSub = written + 1;
                    firstFailValue = value;
                }
            }
            if (firstFailSub >= 0)
                HookLog(" [0x%04X sub%d rejected val=0x%08X]", pdoIndex, firstFailSub, firstFailValue);

            uint8 count = (uint8)written;
            ecx_SDOwrite(context, slave, pdoIndex, 0x00, FALSE, sizeof(count), &count, EC_TIMEOUTRXM);
            return written;
        }

        /// <summary>Tải mapping cho một slot, thử lần lượt các biến thể của ESI. Module khai cùng các
        /// kênh hai lần — mỗi bit một entry, và gói thành word — mà thiết bị có thể chỉ đủ chỗ cho
        /// một: 0x1A00 của R1EC có 9 subindex nên 32 entry 1 bit không vừa, còn 0x1A01 dạng gói chỉ
        /// cần 2. Trả chỉ số PDO map được trọn vẹn, hoặc 0. Mapping ghi dở được xoá trước khi sang
        /// biến thể kế, nên biến thể bị từ chối không để lại cấu hình nửa vời.</summary>
        uint16 DownloadSlotMapping(ecx_contextt* context, uint16 slave, int slot,
                                   const ModPdo* primary, const ModPdo* alternate,
                                   int slotPdoIncrement, int slotIndexOffset)
        {
            const ModPdo* variants[2] = { primary, alternate };
            for (int v = 0; v < 2; v++)
            {
                const ModPdo* pdo = variants[v];
                if (pdo == nullptr || pdo->entryCount <= 0) continue;

                uint16 index = (uint16)(pdo->index + slot * slotPdoIncrement);

                // PDO mà ESI đánh dấu Fixed mang mapping nằm cứng trong firmware, không được ghi — đã
                // xác nhận trên R1EC: ghi entry vào 0x1A00 Fixed (thứ TwinCAT để yên và vẫn nhận được
                // image sống) chính là thứ bị từ chối sau subindex 9. Với nó, việc của master chỉ là
                // phần gán (PDO nào vào SyncManager nào).
                if (pdo->isFixed)
                {
                    HookLog(" map:0x%04X(fixed, not written)", index);
                    return index;
                }

                int mapped = DownloadPdoMapping(context, slave, index, pdo, slotIndexOffset);
                if (mapped == pdo->entryCount)
                {
                    HookLog(" map:0x%04X(%d entries)", index, mapped);
                    return index;
                }

                HookLog(" map:0x%04X rejected after %d/%d", index, mapped, pdo->entryCount);
                uint8 zero = 0;
                ecx_SDOwrite(context, slave, index, 0x00, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM);
            }
            return 0;
        }

        int PreOpToSafeOpConfig(ecx_contextt* context, uint16 slave)
        {
            static const uint16 IDX_DETECTED_MODULES   = 0xF050;
            static const uint16 IDX_CONFIGURED_MODULES = 0xF030;
            static const uint16 SM_ASSIGN_OUT = 0x1C12, SM_ASSIGN_IN = 0x1C13;
            static const uint16 COUPLER_STATUS_TXPDO = 0x1BFA; // 0xF100:01 Device status
            static const int SLOT_PDO_INCREMENT = 0x10;        // ESI <Slots SlotPdoIncrement="#x10">
            static const int SLOT_INDEX_INCREMENT = 0x10;      // ESI <Slots SlotIndexIncrement="#x10">

            uint8 count = 0;
            int size = sizeof(count);
            if (ecx_SDOread(context, slave, IDX_DETECTED_MODULES, 0x00, FALSE, &size, &count, EC_TIMEOUTRXM) <= 0
                || count == 0 || count > MAX_SLOTS)
                return 0; // không phải bộ ghép modular — không làm gì, để SOEM đi tiếp bình thường

            // 1. Chép rack đã dò vào danh sách cấu hình. Ghi số lượng trước mới định cỡ danh sách;
            //    các subindex theo slot chỉ ghi được khi danh sách đã dài chừng đó.
            uint32 idents[MAX_SLOTS];
            for (uint8 i = 1; i <= count; i++)
            {
                int isz = sizeof(uint32);
                idents[i - 1] = 0;
                ecx_SDOread(context, slave, IDX_DETECTED_MODULES, i, FALSE, &isz, &idents[i - 1], EC_TIMEOUTRXM);
            }
            ecx_SDOwrite(context, slave, IDX_CONFIGURED_MODULES, 0x00, FALSE, sizeof(count), &count, EC_TIMEOUTRXM);
            int identsOk = 0;
            for (uint8 i = 1; i <= count; i++)
                if (ecx_SDOwrite(context, slave, IDX_CONFIGURED_MODULES, i, FALSE, sizeof(uint32), &idents[i - 1], EC_TIMEOUTRXM) > 0)
                    identsOk++;

            s_hookIdentCount[slave] = count;
            for (int i = 0; i < count; i++)
            {
                s_hookIdents[slave][i] = idents[i];
                s_hookSlotInBits[slave][i] = 0;   // chỉ đặt bên dưới nếu mapping của slot này tải xuống được
                s_hookSlotOutBits[slave][i] = 0;
            }
            HookLog(" | Slave %d: %d module(s) detected, %d/%d written to 0xF030", slave, count, identsOk, count);

            // 2. Map PDO của từng slot vào SyncManager, định cỡ theo độ rộng module lấy từ ESI. Bỏ
            //    hẳn bước này trừ khi MỌI ident đều tra được, vì gán PDO của slot mà không biết độ
            //    rộng để bộ ghép ở trạng thái tệ hơn mặc định của nó: đối tượng PDO theo slot không
            //    tồn tại trên firmware này, bộ ghép vẫn NHẬN lệnh ghi, và SOEM đo image thành 4 vào / 0 ra.
            //
            //    Mặc định cũng chẳng hơn — kiểm trên phần cứng vừa cấp nguồn lại: bộ ghép để yên báo
            //    phép gán rỗng (0x1C12/0x1C13 đều đếm 0) và SOEM rơi về 7/5 cố định từ EEPROM, không
            //    mang dữ liệu module nào. Vậy bước này là thứ làm module hiện ra; nó chỉ cần độ rộng đi kèm.
            int slotInBits[MAX_SLOTS] = { 0 }, slotOutBits[MAX_SLOTS] = { 0 };
            int resolved = 0;
            for (int slot = 0; slot < count; slot++)
                if (LookupModuleWidth(idents[slot], &slotInBits[slot], &slotOutBits[slot]))
                    resolved++;

            if (s_modCatCount == 0 || resolved < count)
            {
                HookLog(" | Slave %d: ESI widths missing for %d/%d slot(s) - leaving SM assignment at the coupler default",
                        slave, count - resolved, count);
                return 0;
            }

            uint8 zero = 0;
            ecx_SDOwrite(context, slave, SM_ASSIGN_IN,  0x00, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM);
            ecx_SDOwrite(context, slave, SM_ASSIGN_OUT, 0x00, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM);

            uint8 nIn = 0, nOut = 0;
            uint16 statusPdo = COUPLER_STATUS_TXPDO; // giữ 0xF100:01, map TRƯỚC các kênh
            if (ecx_SDOwrite(context, slave, SM_ASSIGN_IN, (uint8)(nIn + 1), FALSE, sizeof(statusPdo), &statusPdo, EC_TIMEOUTRXM) > 0)
                nIn++;

            int totalInBytes = 0, totalOutBytes = 0;
            for (int slot = 0; slot < count; slot++)
            {
                int cat = LookupModuleIndex(idents[slot]);
                if (cat < 0) continue; // đã tra được ở trên, không thể xảy ra — nhưng không bao giờ index bằng -1
                int slotIndexOffset = slot * SLOT_INDEX_INCREMENT;

                if (slotInBits[slot] > 0)
                {
                    // Tải mapping TRƯỚC. PDO đã gán mà rỗng thì không góp gì — đúng cách bộ ghép này
                    // rơi vào image ngõ vào 4 byte chỉ có Device status. Thiết bị không cho hỏi mapping
                    // (PdoUpload="false") nên lấy từ ESI.
                    uint16 pdo = DownloadSlotMapping(context, slave, slot, &s_modInPdo[cat], &s_modInPdoAlt[cat],
                                                     SLOT_PDO_INCREMENT, slotIndexOffset);
                    if (pdo != 0
                        && ecx_SDOwrite(context, slave, SM_ASSIGN_IN, (uint8)(nIn + 1), FALSE, sizeof(pdo), &pdo, EC_TIMEOUTRXM) > 0)
                    {
                        nIn++;
                        HookLog(" in:0x%04X(%db)", pdo, slotInBits[slot]);
                        totalInBytes += (slotInBits[slot] + 7) / 8;
                        s_hookSlotInBits[slave][slot] = slotInBits[slot];
                    }
                }
                if (slotOutBits[slot] > 0)
                {
                    uint16 pdo = DownloadSlotMapping(context, slave, slot, &s_modOutPdo[cat], &s_modOutPdoAlt[cat],
                                                     SLOT_PDO_INCREMENT, slotIndexOffset);
                    if (pdo != 0
                        && ecx_SDOwrite(context, slave, SM_ASSIGN_OUT, (uint8)(nOut + 1), FALSE, sizeof(pdo), &pdo, EC_TIMEOUTRXM) > 0)
                    {
                        nOut++;
                        HookLog(" out:0x%04X(%db)", pdo, slotOutBits[slot]);
                        totalOutBytes += (slotOutBits[slot] + 7) / 8;
                        s_hookSlotOutBits[slave][slot] = slotOutBits[slot];
                    }
                }
            }

            if (ecx_iserror(context))
            {
                char* errs = ecx_elist2string(context);
                HookLog(" | Slave %d SDO errors: %.400s", slave, errs != nullptr ? errs : "(none)");
            }

            int wIn  = ecx_SDOwrite(context, slave, SM_ASSIGN_IN,  0x00, FALSE, sizeof(nIn),  &nIn,  EC_TIMEOUTRXM);
            int wOut = ecx_SDOwrite(context, slave, SM_ASSIGN_OUT, 0x00, FALSE, sizeof(nOut), &nOut, EC_TIMEOUTRXM);
            HookLog(" | Slave %d: assigned SM3=%d PDO(s) (wkc=%d), SM2=%d PDO(s) (wkc=%d)", slave, nIn, wIn, nOut, wOut);

            // 3. Cấp cho SOEM các SyncManager process-data nếu lần đọc SII của nó không thấy. ecx_map_sm
            //    (chạy ngay sau khi hook trả về) bỏ qua mọi SM có StartAddr = 0, nên thiếu bước này
            //    SM2/SM3 không bao giờ được lập trình trên ESC và slave trả lời yêu cầu SAFE-OP bằng AL
            //    status 0x1E "Invalid input configuration". Độ dài ghi ở đây là tạm — ecx_map_sm ghi đè
            //    từ phép đo 0 bit của nó, ApplyModularCouplerIo sửa lại sau.
            for (int n = 2; n <= 3; n++)
            {
                if (context->slavelist[slave].SM[n].StartAddr != 0)
                    continue; // EEPROM đã mô tả — để nguyên giá trị của SOEM
                const SmCatalogEntry* e = LookupSyncManager(context->slavelist[slave].eep_man,
                                                            context->slavelist[slave].eep_id, n);
                if (e == nullptr || e->startAddress == 0)
                {
                    HookLog(" | Slave %d: SM%d unconfigured, no ESI entry (catalog=%d entries, looking for man=0x%X id=0x%X)",
                            slave, n, s_smCatCount,
                            (unsigned int)context->slavelist[slave].eep_man, (unsigned int)context->slavelist[slave].eep_id);
                    continue;
                }
                context->slavelist[slave].SM[n].StartAddr = htoes((uint16)e->startAddress);
                context->slavelist[slave].SM[n].SMflags = htoel((uint32)e->controlByte | (e->enable ? 0x00010000u : 0u));
                context->slavelist[slave].SMtype[n] = (n == 2) ? 3 : 4; // 3 = ngõ ra, 4 = ngõ vào
                HookLog(" | Slave %d: SM%d from ESI @0x%04X ctrl=0x%02X", slave, n, e->startAddress, e->controlByte);
            }

            // Bảo ecx_map_coe_soe (sắp chạy, ngay sau khi hook trả về) cộng số byte module vào kết quả
            // nó tự đọc được (chỉ có 0x1BFA Device status) — để Ibits/Obits đúng TRƯỚC khi ecx_map_sm
            // định cỡ SyncManager và slave chuyển sang SAFE-OP.
            if (slave <= EC_MAXSLAVE)
            {
                ecx_userExtraIbits[slave] = (uint16)(totalInBytes * 8);
                ecx_userExtraObits[slave] = (uint16)(totalOutBytes * 8);
            }

            // Chuyển số đo cho ApplyModularCouplerIo, chạy khi IOmap đã có.
            s_couplerStatusBytes[slave] = DEVICE_STATUS_BYTES; // 0x1BFA, gán trước các kênh
            s_couplerInBytes[slave] = totalInBytes;
            s_couplerOutBytes[slave] = totalOutBytes;
            return 0;
        }

        void CopyPdoInfo(const std::optional<EsiPdoInfo>& src, ModPdo* dst)
        {
            memset(dst, 0, sizeof(ModPdo));
            if (!src.has_value()) return;

            dst->index = src->Index;
            dst->isFixed = src->IsFixed;
            for (const EsiPdoEntryInfo& e : src->Entries)
            {
                if (e.BitLength <= 0 || e.BitLength > 255) continue;
                if (dst->entryCount >= MODPDO_MAX_ENTRIES) break;
                dst->entryIndex[dst->entryCount] = (unsigned short)e.Index;
                dst->entrySub[dst->entryCount] = (unsigned char)e.SubIndex;
                dst->entryBits[dst->entryCount] = (unsigned char)e.BitLength;
                dst->entryCount++;
            }
        }

        /// <summary>Mặt nạ đủ rộng cho tới 64 kênh — 1ULL &lt;&lt; 64 là hành vi không xác định trong C++.</summary>
        inline uint64 FullBitMask(int bitCount)
        {
            return (bitCount >= 64) ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << bitCount) - 1);
        }

        inline uint32 FullBitMask32(int bitCount)
        {
            return (bitCount >= 32) ? 0xFFFFFFFFu : ((1u << bitCount) - 1);
        }

        void InitIoLayout(std::vector<IoLayout>& layouts)
        {
            // Chỉ memset thì deviceStatusByteOffset = 0, bị đọc nhầm thành "map ở byte 0" cho mọi slave
            // không qua DiscoverIoLayout (mọi slot trục).
            IoLayout empty{};
            empty.deviceStatusByteOffset = -1;
            layouts.assign(MAX_SLAVES + 1, empty);
        }

        HMODULE g_wpcap = nullptr;
        std::once_flag g_wpcapOnce;

        /// <summary>
        /// Trần ưu tiên cho mọi giao dịch mailbox (SDO) trong lúc vòng PDO đang chạy.
        ///
        /// <para>Luồng SDO và luồng chu kỳ dùng chung các critical section của nicdrv (getindex_mutex,
        /// rx_mutex): ai đang chờ khung trả lời đều vào ecx_inframe, giữ rx_mutex trong lúc lấy gói từ Npcap.
        /// Giữ chỉ vài micro giây — nhưng nếu luồng SDO ở mức ưu tiên thường bị một luồng khác giành CPU
        /// ĐÚNG lúc đang giữ, luồng chu kỳ (TIME_CRITICAL, core riêng) phải chờ hết lượt của luồng kia,
        /// 15–30 ms trên Windows: đủ để driver thiếu khung và báo watchdog. Critical section của Windows
        /// không có kế thừa ưu tiên, nên ta tự làm: luồng nào gọi SOEM thì trong lúc gọi chạy cùng mức
        /// với luồng chu kỳ. Luồng đó nằm trên các core còn lại (không bao giờ trên core dành riêng), và
        /// phần lớn thời gian của một lần SDO là ngủ chờ slave trả lời, nên không chiếm CPU của ai.</para>
        /// </summary>
        class MailboxPriorityScope
        {
        public:
            MailboxPriorityScope()
                : m_thread(GetCurrentThread()), m_previous(GetThreadPriority(GetCurrentThread()))
            {
                if (m_previous != THREAD_PRIORITY_ERROR_RETURN && m_previous < THREAD_PRIORITY_TIME_CRITICAL)
                    m_changed = SetThreadPriority(m_thread, THREAD_PRIORITY_TIME_CRITICAL) != FALSE;
            }
            ~MailboxPriorityScope()
            {
                if (m_changed) SetThreadPriority(m_thread, m_previous);
            }
            MailboxPriorityScope(const MailboxPriorityScope&) = delete;
            MailboxPriorityScope& operator=(const MailboxPriorityScope&) = delete;

        private:
            HANDLE m_thread;
            int m_previous;
            bool m_changed{ false };
        };
    }

    // ══════════════════════════════════════════════════════════════
    //  Impl
    // ══════════════════════════════════════════════════════════════

    struct SoemEtherCatBus::Impl
    {
        SoemEtherCatBus* owner{};

        ecx_contextt* context{};
        uint8_t* ioMap{};
        bool isOpen{};
        std::vector<PdoLayout> layout;      // theo (chỉ số slave của SOEM 1-based, kênh trục) — xem Layout()
        std::vector<IoLayout> ioLayout;

        /// <summary>Hook PreOpToSafeOpConfig (và ApplyModularCouplerIo) thấy/làm gì ở lần kết nối gần
        /// nhất — nối vào chẩn đoán "không tới OPERATIONAL" để lỗi bring-up bộ ghép hiện kèm danh
        /// sách module đã dò.</summary>
        std::string moduleIdentDiag;
        /// <summary>DiscoverIoLayout tìm thấy gì lúc bring-up (entry PDO-assign thật hay số liệu
        /// EEPROM) — chụp khi còn ở SAFE-OP vì R1EC ngừng trả lời mailbox khi đã OPERATIONAL.</summary>
        std::string ioLayoutDiag;
        std::vector<std::vector<uint32_t>> detectedModules;
        std::string lastOpenError;

        mutable std::mutex optionsMutex;
        rt::RtOptions rtOptions;
        std::string rtInfo;

        rt::RtCyclicThread rtThread;
        rt::ProcessImage image;

        void Allocate()
        {
            context = new ecx_contextt();
            memset(context, 0, sizeof(ecx_contextt));
            ioMap = new uint8_t[IOMAP_SIZE];
            memset(ioMap, 0, IOMAP_SIZE);
            layout.assign((size_t)(MAX_SLAVES + 1) * AxisAddress::MaxChannels, PdoLayout{});
            InitIoLayout(ioLayout);
            moduleIdentDiag.clear();
            ioLayoutDiag.clear();
            detectedModules.assign(MAX_SLAVES + 1, {});
        }

        void FreeNative()
        {
            delete context;
            context = nullptr;
            delete[] ioMap;
            ioMap = nullptr;
            layout.clear();
            ioLayout.clear();
            isOpen = false;
        }

        void EnsureOpen() const
        {
            if (context == nullptr || !isOpen)
                throw std::runtime_error("EtherCAT bus is not open - call Open() first.");
        }

        /// <summary>Nhận vị trí slave hoặc địa chỉ trục (AxisAddress) — phần kênh bỏ qua ở đây.</summary>
        uint16 ResolveSlave(int slavePosition) const
        {
            EnsureOpen();
            // Slave 0 của SOEM là slave ảo broadcast; slave thật đánh số từ 1 theo thứ tự chuỗi.
            slavePosition = AxisAddress::Slave(slavePosition);
            int idx = slavePosition + 1;
            if (idx < 1 || idx > context->slavecount)
                throw std::out_of_range(std::format("No EtherCAT slave at position {} ({} slave(s) found on the bus)",
                                                    slavePosition, context->slavecount));
            return (uint16)idx;
        }

        /// <summary>Layout PDO của một trục: slave SOEM (1-based) + kênh trục trong slave.</summary>
        PdoLayout& Layout(uint16 slave, int channel)
        {
            return layout[(size_t)slave * AxisAddress::MaxChannels + (size_t)channel];
        }

        /// <summary>Layout PDO theo địa chỉ trục — ResolveSlave trước (ném nếu không có slave).</summary>
        PdoLayout& AxisLayout(int address, uint16& slave)
        {
            slave = ResolveSlave(address);
            return Layout(slave, AxisAddress::Channel(address));
        }

        // Offset của vùng ngõ ra/ngõ vào của một slave tính từ đầu vùng của group — toạ độ mà
        // ProcessImage dùng. Slave không có vùng tương ứng trả một offset chắc chắn ngoài phạm vi để
        // ProcessImage tự bỏ qua.
        size_t OutBase(uint16 slave) const
        {
            const uint8* p = context->slavelist[slave].outputs;
            const uint8* g = context->grouplist[0].outputs;
            return (p == nullptr || g == nullptr || p < g) ? (size_t)IOMAP_SIZE * 2 : (size_t)(p - g);
        }

        size_t InBase(uint16 slave) const
        {
            const uint8* p = context->slavelist[slave].inputs;
            const uint8* g = context->grouplist[0].inputs;
            return (p == nullptr || g == nullptr || p < g) ? (size_t)IOMAP_SIZE * 2 : (size_t)(p - g);
        }

        bool OpenSocket(const std::string& networkAdapter)
        {
            lastOpenError.clear();
            std::string npcapError;
            if (!SoemEtherCatBus::IsNpcapAvailable(&npcapError))
            {
                lastOpenError = npcapError;
                owner->OnLog.Raise(npcapError);
                return false;
            }
            if (ecx_init(context, networkAdapter.c_str()) <= 0)
            {
                lastOpenError = "Cannot open network adapter '" + networkAdapter + "' via Npcap (wrong device name, adapter disabled, or insufficient rights)";
                return false;
            }
            isOpen = true; // từ đây socket đã mở — Close() phải chạy ecx_close kể cả khi các bước sau hỏng
            return true;
        }

        // ── Vòng PDO ──

        static int Cycle(void* user)
        {
            auto* self = static_cast<Impl*>(user);
            self->image.ApplyPendingOutputs();
            ecx_send_processdata(self->context);
            int wkc = ecx_receive_processdata(self->context, EC_TIMEOUTRET);
            self->image.PublishInputs();
            return wkc;
        }

        void StopCyclic()
        {
            rtThread.Stop();   // chờ luồng thoát hẳn: sau dòng này không còn ai đụng context/IOmap
            image.Detach();
            memset(s_coupler, 0, sizeof(s_coupler));
        }

        void StartCyclic()
        {
            StopCyclic();

            ec_groupt& group = context->grouplist[0];
            image.Attach(group.outputs, group.Obytes, group.inputs, group.Ibytes);
            int expectedWkc = (int)group.outputsWKC * 2 + (int)group.inputsWKC;

            rt::RtOptions options;
            {
                std::lock_guard<std::mutex> lock(optionsMutex);
                options = rtOptions;
            }
            std::string info;
            rtThread.Start(options, &Impl::Cycle, this, expectedWkc, &info);
            {
                std::lock_guard<std::mutex> lock(optionsMutex);
                rtInfo = info;
            }
            owner->OnLog.Raise(info);
        }

        /// <summary>Đoạn cuối bring-up dùng chung cho Open() và ConnectAllAxis(): cấu hình DC, chờ
        /// SAFE-OP, trao đổi vài vòng cho ngõ ra ổn định, rồi xin và chờ OPERATIONAL.</summary>
        bool BringToOperational(int* settleWkcOut, int* opWkcOut)
        {
            ecx_configdc(context);
            ecx_statecheck(context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);

            // Đã thử bật SYNC0 thật (ecx_dcsync0) trên R1EC — ecx_configdc chỉ đo trễ truyền/offset,
            // không tự khởi động SYNC0. hasdc=true với R1EC, bật SYNC0 không đổi gì (vẫn AL status
            // 0x19, WKC vẫn tốt) — đã loại trừ, không đáng liều đổi cách đồng bộ của các trục đang
            // chạy tốt, nên để free-run.

            // Trao đổi vài vòng cho ngõ ra của slave ổn định trước khi xin OPERATIONAL. Một vòng
            // không đủ cho bộ ghép R1EC trên phần cứng thật — nó từ chối OP với AL status 0x19 "No
            // valid outputs" dù PDO mapping đúng.
            int settleWkc = 0;
            for (int settle = 0; settle < 10; settle++)
            {
                ecx_send_processdata(context);
                settleWkc = ecx_receive_processdata(context, EC_TIMEOUTRET);
                Sleep(2);
            }

            context->slavelist[0].state = EC_STATE_OPERATIONAL;
            ecx_writestate(context, 0);

            // Xin OPERATIONAL trên TỪNG slave nữa, không chỉ broadcast slave 0. Đã xác nhận cần trên
            // phần cứng thật: chỉ broadcast thì trạng thái truyền thông lên được (process data chạy,
            // statusword cập nhật) nhưng driver kẹt ở CiA402 "Switched On", không bao giờ Enable
            // Operation xong.
            for (int i = 1; i <= context->slavecount && i <= MAX_SLAVES; i++)
            {
                context->slavelist[i].state = EC_STATE_OPERATIONAL;
                ecx_writestate(context, (uint16)i);
            }

            int opWkc = 0;
            bool reached = false;
            for (int i = 0; i < 40 && !reached; i++)
            {
                ecx_send_processdata(context);
                opWkc = ecx_receive_processdata(context, EC_TIMEOUTRET);
                ecx_statecheck(context, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 40);
                reached = (context->slavelist[0].state == EC_STATE_OPERATIONAL);
            }

            if (settleWkcOut != nullptr) *settleWkcOut = settleWkc;
            if (opWkcOut != nullptr) *opWkcOut = opWkc;
            return reached;
        }

        // ── Dò PDO sống qua CoE (0x1C12/0x1C13 assign + mapping họ 0x1600/0x1A00) — chạy với mọi
        // slave CiA402, không cần bảng theo hãng. Cùng thuật toán với si_PDOassign() trong
        // samples/slaveinfo của SOEM, đổi thành ghi lại offset thay vì in ra.

        int ReadPdoAssignEntries(uint16 slave, uint16 pdoAssignIndex, PdoEntry* entries, int maxEntries, int* entryCount)
        {
            *entryCount = 0;
            int bitoffset = 0;

            uint16 numPdos = 0;
            int rdl = sizeof(numPdos);
            int wkc = ecx_SDOread(context, slave, pdoAssignIndex, 0x00, FALSE, &rdl, &numPdos, EC_TIMEOUTRXM);
            if (wkc <= 0 || numPdos == 0)
                return 0;

            for (uint16 i = 1; i <= numPdos; i++)
            {
                uint16 pdoIndex = 0;
                rdl = sizeof(pdoIndex);
                ecx_SDOread(context, slave, pdoAssignIndex, (uint8)i, FALSE, &rdl, &pdoIndex, EC_TIMEOUTRXM);
                if (pdoIndex == 0)
                    continue;

                uint8 subCount = 0;
                rdl = sizeof(subCount);
                ecx_SDOread(context, slave, pdoIndex, 0x00, FALSE, &rdl, &subCount, EC_TIMEOUTRXM);

                for (uint8 s = 1; s <= subCount; s++)
                {
                    uint32 mapEntry = 0;
                    rdl = sizeof(mapEntry);
                    ecx_SDOread(context, slave, pdoIndex, s, FALSE, &rdl, &mapEntry, EC_TIMEOUTRXM);

                    uint8 bitLen = (uint8)(mapEntry & 0xFF);
                    uint8 objSub = (uint8)((mapEntry >> 8) & 0xFF);
                    uint16 objIdx = (uint16)(mapEntry >> 16);

                    if (*entryCount < maxEntries)
                    {
                        entries[*entryCount].index = objIdx;
                        entries[*entryCount].subIndex = objSub;
                        entries[*entryCount].byteOffset = bitoffset / 8;
                        entries[*entryCount].bitOffset = bitoffset % 8;
                        entries[*entryCount].bitLen = bitLen;
                        (*entryCount)++;
                    }
                    bitoffset += bitLen;
                }
            }
            return bitoffset; // tổng số bit đã map
        }

        static int FindPdoOffset(const PdoEntry* entries, int entryCount, uint16 index, uint8 subIndex)
        {
            for (int i = 0; i < entryCount; i++)
                if (entries[i].index == index && entries[i].subIndex == subIndex)
                    return entries[i].byteOffset;
            return -1;
        }

        /// <summary>Dò layout PDO sống của một trục (slave + kênh). Ném NotSupported nếu PDO mặc định của
        /// slave thiếu một trong các đối tượng chuẩn cần dùng — báo lỗi to còn hơn đọc một offset sai. Kênh n tìm
        /// các đối tượng ở 0x6000 + n × 0x800 (driver nhiều trục, ETG.6010).</summary>
        PdoLayout DiscoverPdoLayout(uint16 slave, int channel = 0)
        {
            const uint16 off = (uint16)(channel * AxisAddress::ChannelStride);
            std::vector<PdoEntry> rxEntries(MAX_PDO_ENTRIES), txEntries(MAX_PDO_ENTRIES);
            int rxCount = 0, txCount = 0;
            ReadPdoAssignEntries(slave, SDO_PDO_ASSIGN_BASE + 2, rxEntries.data(), MAX_PDO_ENTRIES, &rxCount); // 0x1C12, SM2/ngõ ra
            ReadPdoAssignEntries(slave, SDO_PDO_ASSIGN_BASE + 3, txEntries.data(), MAX_PDO_ENTRIES, &txCount); // 0x1C13, SM3/ngõ vào

            PdoLayout l;
            l.outControlword     = FindPdoOffset(rxEntries.data(), rxCount, (uint16)(IDX_CONTROLWORD + off), 0);
            l.outTargetPosition  = FindPdoOffset(rxEntries.data(), rxCount, (uint16)(IDX_TARGET_POSITION + off), 0);
            l.outModeOfOperation = FindPdoOffset(rxEntries.data(), rxCount, (uint16)(IDX_MODES_OF_OPERATION + off), 0);
            l.inErrorCode        = FindPdoOffset(txEntries.data(), txCount, (uint16)(IDX_ERROR_CODE + off), 0);
            l.inStatusword       = FindPdoOffset(txEntries.data(), txCount, (uint16)(IDX_STATUSWORD + off), 0);
            l.inModeDisplay      = FindPdoOffset(txEntries.data(), txCount, (uint16)(IDX_MODE_DISPLAY + off), 0);
            l.inPosition         = FindPdoOffset(txEntries.data(), txCount, (uint16)(IDX_POSITION_ACTUAL + off), 0);

            if (l.outControlword < 0 || l.outTargetPosition < 0 || l.inStatusword < 0 || l.inPosition < 0)
            {
                throw NotSupported(std::format(
                    "Slave {} axis channel {}: default PDO does not map one of the objects EtherCatMasterService needs "
                    "(Controlword 0x{:04X}, Target Position 0x{:04X}, Statusword 0x{:04X}, Position Actual "
                    "0x{:04X}). Found: outCW={} outPos={} inErr={} inSW={} inMode={} inPos={}. "
                    "This drive may need its PDO reconfigured (mode other than pp/csp?) before it can "
                    "be used with EtherCatMasterService.",
                    slave, channel, IDX_CONTROLWORD + off, IDX_TARGET_POSITION + off, IDX_STATUSWORD + off,
                    IDX_POSITION_ACTUAL + off, l.outControlword, l.outTargetPosition, l.inErrorCode,
                    l.inStatusword, l.inModeDisplay, l.inPosition));
            }
            return l;
        }

        /// <summary>Dò layout bit DI/DO sống của một module I/O số. Ném NotSupported khi thật sự
        /// không có gì được map, hoặc số bit vượt cửa sổ 64 bit.</summary>
        IoLayout DiscoverIoLayout(uint16 slave)
        {
            std::vector<PdoEntry> rxEntries(MAX_PDO_ENTRIES), txEntries(MAX_PDO_ENTRIES);
            int rxCount = 0, txCount = 0;
            ReadPdoAssignEntries(slave, SDO_PDO_ASSIGN_BASE + 2, rxEntries.data(), MAX_PDO_ENTRIES, &rxCount); // ngõ ra (DO)
            ReadPdoAssignEntries(slave, SDO_PDO_ASSIGN_BASE + 3, txEntries.data(), MAX_PDO_ENTRIES, &txCount); // ngõ vào (DI)

            IoLayout l{};
            l.deviceStatusByteOffset = -1;

            // Ngõ vào: chỉ giữ entry có chỉ số trong dải DI của MDP (0x6000-0x6FFF). 0xF100 (Device
            // status) và mọi thứ ngoài dải là chẩn đoán, không phải kênh DI — ghi riêng, không làm
            // lệch gốc DI.
            bool haveFirstIn = false;
            for (int i = 0; i < txCount; i++)
            {
                uint16 idx = txEntries[i].index;
                if (idx == IDX_DEVICE_STATUS)
                {
                    l.deviceStatusByteOffset = txEntries[i].byteOffset;
                    continue;
                }
                if (idx < MDP_INPUT_INDEX_MIN || idx > MDP_INPUT_INDEX_MAX)
                    continue;
                if (!haveFirstIn)
                {
                    l.inputByteOffset = txEntries[i].byteOffset;
                    l.inputBitOffset = txEntries[i].bitOffset;
                    haveFirstIn = true;
                }
                // Cộng độ rộng, không đếm entry — module có thể map 32 kênh thành 32 BOOL hoặc hai
                // word 16 bit; đếm entry sẽ báo 2 kênh cho kiểu sau.
                l.inputBitCount += txEntries[i].bitLen;
            }

            // Ngõ ra: lọc tương tự theo dải DO của MDP (0x7000-0x7FFF).
            bool haveFirstOut = false;
            for (int i = 0; i < rxCount; i++)
            {
                uint16 idx = rxEntries[i].index;
                if (idx < MDP_OUTPUT_INDEX_MIN || idx > MDP_OUTPUT_INDEX_MAX)
                    continue;
                if (!haveFirstOut)
                {
                    l.outputByteOffset = rxEntries[i].byteOffset;
                    l.outputBitOffset = rxEntries[i].bitOffset;
                    haveFirstOut = true;
                }
                l.outputBitCount += rxEntries[i].bitLen;
            }

            // Rơi về cỡ SyncManager cho chiều nào mapping không mô tả. Hai loại slave cần: loại không
            // trả lời 0x1C12/0x1C13 (hợp lệ theo ETG.1000), và — trường hợp thật sự quan trọng — loại
            // như R1EC có ESI khai PdoUpload="false": đối tượng mapping theo slot (0x1A00.., 0x1600..)
            // không đọc được qua SDO dù phép gán thì đọc/ghi được. Khi đó chỉ số kênh đáng tin duy
            // nhất là cỡ SyncManager mà chính master vừa cấu hình.
            //
            // Phải bỏ qua mọi thứ bộ ghép map trước các kênh (từ 0xF100 Device status mà ta cố ý gán
            // đầu tiên), nếu không gốc DI rơi vào byte chẩn đoán và mọi kênh đọc ra 0.
            int inBytes = context->slavelist[slave].Ibytes;
            int outBytes = context->slavelist[slave].Obytes;

            if (l.inputBitCount == 0 && inBytes > 0)
            {
                int skipBytes = (l.deviceStatusByteOffset >= 0) ? l.deviceStatusByteOffset + DEVICE_STATUS_BYTES : 0;
                if (skipBytes < inBytes)
                {
                    l.inputByteOffset = skipBytes;
                    l.inputBitOffset = 0;
                    l.inputBitCount = (inBytes - skipBytes) * 8;
                }
            }
            if (l.outputBitCount == 0 && outBytes > 0)
            {
                l.outputByteOffset = 0;
                l.outputBitOffset = 0;
                l.outputBitCount = outBytes * 8;
            }

            if (l.inputBitCount <= 0 && l.outputBitCount <= 0)
            {
                throw NotSupported(std::format(
                    "Slave {}: no digital I/O found - neither the 0x1C12/0x1C13 PDO assignment SDO "
                    "nor SOEM's own EEPROM-mapped byte counts (Ibytes={}, Obytes={}) report any "
                    "channel. Check the slave actually supports CoE PDO assignment.",
                    slave, inBytes, outBytes));
            }

            ioLayoutDiag += std::format(
                " | Slave {}: PDO-assign {} Tx/{} Rx entr(ies); SM sizes Ibytes={} Obytes={} -> "
                "inputs byte={} count={}, outputs byte={} count={}, deviceStatusByte={}",
                slave, txCount, rxCount, inBytes, outBytes,
                l.inputByteOffset, l.inputBitCount, l.outputByteOffset, l.outputBitCount, l.deviceStatusByteOffset);

            // GetDigitalInputs/SetDigitalOutputs gói vào 64 bit đọc qua cửa sổ 8 byte bắt đầu từ
            // inputByteOffset/outputByteOffset — cửa sổ đó chỉ phủ 64 bit khi trường bắt đầu thẳng
            // hàng byte. Chặn bitOffset+bitCount ở đây để module bắt đầu giữa byte VÀ dài đủ 64 bit
            // hỏng to lúc dò, thay vì âm thầm mất các bit cao ở mỗi lần đọc.
            if (l.inputBitOffset + l.inputBitCount > 64)
                throw NotSupported(std::format(
                    "Slave {}: {} digital input bits mapped starting at bit offset {} - exceeds the "
                    "64-bit window GetDigitalInputs can carry.", slave, l.inputBitCount, l.inputBitOffset));
            if (l.outputBitOffset + l.outputBitCount > 64)
                throw NotSupported(std::format(
                    "Slave {}: {} digital output bits mapped starting at bit offset {} - exceeds the "
                    "64-bit window SetDigitalOutputs can carry.", slave, l.outputBitCount, l.outputBitOffset));

            return l;
        }

        /// <summary>
        /// Hoàn tất bring-up mọi bộ ghép modular mà hook PRE-OP đã cấu hình, SAU khi IOmap đã có: lập
        /// trình độ dài SyncManager của từng bộ theo độ rộng module từ ESI.
        ///
        /// <para>SOEM định cỡ process image của slave bằng cách cộng độ dài bit trong đối tượng
        /// mapping của từng PDO đã gán, mà bộ ghép này không phục vụ đối tượng mapping nào cho các
        /// slot. Mọi module vì thế đo ra 0 bit, SM3/SM2 được lập trình quá ngắn, và bộ ghép trả lời
        /// bằng AL status 0x1E "Invalid input configuration" — không vào được SAFE-OP cho tới khi độ
        /// dài khớp thứ nó thật sự tạo ra.</para>
        ///
        /// <para>Phải chạy ngay sau ecx_config_map_group và TRƯỚC mọi yêu cầu SAFE-OP/OPERATIONAL. Để
        /// lại số bit theo slot trong ioLayout, ConnectAllAxis báo cáo từ đó.</para>
        /// </summary>
        void ApplyModularCouplerIo()
        {
            for (int s = 1; s <= context->slavecount && s <= MAX_SLAVES; s++)
            {
                if (s_couplerInBytes[s] <= 0 && s_couplerOutBytes[s] <= 0)
                    continue; // không phải bộ ghép mà hook đã cấu hình

                int statusBytes = s_couplerStatusBytes[s];
                int inTotal = statusBytes + s_couplerInBytes[s];
                int outTotal = s_couplerOutBytes[s];
                uint16 configadr = context->slavelist[s].configadr;

                // Trước khi đụng gì: SOEM nghĩ các SM thế nào, và ESC thật sự đang giữ gì? Hai thứ này
                // lệch nhau chính là lý do đường này tế nhị.
                for (int n = 0; n < 4; n++)
                {
                    ec_smt esc;
                    memset(&esc, 0, sizeof(esc));
                    int rw = ecx_FPRD(&context->port, configadr, (uint16)(ECT_REG_SM0 + n * (int)sizeof(ec_smt)),
                                      sizeof(esc), &esc, EC_TIMEOUTRET3);
                    moduleIdentDiag += std::format(
                        " | SM{} soem@0x{:04X} len={} fl=0x{:08X} type={} :: esc@0x{:04X} len={} fl=0x{:08X} (wkc={})",
                        n,
                        (unsigned)etohs(context->slavelist[s].SM[n].StartAddr), (unsigned)etohs(context->slavelist[s].SM[n].SMlength),
                        (unsigned)etohl(context->slavelist[s].SM[n].SMflags), (int)context->slavelist[s].SMtype[n],
                        (unsigned)etohs(esc.StartAddr), (unsigned)etohs(esc.SMlength), (unsigned)etohl(esc.SMflags), rw);
                }

                // Lấy ảnh SM của chính SOEM chứ không đọc lại ESC: đó là địa chỉ bắt đầu và cờ lấy từ
                // SII mà ecx_map_sm vừa lập trình, đã đúng thứ tự byte trên dây, chỉ SMlength là sai.
                // (Đọc lại bằng FPRD ở đây từng trả toàn 0, rồi bị ghi đè lên một cấu hình SM đang
                // chạy tốt — đừng đưa lại lỗi đó.)
                ec_smt sm2 = context->slavelist[s].SM[2];
                ec_smt sm3 = context->slavelist[s].SM[3];

                uint16 inPhys = etohs(sm3.StartAddr);
                uint16 outPhys = etohs(sm2.StartAddr);
                sm3.SMlength = htoes((uint16)inTotal);
                sm2.SMlength = htoes((uint16)outTotal);

                // ecx_map_sm xoá bit Activate của mọi SM nó đo ra độ dài 0, nên SM2 tới đây đang tắt
                // và chỉ ghi độ dài thì nó vẫn tắt — slave từ chối đổi trạng thái với AL status 0x1D
                // "Invalid output configuration". Lặp lại đúng luật của ecx_map_sm với độ dài thật.
                sm3.SMflags = htoel(inTotal > 0 ? (etohl(sm3.SMflags) | ~EC_SMENABLEMASK)
                                                : (etohl(sm3.SMflags) & EC_SMENABLEMASK));
                sm2.SMflags = htoel(outTotal > 0 ? (etohl(sm2.SMflags) | ~EC_SMENABLEMASK)
                                                 : (etohl(sm2.SMflags) & EC_SMENABLEMASK));
                int w3 = ecx_FPWR(&context->port, configadr, (uint16)(ECT_REG_SM0 + 3 * (int)sizeof(ec_smt)), sizeof(sm3), &sm3, EC_TIMEOUTRET3);
                int w2 = outTotal > 0
                    ? ecx_FPWR(&context->port, configadr, (uint16)(ECT_REG_SM0 + 2 * (int)sizeof(ec_smt)), sizeof(sm2), &sm2, EC_TIMEOUTRET3)
                    : 0;
                context->slavelist[s].SM[3] = sm3;
                if (outTotal > 0) context->slavelist[s].SM[2] = sm2;

                // Đọc lại thanh ghi: wkc của lệnh ghi chỉ nói khung đã được trả lời, không nói ESC giữ
                // giá trị — mà bộ ghép âm thầm tự đổi cỡ SM chính là lỗi cả đường này sinh ra để xử lý.
                ec_smt back2, back3;
                memset(&back2, 0, sizeof(back2));
                memset(&back3, 0, sizeof(back3));
                int r2 = ecx_FPRD(&context->port, configadr, (uint16)(ECT_REG_SM0 + 2 * (int)sizeof(ec_smt)), sizeof(back2), &back2, EC_TIMEOUTRET3);
                int r3 = ecx_FPRD(&context->port, configadr, (uint16)(ECT_REG_SM0 + 3 * (int)sizeof(ec_smt)), sizeof(back3), &back3, EC_TIMEOUTRET3);

                // SyncManager mà slave GIỮ LẠI là giới hạn trung thực duy nhất: bộ ghép này từ chối
                // SM3 dài hơn khi firmware chưa dựng image module, và đọc quá cuối một SM trả về DPRAM
                // chưa map, trông y như dữ liệu ngõ vào sống đang nhấp nháy.
                int effIn = (r3 > 0 && etohs(back3.SMlength) > 0) ? etohs(back3.SMlength) : 0;
                int effOut = (r2 > 0 && etohs(back2.SMlength) > 0) ? etohs(back2.SMlength) : 0;
                if (effIn < inTotal || effOut < outTotal)
                    moduleIdentDiag += std::format(
                        " | Slave {}: coupler kept SM3={}/{} and SM2={}/{} bytes - module data is NOT in its"
                        " process image (a coupler that has already been configured since power-up does this;"
                        " power-cycle it and connect once)",
                        s, effIn, inTotal, effOut, outTotal);

                int inChannelBytes = effIn > statusBytes ? effIn - statusBytes : 0;
                int outChannelBytes = effOut;

                s_coupler[s].configadr = configadr;
                s_coupler[s].active = true;

                // Offset tính từ đầu vùng ngõ vào/ngõ ra của chính bộ ghép.
                ioLayout[s].deviceStatusByteOffset = 0;
                ioLayout[s].inputByteOffset = statusBytes;
                ioLayout[s].inputBitOffset = 0;
                ioLayout[s].inputBitCount = inChannelBytes * 8;
                ioLayout[s].outputByteOffset = 0;
                ioLayout[s].outputBitOffset = 0;
                ioLayout[s].outputBitCount = outChannelBytes * 8;

                // Xếp register riêng của từng slot nối nhau, theo đúng thứ tự slot mà hook đã gán —
                // ngõ vào bắt đầu sau từ trạng thái của bộ ghép, ngõ ra bắt đầu từ 0.
                int runningIn = statusBytes;
                int runningOut = 0;
                for (int slot = 0; slot < s_hookIdentCount[s]; slot++)
                {
                    s_slotInByteOffset[s][slot] = runningIn;
                    runningIn += (s_hookSlotInBits[s][slot] + 7) / 8;
                    s_slotOutByteOffset[s][slot] = runningOut;
                    runningOut += (s_hookSlotOutBits[s][slot] + 7) / 8;
                }

                moduleIdentDiag += std::format(
                    " | Slave {}: direct I/O - SM3@0x{:04X} len={} (wkc={}), SM2@0x{:04X} len={} (wkc={}), {} DI / {} DO"
                    " | readback SM2@0x{:04X} len={} flags=0x{:08X} (wkc={}), SM3@0x{:04X} len={} flags=0x{:08X} (wkc={})",
                    s, (unsigned)inPhys, inTotal, w3, (unsigned)outPhys, outTotal, w2,
                    ioLayout[s].inputBitCount, ioLayout[s].outputBitCount,
                    (unsigned)etohs(back2.StartAddr), (unsigned)etohs(back2.SMlength), (unsigned)etohl(back2.SMflags), r2,
                    (unsigned)etohs(back3.StartAddr), (unsigned)etohs(back3.SMlength), (unsigned)etohl(back3.SMflags), r3);
            }
        }
    };

    // ══════════════════════════════════════════════════════════════
    //  Vòng đời
    // ══════════════════════════════════════════════════════════════

    SoemEtherCatBus::SoemEtherCatBus() : m(std::make_unique<Impl>())
    {
        m->owner = this;
    }

    SoemEtherCatBus::~SoemEtherCatBus()
    {
        try { Close(); } catch (...) {}
    }

    bool SoemEtherCatBus::IsNpcapAvailable(std::string* error)
    {
        std::call_once(g_wpcapOnce, [] {
            // Npcap cài ở chế độ thường đặt wpcap.dll + Packet.dll trong System32\Npcap, KHÔNG nằm
            // trên đường tìm DLL mặc định. Nạp bằng đường dẫn đầy đủ (kèm thư mục của nó để Packet.dll
            // đi theo); sau đó cơ chế nạp trễ của exe tìm thấy module đã nạp theo tên.
            wchar_t sys[MAX_PATH]{};
            if (GetSystemDirectoryW(sys, MAX_PATH) > 0)
            {
                std::wstring path = std::wstring(sys) + L"\\Npcap\\wpcap.dll";
                g_wpcap = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            }
            if (g_wpcap == nullptr) g_wpcap = LoadLibraryW(L"wpcap.dll");   // Npcap chế độ tương thích WinPcap
        });

        if (g_wpcap != nullptr) return true;
        if (error != nullptr)
            *error = "Npcap not installed (cannot load wpcap.dll) — install Npcap, then restart app";
        return false;
    }

    std::vector<NetworkAdapterInfo> SoemEtherCatBus::ListAdapters()
    {
        std::vector<NetworkAdapterInfo> list;

        ULONG size = 16 * 1024;
        std::vector<uint8_t> buffer;
        ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_UNICAST;
        DWORD rc = ERROR_BUFFER_OVERFLOW;
        for (int attempt = 0; attempt < 4 && rc == ERROR_BUFFER_OVERFLOW; ++attempt)
        {
            buffer.resize(size);
            rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
        }
        if (rc != NO_ERROR) return list;

        for (auto* a = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); a != nullptr; a = a->Next)
        {
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            NetworkAdapterInfo info;
            // Npcap đặt tên thiết bị theo đúng GUID của card: \Device\NPF_{GUID}.
            info.DeviceName = std::string("\\Device\\NPF_") + (a->AdapterName != nullptr ? a->AdapterName : "");
            info.FriendlyName = a->FriendlyName != nullptr ? base::Narrow(a->FriendlyName) : std::string();
            info.Description = a->Description != nullptr ? base::Narrow(a->Description) : std::string();
            list.push_back(std::move(info));
        }
        return list;
    }

    void SoemEtherCatBus::SetRealtimeOptions(const rt::RtOptions& options)
    {
        std::lock_guard<std::mutex> lock(m->optionsMutex);
        m->rtOptions = options;
    }

    rt::RtOptions SoemEtherCatBus::RealtimeOptions() const
    {
        std::lock_guard<std::mutex> lock(m->optionsMutex);
        return m->rtOptions;
    }

    rt::RtStats SoemEtherCatBus::GetRtStats() const
    {
        return m->rtThread.Stats();
    }

    std::string SoemEtherCatBus::RealtimeInfo() const
    {
        std::lock_guard<std::mutex> lock(m->optionsMutex);
        return m->rtInfo;
    }

    std::string SoemEtherCatBus::LastOpenError() const
    {
        return m->lastOpenError;
    }

    // ══════════════════════════════════════════════════════════════
    //  Open / Close / CyclicExchange
    // ══════════════════════════════════════════════════════════════

    bool SoemEtherCatBus::Open(const std::string& networkAdapter, int /*expectedSlaveCount*/)
    {
        Close();
        m->Allocate();

        bool ok = false;
        try
        {
            if (m->OpenSocket(networkAdapter) && ecx_config_init(m->context) > 0)
            {
                // ecx_config_map_group() dựng IOmap và đưa slave lên SAFE-OP (xem ec_config.c — nó xin
                // SAFE-OP, không phải PRE-OP). Nó cũng là thứ dựng mailbox CoE của SOEM 2.x, nên
                // trước lời gọi này chưa có SDO.
                ecx_config_map_group(m->context, m->ioMap, 0);

                // Đường chỉ-có-trục này không cấu hình bộ ghép I/O modular (xem hook của
                // ConnectAllAxis) — chỉ dùng với bus toàn trục CiA402.

                // Dò layout PDO sống của từng slave. Ném (DiscoverPdoLayout) nếu driver không map
                // thứ ta cần, thay vì âm thầm dùng offset sai.
                for (int i = 1; i <= m->context->slavecount && i <= MAX_SLAVES; i++)
                    m->Layout((uint16)i, 0) = m->DiscoverPdoLayout((uint16)i);

                ok = m->BringToOperational(nullptr, nullptr);
            }
        }
        catch (...)
        {
            Close();
            throw;
        }

        if (ok)
            m->StartCyclic();
        else
            Close();

        return ok;
    }

    // ══════════════════════════════════════════════════════════════
    //  ConnectAllAxis — bring-up "Connect All Axis" cho trang cấu hình. Cố ý KHÔNG dừng ở slave hỏng
    //  đầu tiên như Open(): gom một AxisConnectResult cho mỗi trục đã cấu hình để UI hiện ✅/⚠ cho
    //  mọi slot sau một lần bấm. Cũng xác nhận các module I/O số trên CÙNG lần bring-up (qua
    //  DiscoverIoLayout) — trục và module I/O chung một chuỗi nên phải lên trong một trình tự
    //  ecx_init/config_init/OPERATIONAL duy nhất, chỉ kiểm khác nhau và trả về hai danh sách riêng.
    // ══════════════════════════════════════════════════════════════

    std::vector<AxisConnectResult> SoemEtherCatBus::ConnectAllAxis(const std::string& networkAdapter,
                                                                   const std::vector<AxisConfig>& axes,
                                                                   const std::vector<IoModuleConfig>& ioModules,
                                                                   std::vector<AxisConnectResult>& ioResults)
    {
        Close();

        std::vector<AxisConnectResult> results;
        ioResults.clear();
        m->Allocate();
        ecx_contextt* context = m->context;

        bool busReady = false;
        if (m->OpenSocket(networkAdapter))
        {
            // ecx_config_init() quét slave qua EEPROM/SII (chạy được ở INIT) và điền
            // eep_man/eep_id/eep_rev cho mọi slave tìm thấy — đó là định danh cần bên dưới, không tốn
            // thêm vòng SDO nào.
            if (ecx_config_init(context) > 0)
            {
                // Đăng ký hook PRE-OP → SAFE-OP trước khi map: đó là điểm duy nhất cấu hình được bộ
                // ghép modular kịp cho IOmap mà ecx_config_map_group sắp dựng.
                s_hookDiagLen = 0;
                s_hookDiag[0] = '\0';
                for (int i = 1; i <= context->slavecount && i <= MAX_SLAVES; i++)
                {
                    s_hookIdentCount[i] = 0;
                    s_couplerInBytes[i] = 0;
                    s_couplerOutBytes[i] = 0;
                    s_couplerStatusBytes[i] = 0;
                    ecx_userExtraIbits[i] = 0;
                    ecx_userExtraObits[i] = 0;
                    context->slavelist[i].PO2SOconfig = PreOpToSafeOpConfig;
                }

                // Dựng IOmap và đưa slave lên SAFE-OP, gọi hook ở trên cho từng slave dọc đường.
                ecx_config_map_group(context, m->ioMap, 0);

                m->moduleIdentDiag += s_hookDiag;
                for (int i = 1; i <= context->slavecount && i <= MAX_SLAVES; i++)
                {
                    if (s_hookIdentCount[i] <= 0) continue;
                    m->detectedModules[i].assign(s_hookIdents[i], s_hookIdents[i] + s_hookIdentCount[i]);
                }

                // Sửa độ dài SyncManager của mọi bộ ghép modular NGAY, khi các slave còn nằm ở chỗ
                // ecx_config_map_group để lại. SOEM vừa lập trình SM3/SM2 từ một phép đo chắc chắn ra
                // 0 cho các module, và bộ ghép trả lời bằng AL status 0x1E — nó không vào SAFE-OP cho
                // tới khi độ dài khớp. Không thể chờ tới đoạn OPERATIONAL bên dưới.
                m->ApplyModularCouplerIo();

                // ecx_config_map_group đã thử SAFE-OP và nhận lỗi đó, nên bộ ghép đang đậu ở PRE-OP
                // với lỗi bị chốt. Xoá lỗi (EC_STATE_ACK) và xin lại khi SM đã đúng; đoạn cuối bên
                // dưới sẽ thấy nó ở SAFE-OP như mọi slave khác.
                s_hookDiagLen = 0; // đã chép vào moduleIdentDiag ở trên; dùng lại bộ đệm
                s_hookDiag[0] = 0;
                for (int i = 1; i <= context->slavecount && i <= MAX_SLAVES; i++)
                {
                    if (!s_coupler[i].active) continue;
                    context->slavelist[i].state = EC_STATE_SAFE_OP + EC_STATE_ACK;
                    ecx_writestate(context, (uint16)i);
                    ecx_statecheck(context, (uint16)i, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
                    HookLog(" | Slave %d: after SM fix state=0x%02X AL=0x%04X",
                            i, context->slavelist[i].state, context->slavelist[i].ALstatuscode);
                }
                m->moduleIdentDiag += s_hookDiag;

                busReady = true;
            }
        }

        const std::string notReady = m->lastOpenError.empty()
            ? std::string("EtherCAT bus did not reach PRE-OP (check adapter name / cabling / power)")
            : "EtherCAT bus did not reach PRE-OP: " + m->lastOpenError;

        bool allOk = busReady;
        for (const AxisConfig& axis : axes)
        {
            AxisConnectResult result;
            result.SlavePosition = axis.SlavePosition;
            result.AxisChannel = axis.AxisChannel;
            result.AxisName = axis.Name;

            int idx = axis.SlavePosition + 1;
            if (!busReady || idx < 1 || idx > context->slavecount)
            {
                result.Status = AxisConnectStatus::NoResponse;
                result.Message = busReady
                    ? std::format("No slave found at position {} ({} slave(s) found on bus)", axis.SlavePosition, context->slavecount)
                    : notReady;
                allOk = false;
                results.push_back(result);
                continue;
            }

            uint16 slave = (uint16)idx;
            result.ActualVendorId = context->slavelist[slave].eep_man;
            result.ActualProductCode = context->slavelist[slave].eep_id;
            result.ActualRevision = context->slavelist[slave].eep_rev;

            if (result.ActualVendorId != axis.ExpectedVendorId || result.ActualProductCode != axis.ExpectedProductCode)
            {
                result.Status = AxisConnectStatus::WrongDriver;
                result.Message = std::format("Expected Vendor 0x{:X} Product 0x{:X}, found Vendor 0x{:X} Product 0x{:X}",
                    axis.ExpectedVendorId, axis.ExpectedProductCode, result.ActualVendorId, result.ActualProductCode);
                allOk = false;
                results.push_back(result);
                continue;
            }

            try
            {
                if (axis.AxisChannel < 0 || axis.AxisChannel >= AxisAddress::MaxChannels)
                    throw NotSupported(std::format("Slave {}: axis channel {} out of range 0..{}", slave, axis.AxisChannel,
                                                   AxisAddress::MaxChannels - 1));
                m->Layout(slave, axis.AxisChannel) = m->DiscoverPdoLayout(slave, axis.AxisChannel);
                result.Status = AxisConnectStatus::Ok;
                result.Message = "OK";
            }
            catch (const NotSupported& ex)
            {
                result.Status = AxisConnectStatus::PdoUnsupported;
                result.Message = ex.what();
                allOk = false;
            }
            results.push_back(result);
        }

        for (const IoModuleConfig& io : ioModules)
        {
            AxisConnectResult result;
            result.SlavePosition = io.SlavePosition;
            result.AxisName = io.Name;

            int idx = io.SlavePosition + 1;
            if (!busReady || idx < 1 || idx > context->slavecount)
            {
                result.Status = AxisConnectStatus::NoResponse;
                result.Message = busReady
                    ? std::format("No slave found at position {} ({} slave(s) found on bus)", io.SlavePosition, context->slavecount)
                    : notReady;
                allOk = false;
                ioResults.push_back(result);
                continue;
            }

            uint16 slave = (uint16)idx;
            result.ActualVendorId = context->slavelist[slave].eep_man;
            result.ActualProductCode = context->slavelist[slave].eep_id;
            result.ActualRevision = context->slavelist[slave].eep_rev;

            if (result.ActualVendorId != io.ExpectedVendorId || result.ActualProductCode != io.ExpectedProductCode)
            {
                result.Status = AxisConnectStatus::WrongDriver;
                result.Message = std::format("Expected Vendor 0x{:X} Product 0x{:X}, found Vendor 0x{:X} Product 0x{:X}",
                    io.ExpectedVendorId, io.ExpectedProductCode, result.ActualVendorId, result.ActualProductCode);
                allOk = false;
                ioResults.push_back(result);
                continue;
            }

            // Bộ ghép modular không đo được theo cách thường và không được xét theo cách đó. Đối tượng
            // mapping theo slot của nó không bao giờ trả lời qua CoE (ESI PdoUpload="false"), nên
            // ReadPdoAssignEntries cộng mọi module ra 0 bit và DiscoverIoLayout sẽ loại một thiết bị
            // thật ra đang chạy. Khi hook PRE-OP đã định cỡ slave này từ ESI thì lấy đó làm đáp án.
            // Phải giữ allOk đúng, nếu không đoạn cuối không bao giờ chạy.
            if (s_coupler[slave].active)
            {
                result.InputBitCount = m->ioLayout[slave].inputBitCount;
                result.OutputBitCount = m->ioLayout[slave].outputBitCount;
                bool haveChannels = result.InputBitCount > 0 || result.OutputBitCount > 0;
                result.Status = haveChannels ? AxisConnectStatus::Ok : AxisConnectStatus::PdoUnsupported;
                result.Message = std::string(haveChannels
                    ? "OK (modular coupler, direct SM access)"
                    : "Modular coupler configured, but it exposed no module channels") + m->moduleIdentDiag;
                if (!haveChannels) allOk = false;
                ioResults.push_back(result);
                continue;
            }

            // DiscoverIoLayout ném NotSupported cho module thật sự không map gì hoặc nhiều bit hơn
            // GetDigitalInputs/SetDigitalOutputs mang được — bắt ở đây để một slot I/O hỏng vẫn hiện
            // thành ⚠ PdoUnsupported thay vì huỷ cả lần "Connect All Axis".
            try
            {
                IoLayout layout = m->DiscoverIoLayout(slave);
                if (layout.inputBitCount <= 0 && layout.outputBitCount <= 0)
                {
                    result.Status = AxisConnectStatus::PdoUnsupported;
                    result.Message = std::format("Slave {}: no digital input/output bits found in its default PDO{}{}",
                        io.SlavePosition, m->moduleIdentDiag, m->ioLayoutDiag);
                    allOk = false;
                }
                else
                {
                    m->ioLayout[slave] = layout;
                    result.Status = AxisConnectStatus::Ok;
                    result.Message = "OK";
                    result.InputBitCount = layout.inputBitCount;
                    result.OutputBitCount = layout.outputBitCount;
                }
            }
            catch (const NotSupported& ex)
            {
                result.Status = AxisConnectStatus::PdoUnsupported;
                result.Message = ex.what() + m->moduleIdentDiag;
                allOk = false;
            }
            ioResults.push_back(result);
        }

        if (!allOk)
        {
            Close();
            return results;
        }

        int settleWkc = 0, opWkc = 0;
        if (m->BringToOperational(&settleWkc, &opWkc))
        {
            // Đọc lại SyncManager của mọi bộ ghép khi nó đã thật sự chạy. Ở PRE-OP thiết bị này giữ
            // SM3 ở độ dài firmware của nó quyết định; nếu image ngõ vào có lớn lên thì OPERATIONAL
            // là lúc thấy được.
            for (int i = 1; i <= context->slavecount && i <= MAX_SLAVES; i++)
            {
                if (!s_coupler[i].active) continue;
                ec_smt now2, now3;
                memset(&now2, 0, sizeof(now2));
                memset(&now3, 0, sizeof(now3));
                uint16 adr = s_coupler[i].configadr;
                int q2 = ecx_FPRD(&context->port, adr, (uint16)(ECT_REG_SM0 + 2 * (int)sizeof(ec_smt)), sizeof(now2), &now2, EC_TIMEOUTRET3);
                int q3 = ecx_FPRD(&context->port, adr, (uint16)(ECT_REG_SM0 + 3 * (int)sizeof(ec_smt)), sizeof(now3), &now3, EC_TIMEOUTRET3);
                std::string opDiag = std::format(
                    " | Slave {} in OP: SM2 len={} fl=0x{:08X} (wkc={}), SM3 len={} fl=0x{:08X} (wkc={})",
                    i, (unsigned)etohs(now2.SMlength), (unsigned)etohl(now2.SMflags), q2,
                    (unsigned)etohs(now3.SMlength), (unsigned)etohl(now3.SMflags), q3);
                m->moduleIdentDiag += opDiag;
                // Kết quả theo slot được dựng trước khi bus tới OP, nên nối cả vào đó, nếu không
                // thông tin này không tới được bên gọi.
                for (AxisConnectResult& r : ioResults)
                    if (r.SlavePosition == i - 1) r.Message += opDiag;
            }

            m->StartCyclic();
            return results;
        }

        // Mọi slot đều đạt nhưng cả bus không tới OPERATIONAL — báo lên mọi kết quả Ok thay vì âm
        // thầm trả về một bus "đã kết nối" mà không trao đổi process data. Nối AL status của mọi slave
        // chưa OP và danh sách lỗi SOEM tích luỹ, để lỗi bring-up trên phần cứng không đọc ra thành
        // một lần hết giờ trơ trụi.
        std::string diag;
        ecx_readstate(context); // làm mới state/ALstatuscode của mọi slave trước khi báo
        for (int i = 1; i <= context->slavecount && i <= MAX_SLAVES; i++)
        {
            if (context->slavelist[i].state != EC_STATE_OPERATIONAL)
            {
                char* alMsg = ec_ALstatuscode2string(context->slavelist[i].ALstatuscode);
                diag += std::format(" | Slave {} state=0x{:02X} ALstatus=0x{:04X} ({}) Alias=0x{:04X} hasdc={}",
                    i, (int)context->slavelist[i].state, (unsigned)context->slavelist[i].ALstatuscode,
                    alMsg != nullptr ? alMsg : "", (unsigned)context->slavelist[i].aliasadr,
                    context->slavelist[i].hasdc != 0 ? "True" : "False");
            }
        }
        while (ecx_iserror(context))
        {
            char* errMsg = ecx_elist2string(context);
            diag += std::string(" | ") + (errMsg != nullptr ? errMsg : "");
        }
        diag += std::format(" | group0: Obytes={} outputsWKC(expected)={} Ibytes={} inputsWKC(expected)={} lastSettleWKC={} lastOpWKC={}",
            context->grouplist[0].Obytes, (unsigned)context->grouplist[0].outputsWKC,
            context->grouplist[0].Ibytes, (unsigned)context->grouplist[0].inputsWKC,
            settleWkc, opWkc);
        diag += m->moduleIdentDiag;

        for (AxisConnectResult& r : results)
        {
            if (r.Status == AxisConnectStatus::Ok)
            {
                r.Status = AxisConnectStatus::PdoUnsupported;
                r.Message = "All slots matched, but the bus did not reach OPERATIONAL state" + diag;
            }
        }
        for (AxisConnectResult& r : ioResults)
        {
            if (r.Status == AxisConnectStatus::Ok)
            {
                r.Status = AxisConnectStatus::PdoUnsupported;
                r.Message = "All slots matched, but the bus did not reach OPERATIONAL state" + diag;
            }
        }
        Close();
        return results;
    }

    // ══════════════════════════════════════════════════════════════
    //  ScanSlaves — chỉ dò, cho nút "Scan". Định danh (eep_man/eep_id/eep_rev) chỉ cần
    //  ecx_config_init. Độ phân giải encoder (0x608F) thì cần mailbox, nên cũng lên PRE-OP qua
    //  ecx_config_map_group — được thì tốt: slave không tới PRE-OP hoặc không khai 0x608F thì các
    //  lần đọc SDO chỉ hỏng (wkc ≤ 0), trường đó để 0, định danh vẫn trả về. Không bao giờ để bus mở.
    // ══════════════════════════════════════════════════════════════

    std::vector<DiscoveredSlaveInfo> SoemEtherCatBus::ScanSlaves(const std::string& networkAdapter)
    {
        Close();

        std::vector<DiscoveredSlaveInfo> results;
        m->Allocate();
        ecx_contextt* context = m->context;

        if (m->OpenSocket(networkAdapter) && ecx_config_init(context) > 0)
        {
            ecx_config_map_group(context, m->ioMap, 0);

            for (int i = 1; i <= context->slavecount && i <= MAX_SLAVES; i++)
            {
                DiscoveredSlaveInfo info;
                info.SlavePosition = i - 1;
                info.VendorId = context->slavelist[i].eep_man;
                info.ProductCode = context->slavelist[i].eep_id;
                info.RevisionNo = context->slavelist[i].eep_rev;

                uint32 encInc = 0, motorRevs = 0;
                int rdl = sizeof(encInc);
                if (ecx_SDOread(context, (uint16)i, IDX_ENCODER_RESOLUTION, 1, FALSE, &rdl, &encInc, EC_TIMEOUTRXM) > 0)
                    info.EncoderIncrements = encInc;
                rdl = sizeof(motorRevs);
                if (ecx_SDOread(context, (uint16)i, IDX_ENCODER_RESOLUTION, 2, FALSE, &rdl, &motorRevs, EC_TIMEOUTRXM) > 0)
                    info.EncoderMotorRevolutions = motorRevs;

                results.push_back(info);
            }
        }

        // Giữ lý do không mở được card qua Close() để bên gọi còn đọc được.
        std::string openError = m->lastOpenError;
        Close();
        m->lastOpenError = openError;
        return results;
    }

    void SoemEtherCatBus::Close()
    {
        // Phải dừng luồng chu kỳ TRƯỚC khi huỷ socket/context nó đang dùng.
        m->StopCyclic();

        if (m->isOpen && m->context != nullptr)
        {
            m->context->slavelist[0].state = EC_STATE_INIT;
            ecx_writestate(m->context, 0);
            ecx_close(m->context);
        }
        m->FreeNative();
    }

    int SoemEtherCatBus::SlaveCount() const
    {
        return (m->context != nullptr && m->isOpen) ? m->context->slavecount : 0;
    }

    /// <summary>
    /// Cố ý không làm gì: khung PDO do luồng chu kỳ thời gian thực phát/nhận liên tục (khởi động khi
    /// Open()/ConnectAllAxis() tới OPERATIONAL). Giữ lại vì IEtherCatBus khai báo nó (bus mô phỏng
    /// cần được bước tay) và luồng quét của EtherCatMasterService vẫn gọi mỗi vòng. Vẫn kiểm bus đang
    /// mở, để bên gọi dựa vào "ném nếu chưa mở" không âm thầm nhận một hợp đồng khác.
    /// </summary>
    void SoemEtherCatBus::CyclicExchange()
    {
        m->EnsureOpen();
    }

    // ══════════════════════════════════════════════════════════════
    //  SDO (không theo chu kỳ) — Modes of Operation, Profile Vel/Accel/Decel, Actual Vel/Torque, và
    //  WriteSdo/ReadSdo chung. Không nằm trong PDO mặc định của các driver đã thử nên đều đi SDO.
    //  Phần lớn chỉ gọi một lần mỗi lệnh chạy/servo-on/về gốc, nên 700 ms tệ nhất của EC_TIMEOUTRXM
    //  chấp nhận được. Luồng gọi SDO bị chặn, nhưng vòng PDO thì KHÔNG — nó chạy trên luồng riêng.
    // ══════════════════════════════════════════════════════════════

    void SoemEtherCatBus::WriteSdo(int slavePosition, uint16_t index, uint8_t subIndex, int value, uint8_t sizeBytes)
    {
        MailboxPriorityScope priority;
        uint16 slave = m->ResolveSlave(slavePosition);
        index = AxisAddress::ProfileIndex(slavePosition, index);
        switch (sizeBytes)
        {
            case 1: { int8 v = (int8)value; ecx_SDOwrite(m->context, slave, index, subIndex, FALSE, 1, &v, EC_TIMEOUTRXM); break; }
            case 2: { int16 v = (int16)value; ecx_SDOwrite(m->context, slave, index, subIndex, FALSE, 2, &v, EC_TIMEOUTRXM); break; }
            default: { int32 v = (int32)value; ecx_SDOwrite(m->context, slave, index, subIndex, FALSE, 4, &v, EC_TIMEOUTRXM); break; }
        }
    }

    int SoemEtherCatBus::ReadSdo(int slavePosition, uint16_t index, uint8_t subIndex, uint8_t sizeBytes)
    {
        int value = 0;
        TryReadSdo(slavePosition, index, subIndex, sizeBytes, value);
        return value;
    }

    bool SoemEtherCatBus::TryReadSdo(int slavePosition, uint16_t index, uint8_t subIndex, uint8_t sizeBytes, int& value)
    {
        MailboxPriorityScope priority;
        uint16 slave = m->ResolveSlave(slavePosition);
        index = AxisAddress::ProfileIndex(slavePosition, index);
        int wkc = 0;
        switch (sizeBytes)
        {
            case 1: { int8 v = 0; int s = 1; wkc = ecx_SDOread(m->context, slave, index, subIndex, FALSE, &s, &v, EC_TIMEOUTRXM); value = v; break; }
            case 2: { int16 v = 0; int s = 2; wkc = ecx_SDOread(m->context, slave, index, subIndex, FALSE, &s, &v, EC_TIMEOUTRXM); value = v; break; }
            default: { int32 v = 0; int s = 4; wkc = ecx_SDOread(m->context, slave, index, subIndex, FALSE, &s, &v, EC_TIMEOUTRXM); value = v; break; }
        }
        return wkc > 0;
    }

    void SoemEtherCatBus::SetModeOfOperation(int slavePosition, int8_t mode)
    {
        uint16 slave = 0;
        const PdoLayout& l = m->AxisLayout(slavePosition, slave);
        if (l.outModeOfOperation >= 0)
        {
            // Driver map 0x6060 vào RxPdo (Welcon WE2x): giá trị PDO thắng — ghi SDO sẽ bị đè ở chu kỳ sau.
            m->image.Output<int8_t>(m->OutBase(slave) + l.outModeOfOperation, mode);
            return;
        }
        MailboxPriorityScope priority;
        int8 v = (int8)mode;
        ecx_SDOwrite(m->context, slave, AxisAddress::ProfileIndex(slavePosition, IDX_MODES_OF_OPERATION), 0, FALSE, 1, &v, EC_TIMEOUTRXM);
    }

    void SoemEtherCatBus::SetProfileVelocity(int slavePosition, uint32_t countsPerSec)
    {
        MailboxPriorityScope priority;
        uint16 slave = m->ResolveSlave(slavePosition);
        uint32 v = (uint32)countsPerSec;
        ecx_SDOwrite(m->context, slave, AxisAddress::ProfileIndex(slavePosition, IDX_PROFILE_VELOCITY), 0, FALSE, 4, &v, EC_TIMEOUTRXM);
    }

    void SoemEtherCatBus::SetProfileAcceleration(int slavePosition, uint32_t countsPerSec2)
    {
        MailboxPriorityScope priority;
        uint16 slave = m->ResolveSlave(slavePosition);
        uint32 v = (uint32)countsPerSec2;
        ecx_SDOwrite(m->context, slave, AxisAddress::ProfileIndex(slavePosition, IDX_PROFILE_ACCELERATION), 0, FALSE, 4, &v, EC_TIMEOUTRXM);
    }

    void SoemEtherCatBus::SetProfileDeceleration(int slavePosition, uint32_t countsPerSec2)
    {
        MailboxPriorityScope priority;
        uint16 slave = m->ResolveSlave(slavePosition);
        uint32 v = (uint32)countsPerSec2;
        ecx_SDOwrite(m->context, slave, AxisAddress::ProfileIndex(slavePosition, IDX_PROFILE_DECELERATION), 0, FALSE, 4, &v, EC_TIMEOUTRXM);
    }

    int SoemEtherCatBus::GetActualVelocity(int slavePosition)
    {
        MailboxPriorityScope priority;
        uint16 slave = m->ResolveSlave(slavePosition);
        int32 v = 0;
        int size = sizeof(v);
        ecx_SDOread(m->context, slave, AxisAddress::ProfileIndex(slavePosition, IDX_VELOCITY_ACTUAL), 0, FALSE, &size, &v, EC_TIMEOUT_BESTEFFORT_POLL);
        return v;
    }

    int16_t SoemEtherCatBus::GetActualTorque(int slavePosition)
    {
        MailboxPriorityScope priority;
        uint16 slave = m->ResolveSlave(slavePosition);
        int16 v = 0;
        int size = sizeof(v);
        ecx_SDOread(m->context, slave, AxisAddress::ProfileIndex(slavePosition, IDX_TORQUE_ACTUAL), 0, FALSE, &size, &v, EC_TIMEOUT_BESTEFFORT_POLL);
        return v;
    }

    // ══════════════════════════════════════════════════════════════
    //  PDO theo chu kỳ — Controlword / Target Position (ra), Statusword / Mode Display / Actual
    //  Position / Error Code (vào), tại các offset dò sống lúc mở bus. Không SDO; đi qua ProcessImage
    //  nên không bao giờ chặn và không bao giờ bị luồng chu kỳ bắt gặp giữa chừng.
    // ══════════════════════════════════════════════════════════════

    void SoemEtherCatBus::SetControlword(int slavePosition, uint16_t controlword)
    {
        uint16 slave = 0;
        const PdoLayout& l = m->AxisLayout(slavePosition, slave);
        m->image.Output<uint16_t>(m->OutBase(slave) + l.outControlword, controlword);
    }

    void SoemEtherCatBus::SetTargetPosition(int slavePosition, int counts)
    {
        uint16 slave = 0;
        const PdoLayout& l = m->AxisLayout(slavePosition, slave);
        m->image.Output<int32_t>(m->OutBase(slave) + l.outTargetPosition, (int32_t)counts);
    }

    uint16_t SoemEtherCatBus::GetStatusword(int slavePosition)
    {
        uint16 slave = 0;
        const PdoLayout& l = m->AxisLayout(slavePosition, slave);
        return m->image.Input<uint16_t>(m->InBase(slave) + l.inStatusword);
    }

    int8_t SoemEtherCatBus::GetModeOfOperationDisplay(int slavePosition)
    {
        uint16 slave = 0;
        const PdoLayout& l = m->AxisLayout(slavePosition, slave);
        if (l.inModeDisplay < 0)
            return 0; // slave này không map — không quan trọng, báo "không rõ"
        return m->image.Input<int8_t>(m->InBase(slave) + l.inModeDisplay);
    }

    int SoemEtherCatBus::GetActualPosition(int slavePosition)
    {
        uint16 slave = 0;
        const PdoLayout& l = m->AxisLayout(slavePosition, slave);
        return m->image.Input<int32_t>(m->InBase(slave) + l.inPosition);
    }

    uint16_t SoemEtherCatBus::GetErrorCode(int slavePosition)
    {
        uint16 slave = 0;
        const PdoLayout& l = m->AxisLayout(slavePosition, slave);
        if (l.inErrorCode < 0)
            return 0; // slave này không map (Welcon WE2x không có 0x603F trong PDO mặc định) — không quan trọng
        return m->image.Input<uint16_t>(m->InBase(slave) + l.inErrorCode);
    }

    // ══════════════════════════════════════════════════════════════
    //  I/O số (Fastech Ezi-IO, Leadshine R1EC…) — đọc/ghi gói bit theo IoLayout. Xử lý bit bắt đầu
    //  không thẳng hàng byte và tới 64 kênh một cách tổng quát, không viết riêng theo model.
    // ══════════════════════════════════════════════════════════════

    uint64_t SoemEtherCatBus::GetDigitalInputs(int slavePosition)
    {
        uint16 slave = m->ResolveSlave(slavePosition);
        IoLayout io = m->ioLayout[slave];
        if (io.inputBitCount <= 0)
            return 0;

        int byteCount = (io.inputBitOffset + io.inputBitCount + 7) / 8;
        if (byteCount > 8) byteCount = 8;
        uint8_t bytes[8]{};
        m->image.ReadInput(m->InBase(slave) + io.inputByteOffset, bytes, (size_t)byteCount);

        uint64 raw = 0;
        for (int b = 0; b < byteCount; b++)
            raw |= ((uint64)bytes[b]) << (8 * b);
        raw >>= io.inputBitOffset;

        return raw & FullBitMask(io.inputBitCount);
    }

    void SoemEtherCatBus::SetDigitalOutputs(int slavePosition, uint64_t mask)
    {
        uint16 slave = m->ResolveSlave(slavePosition);
        IoLayout io = m->ioLayout[slave];
        if (io.outputBitCount <= 0)
            return;

        int byteCount = (io.outputBitOffset + io.outputBitCount + 7) / 8;
        if (byteCount > 8) byteCount = 8;
        uint64 bitMask = FullBitMask(io.outputBitCount);
        size_t offset = m->OutBase(slave) + io.outputByteOffset;

        // Đọc-sửa-ghi cả dải byte được phủ để chỉ bit của chính module này đổi — an toàn cả khi nó
        // không bắt đầu ở bit 0 của vùng ngõ ra.
        uint8_t bytes[8]{};
        m->image.ReadOutputShadow(offset, bytes, (size_t)byteCount);
        uint64 existing = 0;
        for (int b = 0; b < byteCount; b++)
            existing |= ((uint64)bytes[b]) << (8 * b);

        uint64 fieldMask = bitMask << io.outputBitOffset;
        uint64 newVal = (existing & ~fieldMask) | ((((uint64)mask) & bitMask) << io.outputBitOffset);

        for (int b = 0; b < byteCount; b++)
            bytes[b] = (uint8_t)((newVal >> (8 * b)) & 0xFF);
        m->image.WriteOutput(offset, bytes, (size_t)byteCount);
    }

    uint32_t SoemEtherCatBus::GetIoModuleDeviceStatus(int slavePosition)
    {
        uint16 slave = m->ResolveSlave(slavePosition);
        IoLayout io = m->ioLayout[slave];
        if (io.deviceStatusByteOffset < 0)
            return 0;
        return m->image.Input<uint32_t>(m->InBase(slave) + io.deviceStatusByteOffset);
    }

    std::vector<uint32_t> SoemEtherCatBus::GetDetectedModuleIdents(int slavePosition)
    {
        int idx = slavePosition + 1;
        if (idx < 1 || idx >= (int)m->detectedModules.size())
            return {};
        return m->detectedModules[idx]; // bản sao — bên gọi không sửa được trạng thái của ta
    }

    // ══════════════════════════════════════════════════════════════
    //  IORegister theo module — một register cho mỗi slot của bộ ghép modular, thay cho một register
    //  phủ cả rack. Rỗng (count 0) với slave không modular.
    // ══════════════════════════════════════════════════════════════

    int SoemEtherCatBus::GetIoSlotCount(int slavePosition)
    {
        uint16 slave = m->ResolveSlave(slavePosition);
        return s_hookIdentCount[slave];
    }

    uint32_t SoemEtherCatBus::GetIoSlotIdent(int slavePosition, int slot)
    {
        uint16 slave = m->ResolveSlave(slavePosition);
        if (slot < 0 || slot >= s_hookIdentCount[slave]) return 0;
        return s_hookIdents[slave][slot];
    }

    int SoemEtherCatBus::GetIoSlotInputBitCount(int slavePosition, int slot)
    {
        uint16 slave = m->ResolveSlave(slavePosition);
        if (slot < 0 || slot >= s_hookIdentCount[slave]) return 0;
        return s_hookSlotInBits[slave][slot];
    }

    int SoemEtherCatBus::GetIoSlotOutputBitCount(int slavePosition, int slot)
    {
        uint16 slave = m->ResolveSlave(slavePosition);
        if (slot < 0 || slot >= s_hookIdentCount[slave]) return 0;
        return s_hookSlotOutBits[slave][slot];
    }

    uint32_t SoemEtherCatBus::GetIoSlotInputRegister(int slavePosition, int slot)
    {
        uint16 slave = m->ResolveSlave(slavePosition);
        if (slot < 0 || slot >= s_hookIdentCount[slave])
            return 0;
        int bits = s_hookSlotInBits[slave][slot];
        if (bits <= 0) return 0;

        int byteCount = (bits + 7) / 8;
        if (byteCount > 4) byteCount = 4;
        uint8_t bytes[4]{};
        m->image.ReadInput(m->InBase(slave) + s_slotInByteOffset[slave][slot], bytes, (size_t)byteCount);

        uint32 raw = 0;
        for (int b = 0; b < byteCount; b++)
            raw |= ((uint32)bytes[b]) << (8 * b);
        return raw & FullBitMask32(bits);
    }

    void SoemEtherCatBus::SetIoSlotOutputRegister(int slavePosition, int slot, uint32_t value)
    {
        uint16 slave = m->ResolveSlave(slavePosition);
        if (slot < 0 || slot >= s_hookIdentCount[slave])
            return;
        int bits = s_hookSlotOutBits[slave][slot];
        if (bits <= 0) return;

        int byteCount = (bits + 7) / 8;
        if (byteCount > 4) byteCount = 4;
        uint32 masked = value & FullBitMask32(bits);
        uint8_t bytes[4]{};
        for (int b = 0; b < byteCount; b++)
            bytes[b] = (uint8_t)((masked >> (8 * b)) & 0xFF);
        m->image.WriteOutput(m->OutBase(slave) + s_slotOutByteOffset[slave][slot], bytes, (size_t)byteCount);
    }

    CyclicStats SoemEtherCatBus::GetCyclicStats()
    {
        rt::RtStats s = m->rtThread.Stats();
        CyclicStats result;
        result.PeriodJitterMinUs = s.PeriodJitterMinUs;
        result.PeriodJitterMaxUs = s.PeriodJitterMaxUs;
        result.PeriodJitterAvgUs = s.PeriodJitterAvgUs;
        result.ScanTimeMinUs = s.ScanTimeMinUs;
        result.ScanTimeMaxUs = s.ScanTimeMaxUs;
        result.ScanTimeAvgUs = s.ScanTimeAvgUs;
        result.SampleCount = s.Cycles > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)s.Cycles;
        return result;
    }

    void SoemEtherCatBus::SetModuleCatalog(const std::vector<EsiModuleInfo>& modules)
    {
        s_modCatCount = 0;
        for (const EsiModuleInfo& mod : modules)
        {
            if (mod.ModuleIdent == 0) continue;
            if (s_modCatCount >= MODCAT_MAX) break;
            // Module không có process data chiều nào (đầu cuối thuần) chỉ thêm nhiễu.
            if (mod.InputBits <= 0 && mod.OutputBits <= 0) continue;

            s_modIdent[s_modCatCount] = mod.ModuleIdent;
            s_modInBits[s_modCatCount] = mod.InputBits;
            s_modOutBits[s_modCatCount] = mod.OutputBits;
            CopyPdoInfo(mod.InputPdo, &s_modInPdo[s_modCatCount]);
            CopyPdoInfo(mod.OutputPdo, &s_modOutPdo[s_modCatCount]);
            CopyPdoInfo(mod.InputPdoAlt, &s_modInPdoAlt[s_modCatCount]);
            CopyPdoInfo(mod.OutputPdoAlt, &s_modOutPdoAlt[s_modCatCount]);
            s_modCatCount++;
        }
    }

    void SoemEtherCatBus::SetSyncManagerCatalog(const std::vector<EsiSyncManagerInfo>& syncManagers)
    {
        s_smCatCount = 0;
        for (const EsiSyncManagerInfo& sm : syncManagers)
        {
            if (sm.StartAddress == 0) continue;
            if (sm.Index < 2) continue; // SM mailbox luôn có trong EEPROM; chỉ SM2 trở lên mới có thể thiếu
            if (s_smCatCount >= SMCAT_MAX) break;

            s_smCat[s_smCatCount].vendorId = sm.VendorId;
            s_smCat[s_smCatCount].productCode = sm.ProductCode;
            s_smCat[s_smCatCount].index = sm.Index;
            s_smCat[s_smCatCount].startAddress = sm.StartAddress;
            s_smCat[s_smCatCount].controlByte = sm.ControlByte;
            s_smCat[s_smCatCount].enable = sm.Enable;
            s_smCatCount++;
        }
    }
}
