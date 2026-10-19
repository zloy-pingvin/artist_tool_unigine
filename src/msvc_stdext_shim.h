#pragma once

// Qt 6.5.3 headers use stdext::make_(un)checked_array_iterator, removed from
// the MSVC STL in VS2022 17.10+ toolsets. Force-included before Qt headers
// (see /FI in CMakeLists.txt) to restore them as plain-pointer pass-throughs,
// matching Qt's own fallback QT_MAKE_CHECKED_ARRAY_ITERATOR(x, N) == (x).
#if defined(_MSC_VER) && _MSC_VER >= 1940

#include <cstddef>

namespace stdext
{
template <typename T>
constexpr T *make_checked_array_iterator(T *ptr, size_t, size_t offset = 0)
{
	return ptr + offset;
}

template <typename T>
constexpr T *make_unchecked_array_iterator(T *ptr)
{
	return ptr;
}
} // namespace stdext

#endif
