/*
 * testBindingModel.cpp -- the controller bindings table, without the dialog.
 *
 * The behaviour a rebinding screen has to get right is all here: what a
 * binding reads as, what happens to the key that already had it, what Cancel
 * means, and what the dialog is allowed to write back. Also the Windows key
 * mapping, which is the one piece that has to be right for keyboards this
 * machine does not have.
 */

#include "../src/frontend/BindingModel.h"
#include "../src/frontend/BindingsDialog.h"

#include <doctest/doctest.h>

#include <string>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

using namespace nesfe;

TEST_CASE("a_binding_reads_back_as_sdl_names_it") {
	// The same names the file uses, because SDL converts both directions --
	// a second naming scheme would drift out of step and get the odd keys
	// wrong, which is the whole reason the config format uses SDL's.
	nesgui::Config config = nesgui::Config::defaults();
	BindingModel model(config);

	CHECK_EQ(model.bindingName(BindingModel::KEYBOARD_1, 0), "Z");
	CHECK_EQ(model.bindingName(BindingModel::KEYBOARD_1, 2), "Right Shift");
	CHECK_EQ(model.bindingName(BindingModel::KEYBOARD_2, 4), "Keypad 8");
	CHECK_EQ(model.bindingName(BindingModel::PAD_1, 0), "a");
	CHECK_EQ(model.bindingName(BindingModel::PAD_1, 3), "start");
}

TEST_CASE("an_unbound_button_says_so_rather_than_showing_nothing") {
	nesgui::Config config = nesgui::Config::defaults();
	BindingModel model(config);

	model.clear(BindingModel::KEYBOARD_1, 0);
	CHECK_FALSE(model.isBound(BindingModel::KEYBOARD_1, 0));
	CHECK_EQ(model.bindingName(BindingModel::KEYBOARD_1, 0), "(none)");
	CHECK(model.changed());
}

TEST_CASE("binding_a_key_takes_it_from_whatever_had_it") {
	// One key driving two NES buttons is never what anyone meant, and finding
	// out while playing is worse than watching the old binding disappear.
	nesgui::Config config = nesgui::Config::defaults();
	BindingModel model(config);
	REQUIRE_EQ(model.bindingName(BindingModel::KEYBOARD_1, 1), "X");

	model.bindKey(BindingModel::KEYBOARD_1, 0, SDL_SCANCODE_X);
	CHECK_EQ(model.bindingName(BindingModel::KEYBOARD_1, 0), "X");
	CHECK_EQ(model.bindingName(BindingModel::KEYBOARD_1, 1), "(none)");
}

TEST_CASE("the_two_ports_are_separate_controllers") {
	// Player 2 keeping a key player 1 also uses is odd but legal, and not this
	// dialog's business to prevent: they are different controllers.
	nesgui::Config config = nesgui::Config::defaults();
	BindingModel model(config);

	model.bindKey(BindingModel::KEYBOARD_2, 0, SDL_SCANCODE_Z);
	CHECK_EQ(model.bindingName(BindingModel::KEYBOARD_2, 0), "Z");
	CHECK_EQ(model.bindingName(BindingModel::KEYBOARD_1, 0), "Z");   // untouched
}

TEST_CASE("a_pad_binding_cannot_be_set_with_a_key_or_the_other_way_round") {
	nesgui::Config config = nesgui::Config::defaults();
	BindingModel model(config);

	model.bindKey(BindingModel::PAD_1, 0, SDL_SCANCODE_Q);
	CHECK_EQ(model.bindingName(BindingModel::PAD_1, 0), "a");        // unchanged
	model.bindPad(BindingModel::KEYBOARD_1, 0, SDL_CONTROLLER_BUTTON_Y);
	CHECK_EQ(model.bindingName(BindingModel::KEYBOARD_1, 0), "Z");   // unchanged
	CHECK_FALSE(model.changed());
}

TEST_CASE("rebinding_a_button_to_what_it_already_had_is_not_a_change") {
	nesgui::Config config = nesgui::Config::defaults();
	BindingModel model(config);
	model.bindKey(BindingModel::KEYBOARD_1, 0, SDL_SCANCODE_Z);
	CHECK_FALSE(model.changed());
}

TEST_CASE("defaults_put_everything_back") {
	nesgui::Config config = nesgui::Config::defaults();
	config.keys[0][0] = SDL_SCANCODE_Q;
	config.padButtons[1][3] = SDL_CONTROLLER_BUTTON_X;
	BindingModel model(config);

	model.restoreDefaults();
	CHECK_EQ(model.bindingName(BindingModel::KEYBOARD_1, 0), "Z");
	CHECK_EQ(model.bindingName(BindingModel::PAD_2, 3), "start");
	CHECK(model.changed());
}

