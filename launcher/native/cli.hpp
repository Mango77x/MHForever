#pragma once

// The launcher's command-line modes: `--install`, `--list`, `--set`, `--play`, `--update`, `--version`. They run without a
// window, so the window program (MHForever) hands `--install` and `--update` to them, and MHForever-cli, a program that
// links no web view (and, on Linux, no GTK), offers all of them. The Steam Deck's SteamOS has no WebKitGTK: there the
// launcher's window cannot even start, and this program installs a copy, sets it up and starts the game.

#include <string>
#include <vector>

namespace mhf::launcher::cli {

// True for the first argument of a command-line mode.
bool handles(const std::string &first_argument);

// Runs the mode named by args[1]; returns the program's exit code.
int run(const std::vector<std::string> &args);

} // namespace mhf::launcher::cli
