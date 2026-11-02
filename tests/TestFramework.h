/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 * Minimal self-contained test framework (no external dependencies).
 */

#ifndef MOD_TRUE_COMPANION_TEST_FRAMEWORK_H
#define MOD_TRUE_COMPANION_TEST_FRAMEWORK_H

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace rtest
{
    struct TestCase
    {
        std::string suite, name;
        std::function<void()> fn;
    };

    inline std::vector<TestCase>& Registry()
    {
        static std::vector<TestCase> r;
        return r;
    }

    struct Failure
    {
        std::string message;
    };

    struct Registrar
    {
        Registrar(char const* suite, char const* name, std::function<void()> fn) { Registry().push_back({ suite, name, std::move(fn) }); }
    };

    inline int& Checks()
    {
        static int c = 0;
        return c;
    }
}

#define RT_CONCAT2(a, b) a##b
#define RT_CONCAT(a, b) RT_CONCAT2(a, b)

#define TEST(suite, name)                                                                                   \
    static void RT_CONCAT(test_##suite##_, name)();                                                        \
    static rtest::Registrar RT_CONCAT(reg_##suite##_, name)(#suite, #name, &RT_CONCAT(test_##suite##_, name)); \
    static void RT_CONCAT(test_##suite##_, name)()

#define RT_FAIL(msg)                                                                     \
    do                                                                                   \
    {                                                                                    \
        std::ostringstream _o;                                                           \
        _o << __FILE__ << ":" << __LINE__ << ": " << msg;                                \
        throw rtest::Failure{ _o.str() };                                                \
    } while (0)

#define CHECK(cond)                                    \
    do                                                 \
    {                                                  \
        ++rtest::Checks();                             \
        if (!(cond))                                   \
            RT_FAIL("CHECK failed: " #cond);           \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do                                                                                              \
    {                                                                                               \
        ++rtest::Checks();                                                                          \
        auto const _a = (a);                                                                        \
        auto const _b = (b);                                                                        \
        if (!(_a == _b))                                                                            \
            RT_FAIL("CHECK_EQ failed: " #a " == " #b " (got '" << _a << "' vs '" << _b << "')");  \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                     \
    do                                                                                            \
    {                                                                                             \
        ++rtest::Checks();                                                                        \
        double _a = double(a), _b = double(b);                                                    \
        if (std::fabs(_a - _b) > (eps))                                                           \
            RT_FAIL("CHECK_NEAR failed: " #a " ~ " #b " (got " << _a << " vs " << _b << ")");    \
    } while (0)

#define CHECK_CONTAINS(hay, needle)                                                                     \
    do                                                                                                  \
    {                                                                                                   \
        ++rtest::Checks();                                                                              \
        std::string const _h = (hay);                                                                   \
        std::string const _n = (needle);                                                                \
        if (_h.find(_n) == std::string::npos)                                                           \
            RT_FAIL("CHECK_CONTAINS failed: '" << _n << "' not found in '" << _h.substr(0, 6000) << "'"); \
    } while (0)

#define CHECK_NOT_CONTAINS(hay, needle)                                                      \
    do                                                                                       \
    {                                                                                        \
        ++rtest::Checks();                                                                   \
        std::string const _h = (hay);                                                        \
        std::string const _n = (needle);                                                     \
        if (_h.find(_n) != std::string::npos)                                                \
            RT_FAIL("CHECK_NOT_CONTAINS failed: '" << _n << "' found in '" << _h.substr(0, 6000) << "'"); \
    } while (0)

#endif
