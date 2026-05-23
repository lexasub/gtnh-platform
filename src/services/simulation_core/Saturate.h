#pragma once
#include <concepts>
#include <limits>
#include <type_traits>

namespace simcore {

template<std::integral T>
constexpr T add_sat(T a, T b) noexcept {
    if constexpr (std::is_unsigned_v<T>) {
        T sum = a + b;
        return sum < a ? std::numeric_limits<T>::max() : sum;
    } else {
        if (b > 0 && a > std::numeric_limits<T>::max() - b)
            return std::numeric_limits<T>::max();
        if (b < 0 && a < std::numeric_limits<T>::min() - b)
            return std::numeric_limits<T>::min();
        return a + b;
    }
}

template<std::integral T>
constexpr T sub_sat(T a, T b) noexcept {
    if constexpr (std::is_unsigned_v<T>) {
        return a < b ? 0 : a - b;
    } else {
        if (b < 0 && a > std::numeric_limits<T>::max() + b)
            return std::numeric_limits<T>::max();
        if (b > 0 && a < std::numeric_limits<T>::min() + b)
            return std::numeric_limits<T>::min();
        return a - b;
    }
}

} // namespace simcore
