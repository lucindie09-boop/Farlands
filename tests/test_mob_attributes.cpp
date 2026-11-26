#include "doctest.h"

#include "core/mob_attributes.hpp"

#include <fstream>
#include <sstream>
#include <string>

using namespace VoxelEngine;

namespace {

// The shipped file, read the way the startup load reads it: the parse is what is under
// test, not the file open, so this is the only place a path appears.
std::string read_data_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// A file written the way data/mobs.json writes one: a full entry, a partial one (the fields
// it leaves out keep their defaults), and a `_comment` beside both.
const char* kMobs = R"({
  "_comment": "not a mob",
  "mobs": {
    "_comment": "not a mob either",
    "goblin": { "health": 12, "damage": 2, "speed": 0.30,
                "follow_range": 35, "alert_box": 32, "anger_window": [400, 799] },
    "skeleton": { "health": 20, "speed": 0.25 }
  }
})";

} // namespace

TEST_CASE("mob attributes read every field, and default what a file leaves out") {
    MobRegistry& registry = MobRegistry::get_instance();
    std::string error;
    CHECK(registry.load_from_json(kMobs, &error));
    CHECK(error.empty());
    CHECK(registry.mob_count() == 2);

    const MobAttributes* goblin = registry.find("goblin");
    CHECK(goblin != nullptr);
    if (goblin != nullptr) {
        CHECK(goblin->name == "goblin");
        CHECK(goblin->health == doctest::Approx(12.0f));
        CHECK(goblin->damage == doctest::Approx(2.0f));
        CHECK(goblin->speed == doctest::Approx(0.30f));
        CHECK(goblin->follow_range == doctest::Approx(35.0f));
        CHECK(goblin->alert_box == doctest::Approx(32.0f));
        CHECK(goblin->anger_min_ticks == 400);
        CHECK(goblin->anger_max_ticks == 799);
    }

    // A partial entry is a mob that changed what it had to: the rest is the struct's own
    // default, which is what makes a new entry two lines instead of seven.
    const MobAttributes* skeleton = registry.find("skeleton");
    CHECK(skeleton != nullptr);
    if (skeleton != nullptr) {
        CHECK(skeleton->health == doctest::Approx(20.0f));
        CHECK(skeleton->speed == doctest::Approx(0.25f));
        CHECK(skeleton->damage == doctest::Approx(MobAttributes{}.damage));
        CHECK(skeleton->follow_range == doctest::Approx(MobAttributes{}.follow_range));
        CHECK(skeleton->alert_box == doctest::Approx(MobAttributes{}.alert_box));
        CHECK(skeleton->anger_min_ticks == MobAttributes{}.anger_min_ticks);
        CHECK(skeleton->anger_max_ticks == MobAttributes{}.anger_max_ticks);
    }

    // A comment key is not a mob, and an unknown name is nothing at all: the walker's own
    // constants are the fallback, so a typo here must not invent a body.
    CHECK(registry.find("_comment") == nullptr);
    CHECK(registry.find("dragon") == nullptr);
}

TEST_CASE("a mob entry that cannot be a body is refused, naming the field") {
    MobRegistry& registry = MobRegistry::get_instance();
    std::string error;
    CHECK(registry.load_from_json(kMobs, &error));

    // Each of these parses as JSON and describes something that is not a mob. The error has
    // to name the field, because a data file is edited by hand and "mobs.json is bad" is not
    // an answer.
    auto refuses = [&](const char* text, const char* field) {
        std::string why;
        const bool loaded = registry.load_from_json(text, &why);
        CHECK_FALSE(loaded);
        CHECK(why.find(field) != std::string::npos);
        // ...and the goblin it loaded before is still a goblin: a broken edit is a mistake to
        // fix, not a reason for every body in the game to lose its numbers.
        const MobAttributes* goblin = registry.find("goblin");
        CHECK(goblin != nullptr);
        if (goblin != nullptr) CHECK(goblin->health == doctest::Approx(12.0f));
    };

    refuses(R"({"mobs": {"ghost": {"health": 0}}})", "health");
    refuses(R"({"mobs": {"ghost": {"damage": -1}}})", "damage");
    refuses(R"({"mobs": {"ghost": {"speed": 0}}})", "speed");
    refuses(R"({"mobs": {"ghost": {"follow_range": 0}}})", "follow_range");
    refuses(R"({"mobs": {"ghost": {"alert_box": -2}}})", "alert_box");
    refuses(R"({"mobs": {"ghost": {"anger_window": [900, 400]}}})", "anger_window");
    refuses(R"({"mobs": {"ghost": {"anger_window": "soon"}}})", "anger_window");
    refuses(R"({"mobs": {"ghost": {"health": "lots"}}})", "health");
    refuses(R"({"mobs": []})", "mobs");
    refuses(R"({"tables": {}})", "mobs");
    refuses("not json", "JSON");
}

TEST_CASE("the shipped data/mobs.json describes the goblin the script was tuned with") {
    // The file is the contract: these are the numbers scripts/goblin.gd used as constants
    // (MAX_HEALTH, MOB_ATTACK_DAMAGE, MOVEMENT_SPEED, CHASE_LEASH, ALERT_RADIUS and the
    // ANGER_* pair). Tuning one is a diff here, and this test is what makes the diff a
    // deliberate one rather than a silent behaviour change.
    const std::string text = read_data_file("data/mobs.json");
    CHECK_FALSE(text.empty());
    std::string error;
    MobRegistry& registry = MobRegistry::get_instance();
    CHECK(registry.load_from_json(text, &error));
    CHECK(error.empty());

    const MobAttributes* goblin = registry.find("goblin");
    CHECK(goblin != nullptr);
    if (goblin == nullptr) return;
    CHECK(goblin->health == doctest::Approx(12.0f));         // six hearts
    CHECK(goblin->damage == doctest::Approx(2.0f));          // one half-heart a blow
    CHECK(goblin->speed == doctest::Approx(0.30f));          // a spider's pace
    CHECK(goblin->follow_range == doctest::Approx(35.0f));   // a zombie's follow range
    CHECK(goblin->alert_box == doctest::Approx(32.0f));      // a pigman's shout
    CHECK(goblin->anger_min_ticks == 400);                   // twenty seconds...
    CHECK(goblin->anger_max_ticks == 799);                   // ...to forty
}
