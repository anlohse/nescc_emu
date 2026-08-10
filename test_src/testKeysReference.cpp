/*
 * testKeysReference.cpp -- the help page, without the window.
 *
 * There is one thing worth testing about a page that lists what your keys do:
 * that it lists what your keys do. A reference built from a table written by
 * hand drifts from the configuration the moment somebody rebinds something, and
 * then it is not merely useless -- it is telling a person something untrue.
 */

#include "../src/frontend/KeysReference.h"

#include <doctest/doctest.h>

#include <string>

using nesfe::keysReferenceText;
using nesgui::Config;

namespace {

bool mentions(const std::string& text, const std::string& what) {
	return text.find(what) != std::string::npos;
}

} // namespace

TEST_CASE("the_page_describes_the_default_bindings") {
	const std::string text = keysReferenceText(Config::defaults());

	CHECK(mentions(text, "Player 1"));
	CHECK(mentions(text, "Player 2"));
	// Z is player one's A by default, and SDL is asked for the name rather than
	// one being written here, so what is printed is what the file will contain.
	CHECK(mentions(text, "Z"));
	CHECK(mentions(text, "Right Shift"));
	CHECK(mentions(text, "Keypad 8"));
}

TEST_CASE("rebinding_a_key_changes_what_the_page_says") {
	// The whole point. A page that says "Z" after somebody has moved A to Q is
	// worse than no page.
	Config c = Config::defaults();
	const std::string before = keysReferenceText(c);
	REQUIRE(mentions(before, "Z"));

	c.keys[0][0] = SDL_SCANCODE_Q;
	const std::string after = keysReferenceText(c);
	CHECK(mentions(after, "Q"));
	CHECK(after != before);
}

TEST_CASE("an_unbound_button_says_so_rather_than_showing_a_gap") {
	Config c = Config::defaults();
	c.keys[1][3] = SDL_SCANCODE_UNKNOWN;
	const std::string text = keysReferenceText(c);
	CHECK(mentions(text, "(not bound)"));
}

TEST_CASE("the_pad_bindings_are_described_too") {
	const std::string text = keysReferenceText(Config::defaults());
	CHECK(mentions(text, "gamepad"));
	// SDL's own names, which are also what nes.cfg uses.
	CHECK(mentions(text, "dpup"));
	CHECK(mentions(text, "start"));
}

TEST_CASE("the_fixed_keys_are_listed_with_what_they_do") {
	const std::string text = keysReferenceText(Config::defaults());
	CHECK(mentions(text, "pause"));
	CHECK(mentions(text, "mute"));
	CHECK(mentions(text, "run unthrottled"));
	CHECK(mentions(text, "reset"));
	CHECK(mentions(text, "screenshot"));
	CHECK(mentions(text, "quit"));

	// And the menu accelerators, which are the newest and least guessable.
	CHECK(mentions(text, "F1"));
	CHECK(mentions(text, "Ctrl+O"));
	CHECK(mentions(text, "F5"));
	CHECK(mentions(text, "slot"));
}

TEST_CASE("the_page_says_where_the_bindings_live") {
	// Somebody reading this wants to know how to change what it describes.
	const std::string text = keysReferenceText(Config::defaults());
	CHECK(mentions(text, "nes.cfg"));
	CHECK(mentions(text, "dialog"));
}

TEST_CASE("every_button_of_both_ports_appears_twice") {
	// Once for the keyboard and once for the pad, for each of two ports: four
	// blocks of eight. A missing block is easy to introduce and invisible in a
	// screenshot of the top of the page.
	const std::string text = keysReferenceText(Config::defaults());
	const char* buttons[8] = { "A", "B", "Select", "Start",
			"Up", "Down", "Left", "Right" };
	for (int i = 0; i < 8; i++) {
		CAPTURE(buttons[i]);
		std::size_t count = 0;
		std::size_t at = 0;
		const std::string labelled = std::string("  ") + buttons[i];
		while ((at = text.find(labelled, at)) != std::string::npos) {
			count++;
			at += labelled.size();
		}
		CHECK(count >= 4u);
	}
}
