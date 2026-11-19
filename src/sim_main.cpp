// `rigface-sim`: the simulated radios on their own.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "sim.h"

static void usage(FILE *to)
{
    std::fputs("usage: rigface-sim [-h] [--port PORT] [--host HOST] [--qmx] [--iq-port IQ_PORT]\n"
               "\n"
               "Simulated Radioberry (HPSDR Protocol 1)\n"
               "\n"
               "options:\n"
               "  -h, --help         show this help message and exit\n"
               "  --port PORT        UDP port to listen on (default 1024)\n"
               "  --host HOST        address to listen on (default 0.0.0.0)\n"
               "  --qmx              simulate a QRP Labs QMX instead\n"
               "  --iq-port IQ_PORT  with --qmx: local UDP port the I/Q is sent to (default 7355)\n",
               to);
}

int main(int argc, char **argv)
{
    int port = 1024, iq_port = 7355, parent = 0;
    std::string host = "0.0.0.0";
    bool qmx = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i], value;
        bool has_value = false;
        if (a.rfind("--", 0) == 0 && a.find('=') != std::string::npos) {
            value = a.substr(a.find('=') + 1);
            a.erase(a.find('='));
            has_value = true;
        }
        auto arg = [&]() -> std::string {
            if (has_value)
                return value;
            if (i + 1 >= argc) {
                std::fprintf(stderr, "rigface-sim: %s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage(stdout);
            return 0;
        } else if (a == "--port") {
            port = std::atoi(arg().c_str());
        } else if (a == "--host") {
            host = arg();
        } else if (a == "--qmx") {
            qmx = true;
        } else if (a == "--iq-port") {
            iq_port = std::atoi(arg().c_str());
        } else if (a == "--parent") {          // set by the panel: exit when it is gone
            parent = std::atoi(arg().c_str());
        } else {
            std::fprintf(stderr, "rigface-sim: unknown argument %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }
    if (qmx)
        return rigface::sim::serve_qmx(iq_port, parent);
    return rigface::sim::serve(port, host, true, parent);
}
