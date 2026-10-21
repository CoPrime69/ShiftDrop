#include "Accessibility.h"

#include <oleauto.h>
#include <wil/resource.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace sd::a11y
{
    namespace
    {
        // UiaRaiseNotificationEvent is Windows 10 1709+, and the SDK only declares it when building for
        // that (NTDDI_WIN10_RS3); this build targets plain Windows 10, so it is looked up. The values
        // are NotificationKind and NotificationProcessing.
        using RaiseNotification = HRESULT(WINAPI*)(IRawElementProviderSimple*, int kind, int processing, BSTR text, BSTR activity);
        constexpr int kKindActionCompleted = 2, kKindOther = 4;
        constexpr int kProcessImportantAll = 0, kProcessMostRecent = 3;

        RaiseNotification Notifier()
        {
            static const auto fn = [] {
                const HMODULE uia = GetModuleHandleW(L"uiautomationcore.dll");   // linked, so already loaded
                return uia ? reinterpret_cast<RaiseNotification>(reinterpret_cast<void*>(GetProcAddress(uia, "UiaRaiseNotificationEvent")))
                           : nullptr;
            }();
            return fn;
        }

        HRESULT StringVariant(const std::wstring& text, VARIANT* out)
        {
            out->vt = VT_BSTR;
            out->bstrVal = SysAllocString(text.c_str());
            return out->bstrVal ? S_OK : E_OUTOFMEMORY;
        }

        void IntVariant(int value, VARIANT* out)
        {
            out->vt = VT_I4;
            out->lVal = value;
        }

        void BoolVariant(bool value, VARIANT* out)
        {
            out->vt = VT_BOOL;
            out->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        }

        // What every provider here says about itself: it lives in this process and wants its calls on
        // the thread that made it (the UI thread, an STA), so none of it needs locking.
        constexpr ProviderOptions kOptions = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
    }

    void Announce(IRawElementProviderSimple* source, const std::wstring& text, const wchar_t* activity, bool done)
    {
        const auto raise = Notifier();
        if (!raise || !source || text.empty() || !UiaClientsAreListening())
            return;
        wil::unique_bstr display(SysAllocString(text.c_str())), id(SysAllocString(activity));
        // A result is queued behind whatever is being said; news along the way replaces older news.
        raise(source, done ? kKindActionCompleted : kKindOther, done ? kProcessImportantAll : kProcessMostRecent, display.get(), id.get());
    }

    // ---- TextElement -----------------------------------------------------------------------------

    class TextElement::Provider final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IRawElementProviderSimple>
    {
    public:
        explicit Provider(HWND hwnd) : m_hwnd(hwnd) {}

        void Detach() { m_hwnd = nullptr; }
        std::wstring name;

        IFACEMETHODIMP get_ProviderOptions(ProviderOptions* options) override
        {
            *options = kOptions;
            return S_OK;
        }
        IFACEMETHODIMP GetPatternProvider(PATTERNID, IUnknown** provider) override
        {
            *provider = nullptr;
            return S_OK;
        }
        IFACEMETHODIMP GetPropertyValue(PROPERTYID id, VARIANT* value) override
        {
            value->vt = VT_EMPTY;
            if (!m_hwnd)
                return UIA_E_ELEMENTNOTAVAILABLE;
            switch (id)
            {
            case UIA_ControlTypePropertyId: IntVariant(UIA_TextControlTypeId, value); return S_OK;
            case UIA_NamePropertyId: return StringVariant(name, value);
            case UIA_IsKeyboardFocusablePropertyId: BoolVariant(false, value); return S_OK;
            default: return S_OK;   // VT_EMPTY: the host window's value
            }
        }
        IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** host) override
        {
            *host = nullptr;
            return m_hwnd ? UiaHostProviderFromHwnd(m_hwnd, host) : UIA_E_ELEMENTNOTAVAILABLE;
        }

    private:
        HWND m_hwnd;
    };

    TextElement::TextElement(HWND hwnd) : m_provider(Microsoft::WRL::Make<Provider>(hwnd)), m_hwnd(hwnd) {}

    TextElement::~TextElement()
    {
        if (!m_provider)
            return;
        m_provider->Detach();
        UiaReturnRawElementProvider(m_hwnd, 0, 0, nullptr);   // UIA's per-window bookkeeping goes too
        UiaDisconnectProvider(m_provider.Get());
    }

    bool TextElement::OnGetObject(WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        if (!m_provider || static_cast<long>(lParam) != static_cast<long>(UiaRootObjectId))
            return false;
        result = UiaReturnRawElementProvider(m_hwnd, wParam, lParam, m_provider.Get());
        return true;
    }

    void TextElement::SetName(std::wstring name)
    {
        if (m_provider)
            m_provider->name = std::move(name);
    }

    void TextElement::Announce(const std::wstring& text, const wchar_t* activity, bool done)
    {
        SetName(text);
        a11y::Announce(m_provider.Get(), text, activity, done);
    }

    // ---- WheelElement ----------------------------------------------------------------------------
    //
    // The root (the list) is what the window hands out; the items are made each time the slices change
    // and remember which showing they belong to, so one kept by a client past a switch says it is gone
    // instead of describing another slice.

    class WheelElement::Root final
        : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IRawElementProviderSimple, IRawElementProviderFragment,
                              IRawElementProviderFragmentRoot, ISelectionProvider>
    {
    public:
        Root(HWND hwnd, WheelView& view) : m_hwnd(hwnd), m_view(&view) {}

        void Detach();
        void Rebuild();   // new items for what the wheel shows now
        WheelView* View() const { return m_view; }
        unsigned Generation() const { return m_generation; }
        Item* ItemAt(int index) const;

        // IRawElementProviderSimple
        IFACEMETHODIMP get_ProviderOptions(ProviderOptions* options) override
        {
            *options = kOptions;
            return S_OK;
        }
        IFACEMETHODIMP GetPatternProvider(PATTERNID pattern, IUnknown** provider) override
        {
            *provider = nullptr;
            if (pattern == UIA_SelectionPatternId)
            {
                *provider = static_cast<ISelectionProvider*>(this);
                AddRef();
            }
            return S_OK;
        }
        IFACEMETHODIMP GetPropertyValue(PROPERTYID id, VARIANT* value) override;
        IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** host) override
        {
            *host = nullptr;
            return m_view ? UiaHostProviderFromHwnd(m_hwnd, host) : UIA_E_ELEMENTNOTAVAILABLE;
        }

        // IRawElementProviderFragment
        IFACEMETHODIMP Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override;
        IFACEMETHODIMP GetRuntimeId(SAFEARRAY** id) override
        {
            *id = nullptr;   // the host window supplies the root's
            return S_OK;
        }
        IFACEMETHODIMP get_BoundingRectangle(UiaRect* rect) override
        {
            *rect = {};   // likewise
            return S_OK;
        }
        IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** roots) override
        {
            *roots = nullptr;
            return S_OK;
        }
        IFACEMETHODIMP SetFocus() override { return S_OK; }   // the wheel never takes the focus
        IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** root) override
        {
            *root = this;
            AddRef();
            return S_OK;
        }

        // IRawElementProviderFragmentRoot
        IFACEMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override;
        IFACEMETHODIMP GetFocus(IRawElementProviderFragment** out) override
        {
            *out = nullptr;
            return S_OK;
        }

        // ISelectionProvider
        IFACEMETHODIMP GetSelection(SAFEARRAY** selection) override;
        IFACEMETHODIMP get_CanSelectMultiple(BOOL* multiple) override
        {
            *multiple = FALSE;
            return S_OK;
        }
        IFACEMETHODIMP get_IsSelectionRequired(BOOL* required) override
        {
            *required = FALSE;
            return S_OK;
        }

    private:
        HWND m_hwnd;
        WheelView* m_view;
        unsigned m_generation = 0;
        std::vector<ComPtr<Item>> m_items;
    };

    class WheelElement::Item final
        : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IRawElementProviderSimple, IRawElementProviderFragment, ISelectionItemProvider>
    {
    public:
        Item(Root* root, int index) : m_root(root), m_index(index), m_generation(root->Generation()) {}

        // IRawElementProviderSimple
        IFACEMETHODIMP get_ProviderOptions(ProviderOptions* options) override
        {
            *options = kOptions;
            return S_OK;
        }
        IFACEMETHODIMP GetPatternProvider(PATTERNID pattern, IUnknown** provider) override
        {
            *provider = nullptr;
            if (pattern == UIA_SelectionItemPatternId)
            {
                *provider = static_cast<ISelectionItemProvider*>(this);
                AddRef();
            }
            return S_OK;
        }
        IFACEMETHODIMP GetPropertyValue(PROPERTYID id, VARIANT* value) override
        {
            value->vt = VT_EMPTY;
            if (!Live())
                return UIA_E_ELEMENTNOTAVAILABLE;
            const auto slice = m_root->View()->ViewSlice(static_cast<size_t>(m_index));
            switch (id)
            {
            case UIA_ControlTypePropertyId: IntVariant(UIA_ListItemControlTypeId, value); return S_OK;
            case UIA_NamePropertyId: return StringVariant(slice.name, value);
            case UIA_IsEnabledPropertyId: BoolVariant(slice.enabled, value); return S_OK;
            case UIA_IsKeyboardFocusablePropertyId: BoolVariant(false, value); return S_OK;
            case UIA_IsOffscreenPropertyId: BoolVariant(false, value); return S_OK;
            default: return S_OK;
            }
        }
        IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** host) override
        {
            *host = nullptr;   // not a window of its own
            return S_OK;
        }

        // IRawElementProviderFragment
        IFACEMETHODIMP Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override
        {
            *out = nullptr;
            if (!Live())
                return UIA_E_ELEMENTNOTAVAILABLE;
            IRawElementProviderFragment* to = nullptr;
            if (direction == NavigateDirection_Parent)
                to = m_root.Get();
            else if (direction == NavigateDirection_NextSibling)
                to = m_root->ItemAt(m_index + 1);
            else if (direction == NavigateDirection_PreviousSibling)
                to = m_root->ItemAt(m_index - 1);
            if (to)
                to->AddRef();
            *out = to;
            return S_OK;
        }
        IFACEMETHODIMP GetRuntimeId(SAFEARRAY** id) override
        {
            *id = nullptr;
            if (!Live())
                return UIA_E_ELEMENTNOTAVAILABLE;
            // UiaAppendRuntimeId: prefixed with the window's own id, unique within it after that.
            const int parts[] = { UiaAppendRuntimeId, static_cast<int>(m_generation), m_index + 1 };
            *id = SafeArrayCreateVector(VT_I4, 0, ARRAYSIZE(parts));
            if (!*id)
                return E_OUTOFMEMORY;
            for (LONG i = 0; i < static_cast<LONG>(ARRAYSIZE(parts)); ++i)
            {
                int part = parts[i];
                SafeArrayPutElement(*id, &i, &part);
            }
            return S_OK;
        }
        IFACEMETHODIMP get_BoundingRectangle(UiaRect* rect) override
        {
            *rect = {};
            if (!Live())
                return UIA_E_ELEMENTNOTAVAILABLE;
            const RECT r = m_root->View()->ViewSlice(static_cast<size_t>(m_index)).bounds;
            *rect = UiaRect{ static_cast<double>(r.left), static_cast<double>(r.top), static_cast<double>(r.right - r.left),
                             static_cast<double>(r.bottom - r.top) };
            return S_OK;
        }
        IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** roots) override
        {
            *roots = nullptr;
            return S_OK;
        }
        IFACEMETHODIMP SetFocus() override { return S_OK; }
        IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** root) override
        {
            *root = nullptr;
            if (!Live())
                return UIA_E_ELEMENTNOTAVAILABLE;
            *root = m_root.Get();
            m_root->AddRef();
            return S_OK;
        }

        // ISelectionItemProvider. The highlight follows the pointer and the arrow keys; it is not set
        // from outside.
        IFACEMETHODIMP Select() override { return UIA_E_INVALIDOPERATION; }
        IFACEMETHODIMP AddToSelection() override { return UIA_E_INVALIDOPERATION; }
        IFACEMETHODIMP RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
        IFACEMETHODIMP get_IsSelected(BOOL* selected) override
        {
            *selected = Live() && m_root->View()->ViewHighlighted() == m_index;
            return S_OK;
        }
        IFACEMETHODIMP get_SelectionContainer(IRawElementProviderSimple** container) override
        {
            *container = nullptr;
            if (!Live())
                return UIA_E_ELEMENTNOTAVAILABLE;
            *container = static_cast<IRawElementProviderSimple*>(m_root.Get());
            m_root->AddRef();
            return S_OK;
        }

    private:
        bool Live() const
        {
            const WheelView* view = m_root->View();
            return view && view->ViewOpen() && m_root->Generation() == m_generation && static_cast<size_t>(m_index) < view->ViewCount();
        }

        ComPtr<Root> m_root;   // the root holds its items only until the next Rebuild, so no cycle stays
        int m_index;
        unsigned m_generation;
    };

    void WheelElement::Root::Detach()
    {
        m_view = nullptr;
        ++m_generation;
        m_items.clear();
    }

    void WheelElement::Root::Rebuild()
    {
        ++m_generation;
        m_items.clear();
        if (!m_view || !m_view->ViewOpen())
            return;
        for (size_t i = 0; i < m_view->ViewCount(); ++i)
            m_items.push_back(Microsoft::WRL::Make<Item>(this, static_cast<int>(i)));
    }

    WheelElement::Item* WheelElement::Root::ItemAt(int index) const
    {
        return index >= 0 && static_cast<size_t>(index) < m_items.size() ? m_items[static_cast<size_t>(index)].Get() : nullptr;
    }

    HRESULT WheelElement::Root::GetPropertyValue(PROPERTYID id, VARIANT* value)
    {
        value->vt = VT_EMPTY;
        if (!m_view)
            return UIA_E_ELEMENTNOTAVAILABLE;
        switch (id)
        {
        case UIA_ControlTypePropertyId: IntVariant(UIA_ListControlTypeId, value); return S_OK;
        case UIA_NamePropertyId:
        {
            const auto title = m_view->ViewTitle();
            return StringVariant(title.empty() ? std::wstring(L"ShiftDrop wheel") : L"ShiftDrop wheel: " + title, value);
        }
        case UIA_IsKeyboardFocusablePropertyId: BoolVariant(false, value); return S_OK;
        default: return S_OK;
        }
    }

    HRESULT WheelElement::Root::Navigate(NavigateDirection direction, IRawElementProviderFragment** out)
    {
        *out = nullptr;
        if (!m_view)
            return UIA_E_ELEMENTNOTAVAILABLE;
        IRawElementProviderFragment* to = nullptr;
        if (direction == NavigateDirection_FirstChild)
            to = ItemAt(0);
        else if (direction == NavigateDirection_LastChild)
            to = ItemAt(static_cast<int>(m_items.size()) - 1);
        // Parent and siblings: the host window's.
        if (to)
            to->AddRef();
        *out = to;
        return S_OK;
    }

    HRESULT WheelElement::Root::ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out)
    {
        *out = nullptr;
        if (!m_view)
            return UIA_E_ELEMENTNOTAVAILABLE;
        const POINT pt{ static_cast<LONG>(std::floor(x)), static_cast<LONG>(std::floor(y)) };
        IRawElementProviderFragment* hit = m_view->ViewOpen() ? ItemAt(m_view->ViewSliceAt(pt)) : nullptr;
        if (!hit)
            hit = this;
        hit->AddRef();
        *out = hit;
        return S_OK;
    }

    HRESULT WheelElement::Root::GetSelection(SAFEARRAY** selection)
    {
        *selection = nullptr;
        if (!m_view)
            return UIA_E_ELEMENTNOTAVAILABLE;
        Item* item = m_view->ViewOpen() ? ItemAt(m_view->ViewHighlighted()) : nullptr;
        *selection = SafeArrayCreateVector(VT_UNKNOWN, 0, item ? 1 : 0);
        if (!*selection)
            return E_OUTOFMEMORY;
        if (item)
        {
            LONG i = 0;
            // SafeArrayPutElement takes its own reference.
            SafeArrayPutElement(*selection, &i, static_cast<IUnknown*>(static_cast<IRawElementProviderSimple*>(item)));
        }
        return S_OK;
    }

    WheelElement::WheelElement(HWND hwnd, WheelView& view) : m_root(Microsoft::WRL::Make<Root>(hwnd, view)), m_hwnd(hwnd) {}

    WheelElement::~WheelElement()
    {
        if (!m_root)
            return;
        m_root->Detach();
        UiaReturnRawElementProvider(m_hwnd, 0, 0, nullptr);
        UiaDisconnectProvider(m_root.Get());
    }

    bool WheelElement::OnGetObject(WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        if (!m_root || static_cast<long>(lParam) != static_cast<long>(UiaRootObjectId))
            return false;
        result = UiaReturnRawElementProvider(m_hwnd, wParam, lParam, m_root.Get());
        return true;
    }

    void WheelElement::SlicesChanged(bool announceSection)
    {
        if (!m_root)
            return;
        m_root->Rebuild();
        m_spoken = -1;   // the slice under the cursor is news again on the new slices
        WheelView* view = m_root->View();
        if (!UiaClientsAreListening() || !view || !view->ViewOpen())
            return;
        UiaRaiseStructureChangedEvent(m_root.Get(), StructureChangeType_ChildrenInvalidated, nullptr, 0);
        if (announceSection)
        {
            const size_t n = view->ViewCount();
            const auto title = view->ViewTitle();
            Announce(m_root.Get(), std::format(L"{}{}{} action{}", title, title.empty() ? L"" : L" wheel, ", n, n == 1 ? L"" : L"s"),
                     L"ShiftDrop.Wheel.Section");
        }
    }

    void WheelElement::HighlightChanged()
    {
        WheelView* view = m_root ? m_root->View() : nullptr;
        if (!view || !view->ViewOpen())
            return;
        const int hot = view->ViewHighlighted();
        if (hot == m_spoken)
            return;
        m_spoken = hot;
        Item* item = m_root->ItemAt(hot);
        if (!item || !UiaClientsAreListening())
            return;
        UiaRaiseAutomationEvent(item, UIA_SelectionItem_ElementSelectedEventId);
        Announce(item, view->ViewSlice(static_cast<size_t>(hot)).name, L"ShiftDrop.Wheel.Slice");
    }

    void WheelElement::Closed()
    {
        if (!m_root)
            return;
        m_root->Rebuild();   // no items while closed
        m_spoken = -1;
        if (UiaClientsAreListening())
            UiaRaiseStructureChangedEvent(m_root.Get(), StructureChangeType_ChildrenInvalidated, nullptr, 0);
    }
}
