/*
 * testMenuModel.cpp -- what the menu bar offers, without a menu bar.
 *
 * A menu is mostly rules about state: what is available with no cartridge
 * loaded, what carries a tick, what is listed but not built yet. None of that
 * needs a window, and none of it is visible from looking at one -- a greyed
 * item and a missing item look the same in a screenshot.
 */

#include "../src/frontend/MenuModel.h"

#include <doctest/doctest.h>

#include <string>

using namespace nesfe;

namespace {

const MenuItem* find(const std::vector<MenuSection>& bar, int action) {
	for (std::size_t s = 0; s < bar.size(); s++)
		for (std::size_t i = 0; i < bar[s].items.size(); i++)
			if (!bar[s].items[i].separator && bar[s].items[i].action == action)
				return &bar[s].items[i];
	return nullptr;
}

MenuState loaded() {
	MenuState state;
	state.romLoaded = true;
	state.canPickFile = true;
	return state;
}

} // namespace

TEST_CASE("the_bar_has_the_four_sections_in_order") {
	const std::vector<MenuSection> bar = buildMenu(MenuState());
	REQUIRE_EQ(bar.size(), 4u);
	CHECK_EQ(bar[0].label, "&Emulation");
	CHECK_EQ(bar[1].label, "&State");
	CHECK_EQ(bar[2].label, "&Settings");
	CHECK_EQ(bar[3].label, "&Help");
}

TEST_CASE("nothing_that_needs_a_cartridge_is_offered_without_one") {
	const std::vector<MenuSection> bar = buildMenu(MenuState());
	CHECK_FALSE(find(bar, MENU_RESET)->enabled);
	CHECK_FALSE(find(bar, MENU_HARD_RESET)->enabled);
	CHECK_FALSE(find(bar, MENU_PAUSE)->enabled);
	CHECK_FALSE(find(bar, MENU_SCREENSHOT)->enabled);

	// These do not need one. Muting and quitting work on an empty machine, and
	// so does the chooser -- which is the whole reason --settings exists.
	CHECK(find(bar, MENU_MUTE)->enabled);
	CHECK(find(bar, MENU_EXIT)->enabled);
	CHECK(find(bar, MENU_CONFIGURE_PLUGINS)->enabled);
}

TEST_CASE("a_cartridge_turns_the_console_items_on") {
	const std::vector<MenuSection> bar = buildMenu(loaded());
	CHECK(find(bar, MENU_RESET)->enabled);
	CHECK(find(bar, MENU_HARD_RESET)->enabled);
	CHECK(find(bar, MENU_PAUSE)->enabled);
	CHECK(find(bar, MENU_SCREENSHOT)->enabled);
}

TEST_CASE("frame_advance_is_only_offered_while_paused") {
	// It does nothing when the emulator is running, and an item that does
	// nothing is worse than one that is visibly unavailable.
	MenuState state = loaded();
	CHECK_FALSE(buildMenu(state).empty());
	CHECK_FALSE(find(buildMenu(state), MENU_FRAME_ADVANCE)->enabled);

	state.paused = true;
	CHECK(find(buildMenu(state), MENU_FRAME_ADVANCE)->enabled);
}

TEST_CASE("the_toggles_show_the_state_they_toggle") {
	MenuState state = loaded();
	CHECK_FALSE(find(buildMenu(state), MENU_PAUSE)->checked);
	CHECK_FALSE(find(buildMenu(state), MENU_MUTE)->checked);

	state.paused = true;
	state.muted = true;
	CHECK(find(buildMenu(state), MENU_PAUSE)->checked);
	CHECK(find(buildMenu(state), MENU_MUTE)->checked);
}

TEST_CASE("a_plugin_with_no_dialog_is_not_offered_one") {
	MenuState state = loaded();
	CHECK_FALSE(find(buildMenu(state), MENU_CONFIGURE_VIDEO)->enabled);
	CHECK_FALSE(find(buildMenu(state), MENU_CONFIGURE_AUDIO)->enabled);
	CHECK_FALSE(find(buildMenu(state), MENU_CONFIGURE_INPUT)->enabled);

	state.videoConfigurable = true;
	state.inputConfigurable = true;
	CHECK(find(buildMenu(state), MENU_CONFIGURE_VIDEO)->enabled);
	CHECK_FALSE(find(buildMenu(state), MENU_CONFIGURE_AUDIO)->enabled);
	CHECK(find(buildMenu(state), MENU_CONFIGURE_INPUT)->enabled);
}