TEST_CASE("applying_writes_only_the_bindings") {
	// The file also holds the window scale and which plugins to use, and none
	// of that belongs to a controller dialog.
	nesgui::Config config = nesgui::Config::defaults();
	config.scale = 6;
	config.videoPlugin = "some-video";
	config.audioPlugin = "some-audio";

	BindingModel model(config);
	model.bindKey(BindingModel::KEYBOARD_1, 0, SDL_SCANCODE_Q);
	model.apply(&config);

	CHECK_EQ(config.keys[0][0], SDL_SCANCODE_Q);
	CHECK_EQ(config.scale, 6);
	CHECK_EQ(config.videoPlugin, "some-video");
	CHECK_EQ(config.audioPlugin, "some-audio");
}

TEST_CASE("a_model_that_is_not_applied_leaves_the_configuration_alone") {
	// What Cancel means, and the reason the model holds its own copy.
	nesgui::Config config = nesgui::Config::defaults();
	BindingModel model(config);

	model.bindKey(BindingModel::KEYBOARD_1, 0, SDL_SCANCODE_Q);
	model.restoreDefaults();
	model.clear(BindingModel::KEYBOARD_1, 1);

	CHECK_EQ(config.keys[0][0], SDL_SCANCODE_Z);
	CHECK_EQ(config.keys[0][1], SDL_SCANCODE_X);
}

TEST_CASE("the_groups_are_two_keyboards_and_two_pads") {
	CHECK_FALSE(BindingModel::isPad(BindingModel::KEYBOARD_1));
	CHECK_FALSE(BindingModel::isPad(BindingModel::KEYBOARD_2));
	CHECK(BindingModel::isPad(BindingModel::PAD_1));
	CHECK(BindingModel::isPad(BindingModel::PAD_2));
	CHECK_EQ(std::string(BindingModel::buttonLabel(2)), "Select");
	CHECK_EQ(std::string(BindingModel::groupLabel(BindingModel::PAD_2)),
			"Player 2 gamepad");
}

/* ------------------------------------------------------------------------ */
/* Turning a key press into a scancode                                       */
/* ------------------------------------------------------------------------ */

#if defined(_WIN32)

TEST_CASE("named_keys_map_to_the_scancode_sdl_uses") {
	CHECK_EQ(scancodeFromVirtualKey(VK_UP, true), SDL_SCANCODE_UP);
	CHECK_EQ(scancodeFromVirtualKey(VK_ESCAPE, false), SDL_SCANCODE_ESCAPE);
	CHECK_EQ(scancodeFromVirtualKey(VK_SPACE, false), SDL_SCANCODE_SPACE);
	CHECK_EQ(scancodeFromVirtualKey(VK_F12, false), SDL_SCANCODE_F12);
	CHECK_EQ(scancodeFromVirtualKey(VK_NUMPAD8, false), SDL_SCANCODE_KP_8);
	CHECK_EQ(scancodeFromVirtualKey(VK_ADD, false), SDL_SCANCODE_KP_PLUS);
}

TEST_CASE("the_extended_flag_tells_the_paired_keys_apart") {
	// Windows reports one code for both shifts unless asked. The default
	// configuration binds Right Shift specifically, so this has to work or the
	// dialog could not reproduce its own defaults.
	CHECK_EQ(scancodeFromVirtualKey(VK_SHIFT, false), SDL_SCANCODE_LSHIFT);
	CHECK_EQ(scancodeFromVirtualKey(VK_SHIFT, true), SDL_SCANCODE_RSHIFT);
	CHECK_EQ(scancodeFromVirtualKey(VK_CONTROL, false), SDL_SCANCODE_LCTRL);
	CHECK_EQ(scancodeFromVirtualKey(VK_CONTROL, true), SDL_SCANCODE_RCTRL);

	// Return and keypad Enter are the same virtual key, and binding one when
	// the other was pressed would be wrong on both ports of the default map.
	CHECK_EQ(scancodeFromVirtualKey(VK_RETURN, false), SDL_SCANCODE_RETURN);
	CHECK_EQ(scancodeFromVirtualKey(VK_RETURN, true), SDL_SCANCODE_KP_ENTER);
}

TEST_CASE("printable_keys_go_through_the_keyboard_layout") {
	// Not through a table of US positions: on a French or German keyboard the
	// binding has to be the key that was pressed, not the one in that spot on
	// a US board. SDL resolves the character, so the layout decides.
	CHECK_EQ(scancodeFromVirtualKey('Z', false), SDL_GetScancodeFromKey(SDLK_z));
	CHECK_EQ(scancodeFromVirtualKey('X', false), SDL_GetScancodeFromKey(SDLK_x));
	CHECK_EQ(scancodeFromVirtualKey('5', false), SDL_GetScancodeFromKey(SDLK_5));
}

TEST_CASE("a_key_with_no_name_is_refused_rather_than_guessed") {
	// Binding the wrong key silently is worse than saying it cannot be bound.
	CHECK_EQ(scancodeFromVirtualKey(0xFF, false), SDL_SCANCODE_UNKNOWN);
}

#endif // _WIN32
