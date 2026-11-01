/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "TestFramework.h"

#include <cstring>

int main(int argc, char** argv)
{
    std::string filter = argc > 1 ? argv[1] : "";
    int passed = 0, failed = 0;
    for (auto const& t : rtest::Registry())
    {
        std::string full = t.suite + "." + t.name;
        if (!filter.empty() && full.find(filter) == std::string::npos)
            continue;
        try
        {
            t.fn();
            ++passed;
            std::cout << "[ PASS ] " << full << "\n";
        }
        catch (rtest::Failure const& f)
        {
            ++failed;
            std::cout << "[ FAIL ] " << full << "\n         " << f.message << "\n";
        }
        catch (std::exception const& e)
        {
            ++failed;
            std::cout << "[ FAIL ] " << full << "\n         exception: " << e.what() << "\n";
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, " << rtest::Checks() << " checks\n";
    return failed ? 1 : 0;
}
