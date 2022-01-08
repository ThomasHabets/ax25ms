#include "fcs.h"

#include <iostream>
#include <sstream>

int wrapmain(int argc, char** argv)
{
    std::stringstream ss;
    ss << std::cin.rdbuf();
    const auto fcs = ax25ms::fcs(ss.str());
    std::cout << ss.str() << fcs.first << fcs.second;
    return 0;
}
