/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Wire-format helper used by tests/addon/test_interop.lua to prove that the
 * C++ server codec and the Lua addon codec understand each other.
 *   romance_wire encode < message.json   -> chunk lines
 *   romance_wire decode < chunks.txt     -> canonical JSON
 */

#include "RomanceProtocol.h"

#include <iostream>
#include <iterator>
#include <string>

using namespace romance;

int main(int argc, char** argv)
{
    std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "encode")
    {
        std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        auto j = Json::Parse(input);
        if (!j)
            return 2;
        for (auto const& c : EncodeAddonMessage(*j, 4242))
            std::cout << c << "\n";
        return 0;
    }
    if (mode == "decode")
    {
        AddonReassembler re;
        std::string line;
        while (std::getline(std::cin, line))
        {
            if (line.empty())
                continue;
            auto r = re.Feed(line, 0);
            if (r.status == AddonReassembler::Status::Complete)
            {
                std::cout << r.message.Dump() << "\n";
                return 0;
            }
            if (r.status != AddonReassembler::Status::Incomplete)
            {
                std::cerr << r.error << "\n";
                return 3;
            }
        }
        return 4;
    }
    std::cerr << "usage: romance_wire encode|decode\n";
    return 1;
}
