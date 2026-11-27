#include "core/ClueReciprocityPolicy.h"

#include <cassert>

int main()
{
    using namespace asst::clue_reciprocity;
    const std::vector<Gift> gifts { { "Doctor#1234", {} } };
    const std::vector<Recipient> recipients {
        { "Online#5678", true, true },
        { "Doctor#1234", true, false },
        { "Doctor#7777", true, true },
    };
    assert(ClueReciprocityPolicy::recipient(recipients, gifts, 9, true) == 1);
    assert(ClueReciprocityPolicy::recipient(recipients, gifts, 3, true) == 1);

    const std::vector<Recipient> without_sender {
        { "Doctor#1234", false, true },
        { "Yesterday#6789", true, false },
        { "Online#5678", true, true },
    };
    assert(!ClueReciprocityPolicy::recipient(without_sender, gifts, 8, true));
    assert(!ClueReciprocityPolicy::recipient(without_sender, gifts, 9, false));
    assert(ClueReciprocityPolicy::recipient(without_sender, gifts, 9, true) == 2);
    assert(!ClueReciprocityPolicy::same_player("Doctor#1234", "Doctor#7777"));
}
