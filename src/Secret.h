#pragma once
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>

namespace sd
{
    // Text that must not linger in memory, such as an archive's password. It lives in one fixed
    // buffer that is never reallocated or copied (only moved), and is wiped with SecureZeroMemory
    // (which the compiler may not optimise away) when it is cleared, reassigned or destroyed.
    template <class Char, size_t Capacity>
    class BasicSecret
    {
    public:
        static constexpr size_t kCapacity = Capacity;   // characters, without the terminating zero

        BasicSecret() = default;
        ~BasicSecret() { Clear(); }
        BasicSecret(BasicSecret&& other) noexcept : m_data(std::move(other.m_data)), m_size(other.m_size) { other.m_size = 0; }
        BasicSecret& operator=(BasicSecret&& other) noexcept
        {
            if (this != &other)
            {
                Clear();
                m_data = std::move(other.m_data);
                m_size = other.m_size;
                other.m_size = 0;
            }
            return *this;
        }
        BasicSecret(const BasicSecret&) = delete;
        BasicSecret& operator=(const BasicSecret&) = delete;

        // Room for kCapacity characters and a zero, for an API to write straight into (an edit
        // control, ReadFile); SetSize then says how much it wrote.
        Char* Buffer()
        {
            if (!m_data)
                m_data = std::make_unique<Char[]>(Capacity + 1);   // zero-filled
            return m_data.get();
        }
        void SetSize(size_t size)
        {
            m_size = std::min(size, Capacity);
            Buffer()[m_size] = Char{};
        }
        // False (and empty) when `text` is longer than kCapacity.
        bool Assign(std::basic_string_view<Char> text)
        {
            Clear();
            if (text.size() > Capacity)
                return false;
            std::copy(text.begin(), text.end(), Buffer());
            SetSize(text.size());
            return true;
        }
        void Clear()
        {
            if (m_data)
                SecureZeroMemory(m_data.get(), (Capacity + 1) * sizeof(Char));
            m_size = 0;
        }

        const Char* c_str() const { return m_data ? m_data.get() : kEmpty; }
        size_t size() const { return m_size; }
        bool empty() const { return m_size == 0; }
        std::basic_string_view<Char> View() const { return { c_str(), m_size }; }

    private:
        static constexpr Char kEmpty[1] = {};
        std::unique_ptr<Char[]> m_data;
        size_t m_size = 0;
    };

    // A password as typed (UTF-16), and one encoded for a library that takes bytes (UTF-8 is at
    // most three bytes per UTF-16 unit).
    using Secret = BasicSecret<wchar_t, 256>;
    using SecretBytes = BasicSecret<char, 3 * 256>;
}
