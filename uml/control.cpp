#include "client.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 4 || (strcmp(argv[2], "read") && strcmp(argv[2], "exec") && strcmp(argv[2], "ports"))) {
        fprintf(stderr, "Usage: goblin-ctl SOCKET {read|exec|ports} PATH_COMMAND_OR_RULES\n"); return 2;
    }
    try {
        return goblin_uml::Request(argv[1], !strcmp(argv[2], "read") ? goblin_uml::ReadFile : !strcmp(argv[2], "ports") ? goblin_uml::ConfigurePorts : goblin_uml::Execute,
            argv[3], [](const char* bytes, size_t size) { fwrite(bytes, 1, size, stdout); fflush(stdout); });
    } catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); return 1; }
}
