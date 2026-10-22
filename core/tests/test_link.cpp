#include "link/link.hpp"

#include "check.hpp"

#include <chrono>
#include <string>
#include <thread>

namespace {

using namespace mhf::link;

} // namespace

int main() {
    auto server = Server::start();
    MHF_CHECK(server != nullptr);
    if (!server) return mhf::test::finish("test_link");
    MHF_CHECK(server->port() != 0 && server->token().size() == 64);

    // A program that does not know the token is dropped; the real game right after it still gets through.
    {
        const std::string wrong = std::to_string(server->port()) + ":not-the-token";
        std::unique_ptr<Connection> game;
        std::thread client([&] {
            auto intruder = connect(wrong);
            MHF_CHECK(intruder != nullptr); // the connection itself works, the greeting is what fails
            game = connect(server->address());
        });
        auto launcher = server->accept(5000);
        client.join();
        MHF_CHECK(launcher != nullptr && game != nullptr);
        if (!launcher || !game) return mhf::test::finish("test_link");

        // Lines go both ways, in order, whole.
        MHF_CHECK(game->send("menu 10 20 800 600"));
        MHF_CHECK(game->send("state village"));
        MHF_CHECK(launcher->receive(2000) == std::optional<std::string>("menu 10 20 800 600"));
        MHF_CHECK(launcher->receive(2000) == std::optional<std::string>("state village"));
        MHF_CHECK(!launcher->receive(0).has_value()); // nothing else: does not wait
        MHF_CHECK(launcher->send("set music 40"));
        MHF_CHECK(game->receive(2000) == std::optional<std::string>("set music 40"));

        // Lines that cannot be framed are refused.
        MHF_CHECK(!game->send(""));
        MHF_CHECK(!game->send("two\nlines"));
        MHF_CHECK(!game->send(std::string(kMaxLine + 1, 'x')));

        // The other end going away is noticed, after the lines that were already on the way.
        MHF_CHECK(launcher->send("quit"));
        launcher.reset();
        MHF_CHECK(game->receive(2000) == std::optional<std::string>("quit"));
        MHF_CHECK(!game->receive(2000).has_value());
        MHF_CHECK(!game->open());
        MHF_CHECK(!game->send("late"));
    }

    // Nobody connects: accept gives up on time.
    const auto before = std::chrono::steady_clock::now();
    MHF_CHECK(server->accept(100) == nullptr);
    MHF_CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(5));

    // Bad addresses are refused without trying.
    MHF_CHECK(connect("") == nullptr);
    MHF_CHECK(connect("abc") == nullptr);
    MHF_CHECK(connect("99999:x") == nullptr);
    MHF_CHECK(connect("12x:token") == nullptr);

    return mhf::test::finish("test_link");
}
