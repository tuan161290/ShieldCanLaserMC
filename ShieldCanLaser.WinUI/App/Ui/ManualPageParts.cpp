#include "pch.h"
#include "Ui/ManualPageParts.h"

#include "Base/Async.h"
#include "Communication/IAxis.h"
#include "Controller/McControl.h"
#include "Ui/Dialogs.h"
#include "Ui/JogControl.h"
#include "Ui/UiUtil.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Documents;
using namespace winrt::Microsoft::UI::Xaml::Media;
using winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview;
namespace WF = winrt::Windows::Foundation;

namespace ui
{
    namespace
    {
        FrameworkElement FindDescendant(DependencyObject const& parent, std::wstring_view name, int depth)
        {
            int count = VisualTreeHelper::GetChildrenCount(parent);
            for (int i = 0; i < count; ++i)
            {
                auto child = VisualTreeHelper::GetChild(parent, i);
                if (auto fe = child.try_as<FrameworkElement>(); fe && fe.Name() == name) return fe;
                if (depth > 0)
                    if (auto found = FindDescendant(child, name, depth - 1)) return found;
            }
            return nullptr;
        }

        // Ô một nhóm chức năng của bảng trục (AxisGroupCellStyle của bản WPF): đường kẻ dọc mảnh bên trái ngăn nhóm.
        Border GroupCell(double width)
        {
            Border cell;
            cell.Width(width);
            cell.BorderBrush(Res(L"DividerBrush"));
            cell.BorderThickness(ThicknessHelper::FromLengths(1, 0, 0, 0));
            cell.Margin(ThicknessHelper::FromLengths(6, 0, 0, 0));
            cell.Padding(ThicknessHelper::FromLengths(6, 4, 0, 4));
            return cell;
        }

        Button CompactButton(wchar_t const* text, double width, wchar_t const* tip = nullptr)
        {
            return ManualCompactButton(text, width, tip);
        }

        TextBox NarrowBox(double width, double padX, wchar_t const* tip)
        {
            TextBox box;
            box.Width(width);
            box.MinWidth(0);   // MinWidth mặc định của TextBox WinUI (64) thắng Width
            box.MinHeight(36);
            box.Padding(ThicknessHelper::FromLengths(padX, 8, padX - 2, 0));
            box.VerticalAlignment(VerticalAlignment::Center);
            ToolTipService::SetToolTip(box, box_value(tip));
            HideTextBoxDeleteButton(box);
            return box;
        }

        StackPanel Horizontal()
        {
            StackPanel panel;
            panel.Orientation(Orientation::Horizontal);
            return panel;
        }

        /// <summary>Thân một lệnh dài trên luồng nền: bị huỷ (EMG/STOP) là đường bình thường, lỗi khác thì báo.</summary>
        void RunGuarded(std::string const& what, std::function<void()> const& fn)
        {
            try { fn(); }
            catch (const ::base::OperationCanceled&) {}
            catch (const std::exception& ex)
            {
                Dialogs::Error(what + ": " + ex.what());
            }
        }

        /// <summary>Thân một event handler trên luồng UI: exception không được lọt ra ranh giới WinRT.</summary>
        template <class F>
        void UiGuarded(F&& fn)
        {
            try { fn(); }
            catch (const std::exception& ex) { Dialogs::Error(ex.what()); }
            catch (winrt::hresult_error const& ex) { Dialogs::Error(winrt::to_string(ex.message())); }
        }
    }

    // ══════════ Đèn ══════════

    FrameworkElement ManualLamp::Build(double size, ManualLampBrushes const& brushes)
    {
        m_brushes = brushes;
        m_size = (float)size;

        m_root = Grid();
        m_root.Width(size);
        m_root.Height(size);
        m_root.VerticalAlignment(VerticalAlignment::Center);

        m_glowHost = Border();   // nằm DƯỚI chấm đèn: quầng sáng toả ra quanh chấm, không phủ lên nó
        m_dot = winrt::Microsoft::UI::Xaml::Shapes::Ellipse();   // tên trần đụng hàm GDI ::Ellipse
        m_dot.Width(size);
        m_dot.Height(size);
        m_dot.Fill(brushes.Off);
        m_dot.Stroke(brushes.Stroke);
        m_dot.StrokeThickness(1);

        m_root.Children().Append(m_glowHost);
        m_root.Children().Append(m_dot);
        m_state = 0;
        return m_root;
    }