TEST_CASE("what_is_not_built_yet_says_so_and_is_disabled") {
	// The two must go together. An item that is enabled and does nothing is a
	// bug report; one that is disabled without being marked is a mystery to
	// whoever reads this next.
	const std::vector<MenuSection> bar = buildMenu(loaded());
	const int notYet[] = {
		MENU_SAVE_STATE, MENU_LOAD_STATE,
		MENU_NEXT_SLOT, MENU_PREV_SLOT, MENU_HOTKEYS
	};
	for (std::size_t i = 0; i < sizeof(notYet) / sizeof(notYet[0]); i++) {
		CAPTURE(notYet[i]);
		const MenuItem* item = find(bar, notYet[i]);
		REQUIRE(item != nullptr);
		CHECK(item->unimplemented);
		CHECK_FALSE(item->enabled);
	}
}

TEST_CASE("everything_that_is_enabled_is_implemented") {
	// The other direction, which is the one that catches a mistake: marking
	// something unimplemented and leaving it clickable.
	const std::vector<MenuSection> bar = buildMenu(loaded());
	for (std::size_t s = 0; s < bar.size(); s++)
		for (std::size_t i = 0; i < bar[s].items.size(); i++) {
			const MenuItem& item = bar[s].items[i];
			if (item.separator || !item.enabled)
				continue;
			CAPTURE(item.label);
			CHECK_FALSE(item.unimplemented);
		}
}

TEST_CASE("every_slot_is_listed_and_exactly_one_is_current") {
	MenuState state = loaded();
	state.saveSlot = 2;
	const std::vector<MenuSection> bar = buildMenu(state);

	int ticked = 0;
	for (int i = 0; i < MENU_SLOT_COUNT; i++) {
		const MenuItem* item = find(bar, slotAction(i));
		REQUIRE(item != nullptr);
		CHECK(item->radio);
		if (item->checked)
			ticked++;
	}
	CHECK_EQ(ticked, 1);
	CHECK(find(bar, slotAction(2))->checked);
}

TEST_CASE("an_empty_slot_says_empty_and_a_written_one_says_when") {
	MenuState state = loaded();
	state.slotLabels.push_back("2026-08-10 14:32");
	const std::vector<MenuSection> bar = buildMenu(state);
	CHECK(find(bar, slotAction(0))->label.find("2026-08-10 14:32") != std::string::npos);
	CHECK(find(bar, slotAction(1))->label.find("(empty)") != std::string::npos);
}

TEST_CASE("recent_roms_appear_once_there_are_any") {
	MenuState state = loaded();
	CHECK(find(buildMenu(state), MENU_RECENT_FIRST)->unimplemented);

	state.recentRoms.push_back("Super Mario Bros");
	state.recentRoms.push_back("Zelda");
	const std::vector<MenuSection> bar = buildMenu(state);
	REQUIRE(find(bar, recentAction(0)) != nullptr);
	CHECK_EQ(find(bar, recentAction(0))->label, "Super Mario Bros");
	CHECK_EQ(find(bar, recentAction(1))->label, "Zelda");
	CHECK(find(bar, recentAction(0))->enabled);
}

TEST_CASE("the_numbered_families_do_not_collide_with_the_named_actions") {
	// The reason they live past MENU_ACTION_COUNT: an enumerator plus an index
	// is a trap in an enum that is still being added to.
	CHECK(MENU_SLOT_FIRST > MENU_ACTION_COUNT);
	CHECK(MENU_RECENT_FIRST > MENU_SLOT_FIRST + MENU_SLOT_COUNT);

	CHECK_EQ(slotForAction(slotAction(3)), 3);
	CHECK_EQ(slotForAction(MENU_RESET), -1);
	CHECK_EQ(recentForAction(recentAction(5)), 5);
	CHECK_EQ(recentForAction(MENU_ABOUT), -1);
}

TEST_CASE("loading_needs_a_file_picker_and_closing_needs_a_cartridge") {
	MenuState state;                 // nothing loaded, no picker
	CHECK_FALSE(find(buildMenu(state), MENU_LOAD_ROM)->enabled);
	CHECK_FALSE(find(buildMenu(state), MENU_CLOSE_ROM)->enabled);

	state.canPickFile = true;
	CHECK(find(buildMenu(state), MENU_LOAD_ROM)->enabled);
	// Still nothing to close.
	CHECK_FALSE(find(buildMenu(state), MENU_CLOSE_ROM)->enabled);

	state.romLoaded = true;
	CHECK(find(buildMenu(state), MENU_CLOSE_ROM)->enabled);
}
