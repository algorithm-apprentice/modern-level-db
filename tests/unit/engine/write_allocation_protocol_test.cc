#include <gtest/gtest.h>

#include <new>

namespace modern_leveldb {
namespace {

// Observe replacement functions as a client, without inlining their implementation.
TEST(WriteAllocationTest, NothrowFormsUseTheSameAllocationAndReleaseFamily) {
    void* scalar = ::operator new(96, std::nothrow);
    void* array = ::operator new[](96, std::nothrow);
    ASSERT_NE(scalar, nullptr);
    ASSERT_NE(array, nullptr);
    ::operator delete(scalar);
    ::operator delete[](array);
    scalar = ::operator new(96, std::nothrow);
    array = ::operator new[](96, std::nothrow);
    ASSERT_NE(scalar, nullptr);
    ASSERT_NE(array, nullptr);
    ::operator delete(scalar, std::nothrow);
    ::operator delete[](array, std::nothrow);
}

}  // namespace
}  // namespace modern_leveldb