    void ManualLamp::Set(bool on)
    {
        int state = on ? 1 : 0;
        if (!m_dot || m_state == state) return;
        m_state = state;
        m_dot.Fill(on ? m_brushes.On : m_brushes.Off);

        if (on && !m_glow)
        {
            // Glow riêng cỡ chấm (#FF5C5C, blur 6, không lệch) — bóng đổ lấy chính hình tròn làm mặt nạ.
            auto compositor = ElementCompositionPreview::GetElementVisual(m_glowHost).Compositor();
            m_glow = compositor.CreateSpriteVisual();
            m_glow.Size({ m_size, m_size });
            auto shadow = compositor.CreateDropShadow();
            shadow.BlurRadius(6.0f);
            shadow.Offset({ 0.0f, 0.0f, 0.0f });
            shadow.Opacity(1.0f);
            shadow.Color(ColorOf(0xFFFF5C5C));
            shadow.Mask(m_dot.GetAlphaMask());
            m_glow.Shadow(shadow);
            ElementCompositionPreview::SetElementChildVisual(m_glowHost, m_glow);
        }
        if (m_glow) m_glow.IsVisible(on);
    }

    Button ManualCompactButton(wchar_t const* text, double width, wchar_t const* tip)
    {
        Button b;
        b.Content(box_value(text));
        b.Width(width);
        b.Padding(ThicknessHelper::FromLengths(4, 3, 4, 3));
        if (tip) ToolTipService::SetToolTip(b, box_value(tip));
        return b;
    }

    void HideTextBoxDeleteButton(TextBox const& box)
    {
        auto token = std::make_shared<winrt::event_token>();
        *token = box.Loaded([token](WF::IInspectable const& sender, RoutedEventArgs const&) {
            auto tb = sender.as<TextBox>();
            tb.Loaded(*token);
            tb.ApplyTemplate();
            auto button = FindDescendant(tb, L"DeleteButton", 4).try_as<Button>();
            if (!button) return;
            button.MinWidth(0);
            button.Width(0);
            button.MaxWidth(0);
            button.Margin(ThicknessHelper::FromUniformLength(0));
            button.Padding(ThicknessHelper::FromUniformLength(0));
            button.Opacity(0);
            button.IsHitTestVisible(false);
            button.IsTabStop(false);
        });
    }

    // ══════════ Hàng trục ══════════

    std::shared_ptr<ManualAxisRow> ManualAxisRow::Create(std::shared_ptr<scs::IAxis> axis, ManualLampBrushes const& brushes)
    {
        auto row = std::make_shared<ManualAxisRow>(std::move(axis));
        row->Build(brushes);   // handler bắt weak_from_this — phải dựng SAU khi shared_ptr đã có
        row->Update();
        return row;
    }

    ManualAxisRow::ManualAxisRow(std::shared_ptr<scs::IAxis> axis)
        : m_axis(std::move(axis)), m_name(m_axis->Name())
    {
    }

