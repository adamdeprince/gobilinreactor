#include "../uml/disk-backup.h"
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        if (std::string(argv[1]) == "previous") { std::cout << goblin_disk::restorePrevious(argv[2], argv[3]) << '\n'; return 0; }
        bool restore = std::string(argv[1]) == "import";
        goblin_disk::File stream(open(argv[3], restore ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC, 0600));
        if (restore) std::cout << goblin_disk::importDisk(argv[2], stream.fd) << '\n';
        else goblin_disk::exportDisk(argv[2], stream.fd);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
