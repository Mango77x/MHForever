// Tests how the notes played are matched against a hunting horn's songs (host/horn_songs.hpp), with made-up songs in the
// game's layout (no game data): the newest note first, 0 past the first note, 0x80 | colour for "not that colour".

#include "horn_songs.hpp"

#include "check.hpp"

#include <vector>

using namespace mhf::mhfu;

int main() {
    // Playing order is the reverse of the stored order; "not" bytes and the zeros after the first note are not notes.
    MHF_CHECK((song_notes_in_order({4, 6, 0, 0}) == std::vector<std::uint8_t>{6, 4}));
    MHF_CHECK((song_notes_in_order({3, 3, 0x83, 0}) == std::vector<std::uint8_t>{3, 3}));
    MHF_CHECK((song_notes_in_order({2, 7, 7, 5}) == std::vector<std::uint8_t>{5, 7, 7, 2}));

    // A two-note song (6 then 4): nothing played, its first note, then both.
    const SongNotes two{4, 6, 0, 0};
    MHF_CHECK(song_progress(two, {}).played == 0 && !song_progress(two, {}).ready);
    MHF_CHECK(song_progress(two, {6}).played == 1 && !song_progress(two, {6}).ready);
    MHF_CHECK(song_progress(two, {4, 6}).ready);
    MHF_CHECK(song_progress(two, {4, 6, 1, 2, 3}).ready); // older notes do not matter
    MHF_CHECK(!song_progress(two, {6, 4}).ready && song_progress(two, {6, 4}).played == 1);

    // A four-note song (5, 7, 7, 2): progress note by note, and a wrong note starts it over.
    const SongNotes four{2, 7, 7, 5};
    MHF_CHECK(song_progress(four, {5}).played == 1);
    MHF_CHECK(song_progress(four, {7, 5}).played == 2);
    MHF_CHECK(song_progress(four, {7, 7, 5}).played == 3);
    MHF_CHECK(song_progress(four, {2, 7, 7, 5}).ready);
    MHF_CHECK(song_progress(four, {1, 7, 7, 5}).played == 0 && !song_progress(four, {1, 7, 7, 5}).ready);

    // A song of one note twice, with "not 3" before them: a third 3 does not play it again.
    const SongNotes twice{3, 3, 0x83, 0};
    MHF_CHECK(song_progress(twice, {3, 3}).ready);
    MHF_CHECK(song_progress(twice, {3, 3, 1}).ready);
    MHF_CHECK(!song_progress(twice, {3, 3, 3}).ready);
    MHF_CHECK(song_progress(twice, {3, 3, 3}).played == 1);

    return mhf::test::finish("test_horn_songs");
}
