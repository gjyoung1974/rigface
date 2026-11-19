#include "radio.h"

#include <algorithm>
#include <chrono>
#include <cctype>

namespace rigface {

const char *mode_name(Mode m)
{
    switch (m) {
    case Mode::LSB: return "LSB";
    case Mode::USB: return "USB";
    case Mode::CW: return "CW";
    case Mode::AM: return "AM";
    case Mode::FM: return "FM";
    }
    return "USB";
}

std::optional<Mode> mode_from(const std::string &name)
{
    for (Mode m : MODES)
        if (name == mode_name(m))
            return m;
    return std::nullopt;
}

double monotonic()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool Caps::has_mode(Mode m) const
{
    return std::find(modes.begin(), modes.end(), m) != modes.end();
}

RadioAddr parse_radio(const std::string &addr)
{
    std::string low = addr;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (low == "qmx")
        return {true, ""};
    if (low.rfind("qmx:", 0) == 0) {
        std::string t = addr.substr(4);
        size_t a = t.find_first_not_of(" \t\r\n");
        size_t b = t.find_last_not_of(" \t\r\n");
        return {true, a == std::string::npos ? "" : t.substr(a, b - a + 1)};
    }
    if (addr.rfind("/dev/", 0) == 0)
        return {true, addr};
    return {false, addr};
}

}  // namespace rigface
