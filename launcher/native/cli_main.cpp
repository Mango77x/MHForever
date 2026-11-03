// MHForever-cli: the launcher without a window. It links no web view and, on Linux, no GTK, so it starts on the Steam
// Deck's SteamOS, where WebKitGTK is missing and the launcher's window cannot start. See cli.hpp.

#include "cli.hpp"

#include <string>
#include <vector>

int main(int argc, char **argv) {
    return mhf::launcher::cli::run(std::vector<std::string>(argv, argv + argc));
}