    void ManualAxisRow::Build(ManualLampBrushes const& brushes)
    {
        std::weak_ptr<ManualAxisRow> weak = weak_from_this();
        auto cfg = m_axis->Config();

        m_root = Border();
        m_root.BorderBrush(Res(L"DividerBrush"));
        m_root.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
        StackPanel line = Horizontal();

        // ── Trục / Vị trí / Cữ ──
        TextBlock name;
        name.Width(30);
        name.Text(H(m_name));
        name.FontSize(14);
        name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        name.VerticalAlignment(VerticalAlignment::Center);
        line.Children().Append(name);

        TextBlock pos;
        pos.Width(84);
        pos.VerticalAlignment(VerticalAlignment::Center);
        pos.FontFamily(FontFamily(L"Consolas"));
        pos.FontSize(15);
        m_posRun = Run();
        Run gap;
        gap.Text(L" ");
        m_unitRun = Run();
        m_unitRun.FontSize(10);
        m_unitRun.Foreground(Res(L"TextSecondaryBrush"));
        pos.Inlines().Append(m_posRun);
        pos.Inlines().Append(gap);
        pos.Inlines().Append(m_unitRun);
        line.Children().Append(pos);

        // POT/NOT — Grid bọc ngoài giữ bề rộng cột kể cả khi 2 đèn bị ẩn (trục quay quấn vòng không có cữ).
        Grid limitCell;
        limitCell.Width(40);
        limitCell.VerticalAlignment(VerticalAlignment::Center);
        m_limitPanel = StackPanel();
        for (int i = 0; i < 2; ++i)
        {
            StackPanel lampLine = Horizontal();
            if (i == 0) lampLine.Margin(ThicknessHelper::FromLengths(0, 0, 0, 2));
            ManualLamp& lamp = i == 0 ? m_pot : m_not;
            lampLine.Children().Append(lamp.Build(10, brushes));
            TextBlock caption;
            caption.Text(i == 0 ? L"POT" : L"NOT");
            caption.FontSize(9);
            caption.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0));
            caption.VerticalAlignment(VerticalAlignment::Center);
            caption.Foreground(Res(L"TextSecondaryBrush"));
            lampLine.Children().Append(caption);
            m_limitPanel.Children().Append(lampLine);
        }
        limitCell.Children().Append(m_limitPanel);
        line.Children().Append(limitCell);

        // ── Servo: SVO (MỘT nút toggle theo Axis.ServoOn THẬT) / RST ──
        {
            Border cell = GroupCell(136);
            StackPanel group = Horizontal();
            m_svoButton = CompactButton(L"SVO OFF", 74);
            m_svoButton.Click([weak](auto&&, auto&&) { if (auto self = weak.lock()) self->OnServoToggle(); });
            m_rstButton = CompactButton(L"RST", 48, L"Reset the driver fault of this axis only (CiA402 Fault Reset)");
            m_rstButton.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0));
            m_rstButton.Click([weak](auto&&, auto&&) { if (auto self = weak.lock()) self->OnResetAlarm(); });
            group.Children().Append(m_svoButton);
            group.Children().Append(m_rstButton);
            cell.Child(group);
            line.Children().Append(cell);
        }

        // ── Gốc — đèn ORG gộp vào chính nút Home (xanh đặc khi Axis.OrgOk) ──
        {
            Border cell = GroupCell(72);
            m_homeButton = CompactButton(L"HOME", 64);
            m_homeButton.HorizontalAlignment(HorizontalAlignment::Left);
            m_homeButton.Click([weak](auto&&, auto&&) { if (auto self = weak.lock()) self->OnHome(); });
            cell.Child(m_homeButton);
            line.Children().Append(cell);
        }

        // ── Jog — giữ để chạy, nhả là dừng ──
        {
            Border cell = GroupCell(96);
            StackPanel group = Horizontal();
            Button minus = CompactButton(L"−", 42, L"Hold to jog backward, release to stop");
            Button plus = CompactButton(L"+", 42, L"Hold to jog forward, release to stop");
            plus.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0));
            AttachJog(minus, m_name, -1);
            AttachJog(plus, m_name, 1);
            group.Children().Append(minus);
            group.Children().Append(plus);
            cell.Child(group);
            line.Children().Append(cell);
        }

        // ── Inching (Move Increment) — dịch một BƯỚC từ vị trí hiện tại ──
        {
            Border cell = GroupCell(138);
            StackPanel group = Horizontal();
            m_stepBox = NarrowBox(46, 5, L"Inching step (axis units)");
            // Bước Inching mặc định — chỉnh lại được ở ô nhập ngay cạnh nút.
            m_stepBox.Text(H(std::format("{}", cfg && cfg->Unit == "deg" ? 1.0 : 0.1)));
            m_incMinusButton = CompactButton(L"−", 38, L"Inching: one step backward from current position");
            m_incMinusButton.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0));
            m_incMinusButton.Click([weak](auto&&, auto&&) { if (auto self = weak.lock()) self->OnInch(-1); });
            m_incPlusButton = CompactButton(L"+", 38, L"Inching: one step forward from current position");
            m_incPlusButton.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0));
            m_incPlusButton.Click([weak](auto&&, auto&&) { if (auto self = weak.lock()) self->OnInch(1); });
            group.Children().Append(m_stepBox);
            group.Children().Append(m_incMinusButton);
            group.Children().Append(m_incPlusButton);
            cell.Child(group);
            line.Children().Append(cell);
        }

        // ── Move Abs — chạy tới toạ độ tuyệt đối gõ tay ──
        {
            Border cell = GroupCell(146);
            StackPanel group = Horizontal();
            m_targetBox = NarrowBox(70, 6, L"Absolute target position for Move Abs");
            m_targetBox.Text(H(std::format("{}", cfg ? cfg->StandbyPos : 0.0)));
            m_moveButton = CompactButton(L"MOVE", 64, L"Move Abs: move to the entered absolute position");
            m_moveButton.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0));
            m_moveButton.Click([weak](auto&&, auto&&) { if (auto self = weak.lock()) self->OnMove(); });
            group.Children().Append(m_targetBox);
            group.Children().Append(m_moveButton);
            cell.Child(group);
            line.Children().Append(cell);
        }

        // ── Dừng riêng trục này — không bao giờ bị khoá ──
        {
            Border cell = GroupCell(68);
            Button stop = CompactButton(L"STOP", 60);
            stop.HorizontalAlignment(HorizontalAlignment::Left);
            stop.Click([weak](auto&&, auto&&) { if (auto self = weak.lock()) self->OnStop(); });
            cell.Child(stop);
            line.Children().Append(cell);
        }

        m_root.Child(line);
    }

    void ManualAxisRow::Update()
    {
        if (!m_root) return;
        const scs::IAxis& axis = *m_axis;

        winrt::hstring pos = Num(axis.CurrentPos(), 3);
        if (pos != m_shownPos)
        {
            m_shownPos = pos;
            m_posRun.Text(pos);
        }

        // Unit/WrapDegrees sửa được ở trang Setting (cùng đối tượng cấu hình) — đọc lại mỗi nhịp, rẻ.
        if (auto cfg = axis.Config())
        {
            if (cfg->Unit != m_shownUnit)
            {
                m_shownUnit = cfg->Unit;
                m_unitRun.Text(H(cfg->Unit));
            }
            int hasLimit = cfg->WrapDegrees ? 0 : 1;
            if (hasLimit != m_shownHasLimit)
            {
                m_shownHasLimit = hasLimit;
                Show(m_limitPanel, hasLimit != 0);
            }
        }

        // POT/NOT — đọc sống Axis.FLS/RLS: thấy đúng lúc driver nhả bit cữ.
        m_pot.Set(axis.FLS());
        m_not.Set(axis.RLS());

        int servo = axis.ServoOn() ? 1 : 0;
        if (servo != m_shownServo)
        {
            m_shownServo = servo;
            m_svoButton.Content(box_value(servo ? L"SVO ON" : L"SVO OFF"));
            SetTint(m_svoButton, servo ? Tint::Green : Tint::None);
        }
        int alarm = axis.Alarm() ? 1 : 0;
        if (alarm != m_shownAlarm)
        {
            m_shownAlarm = alarm;
            SetTint(m_rstButton, alarm ? Tint::Red : Tint::None);
        }
        int org = axis.OrgOk() ? 1 : 0;
        if (org != m_shownOrg)
        {
            m_shownOrg = org;
            SetTint(m_homeButton, org ? Tint::Green : Tint::None);
        }

        ApplyBusy(m_busy->load());
    }

    void ManualAxisRow::ApplyBusy(bool busy)
    {
        int value = busy ? 1 : 0;
        if (value == m_shownBusy) return;
        m_shownBusy = value;
        for (auto const& button : { m_svoButton, m_rstButton, m_homeButton, m_incMinusButton, m_incPlusButton, m_moveButton })
            button.IsEnabled(!busy);
    }

    void ManualAxisRow::RunBlocking(std::function<void()> work)
    {
        if (m_busy->exchange(true)) return;
        ApplyBusy(true);
        std::weak_ptr<ManualAxisRow> weak = weak_from_this();
        RunBackgroundThenUi(std::move(work), [busy = m_busy, weak] {
            busy->store(false);
            if (auto self = weak.lock()) self->ApplyBusy(false);
        });
    }

    // MỘT nút toggle: tự quyết định bật/tắt theo Axis.ServoOn THẬT (driver xác nhận), không phải cờ lạc quan phía
    // UI. Alarm còn sống thì không cho bật — phải RST trước.
    void ManualAxisRow::OnServoToggle()
    {
        UiGuarded([&] {
            auto axis = m_axis;
            std::string what = "SVO " + m_name;
            if (axis->ServoOn())
            {
                RunBlocking([axis, what] { RunGuarded(what, [&] { axis->SetServoOn(false); }); });
                return;
            }
            if (axis->Alarm())
            {
                scs::McControl::Status().SetMessage(m_name + " in alarm — press RST and clear the fault before servo on");
                return;
            }
            RunBlocking([axis, what] { RunGuarded(what, [&] { axis->SetServoOn(true); }); });
        });
    }

    void ManualAxisRow::OnResetAlarm()
    {
        UiGuarded([&] {
            auto axis = m_axis;
            std::string what = "RST " + m_name;
            RunBlocking([axis, what] { RunGuarded(what, [&] { axis->ResetAlarm(); }); });
        });
    }

    void ManualAxisRow::OnHome()
    {
        UiGuarded([&] {
            auto func = scs::McControl::Func();
            if (!func) return;
            // CanMoveAutonomous (không phải CanMove trơn) — về gốc X/Y bấm riêng lẻ còn đòi Z đã về gốc THẬT,
            // không chỉ đang đọc vị trí "an toàn".
            std::string reason;
            if (!func->CanMoveAutonomous(m_name, reason))
            {
                scs::McControl::Status().SetMessage("Cannot home " + m_name + ": " + reason);
                return;
            }
            auto axis = m_axis;
            RunBlocking([axis, what = "Home " + m_name] {
                RunGuarded(what, [&] {
                    auto safety = scs::McControl::Safety();
                    if (!safety) return;
                    scs::McControl::Motion().Home(axis, safety->AbortToken());
                });
            });
        });
    }

    void ManualAxisRow::OnMove()
    {
        UiGuarded([&] {
            auto func = scs::McControl::Func();
            if (!func) return;
            double target = 0;
            winrt::hstring text = m_targetBox.Text();
            if (!ParseDouble(text, target))
            {
                Dialogs::Warn("Move Abs target for axis " + m_name + " is not a valid number: \"" + U8(text) +
                              "\". Enter a number, e.g. 12.5");
                return;
            }
            std::string reason;
            if (!func->CanMoveAutonomous(m_name, reason))
            {
                scs::McControl::Status().SetMessage("Cannot move " + m_name + ": " + reason);
                return;
            }
            auto axis = m_axis;
            RunBlocking([axis, target, what = "Move " + m_name] {
                RunGuarded(what, [&] {
                    auto func = scs::McControl::Func();
                    auto safety = scs::McControl::Safety();
                    if (!func || !safety) return;
                    func->Move(axis->Name(), target, axis->Config()->MoveSpeed, safety->AbortToken());
                });
            });
        });
    }

    // Inching — dịch một BƯỚC tính từ vị trí HIỆN TẠI, khác Move Abs (đi thẳng tới đích). McFunction::MoveInc tự kiểm
    // CanMove và ghi lý do từ chối vào Status.Message.
    void ManualAxisRow::OnInch(int dir)
    {
        UiGuarded([&] {
            double step = 0;
            winrt::hstring text = m_stepBox.Text();
            if (!ParseDouble(text, step))
            {
                Dialogs::Warn("Inching step for axis " + m_name + " is not a valid number: \"" + U8(text) +
                              "\". Enter a number, e.g. 0.1");
                return;
            }
            auto axis = m_axis;
            double delta = dir * step;
            RunBlocking([axis, delta, what = "Inching " + m_name] {
                RunGuarded(what, [&] {
                    auto func = scs::McControl::Func();
                    auto safety = scs::McControl::Safety();
                    if (!func || !safety) return;
                    func->MoveInc(axis->Name(), delta, axis->Config()->MoveSpeed, safety->AbortToken());
                });
            });
        });
    }

    // Cùng hàng đợi với lệnh jog: không có lệnh jog nào đang xếp hàng lại chạy SAU lệnh dừng.
    void ManualAxisRow::OnStop()
    {
        auto axis = m_axis;
        PostMotionCommand([axis] { axis->Stop(); });
    }
}
