/*
 * testPadMapping.cpp -- the mapping guessed for a pad SDL has never heard of.
 *
 * SDL only presents a device through the game controller API if it has a mapping
 * for it, keyed by GUID, and its table covers the pads somebody thought to add.
 * A generic USB pad is usually not in it, so SDL_IsGameController says no and
 * every list of pads in the emulator comes back empty -- for a pad that works
 * perfectly. The fix is to write a mapping from what the device reports.
 *
 * A guess deserves a test more than a certainty does. What is checked here is
 * not that the names land on the right plastic -- nothing can know that, and the
 * bindings dialog makes it not matter -- but that the mapping is well formed:
 * that no physical control is given two jobs, that the directions come from
 * wherever the pad actually keeps them, and that a pad with few buttons is not
 * handed a Start button it does not have.
 */

#include "../src/frontend/PadMapping.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace nesfe;

namespace {

/** The "control:source," pairs of a mapping, minus the header fields. */
std::vector<std::string> pairs(const std::string& mapping) {
	std::vector<std::string> out;
	std::size_t at = 0;
	int field = 0;
	while (at < mapping.size()) {
		const std::size_t comma = mapping.find(',', at);
		const std::string piece = mapping.substr(at,
				(comma == std::string::npos) ? std::string::npos : comma - at);
		// The first two fields are the GUID and the name, neither of which is a
		// binding; "platform" is a declaration rather than a control.
		if (field >= 2 && !piece.empty() && piece.rfind("platform:", 0) != 0
				&& piece.rfind("crc:", 0) != 0)
			out.push_back(piece);
		field++;
		if (comma == std::string::npos)
			break;
		at = comma + 1;
	}
	return out;
}

std::string sourceOf(const std::string& pair) {
	const std::size_t colon = pair.find(':');
	return (colon == std::string::npos) ? std::string() : pair.substr(colon + 1);
}

std::string targetOf(const std::string& pair) {
	const std::size_t colon = pair.find(':');
	return (colon == std::string::npos) ? pair : pair.substr(0, colon);
}

bool has(const std::string& mapping, const std::string& pair) {
	const std::vector<std::string> all = pairs(mapping);
	for (std::size_t i = 0; i < all.size(); i++)
		if (all[i] == pair)
			return true;
	return false;
}

bool binds(const std::string& mapping, const std::string& target) {
	const std::vector<std::string> all = pairs(mapping);
	for (std::size_t i = 0; i < all.size(); i++)
		if (targetOf(all[i]) == target)
			return true;
	return false;
}

/** No physical control doing two jobs, which is the one unforgivable fault. */
void checkNothingIsBoundTwice(const std::string& mapping) {
	const std::vector<std::string> all = pairs(mapping);
	for (std::size_t i = 0; i < all.size(); i++) {
		for (std::size_t j = i + 1; j < all.size(); j++) {
			CAPTURE(all[i]);
			CAPTURE(all[j]);
			CHECK(sourceOf(all[i]) != sourceOf(all[j]));
			CHECK(targetOf(all[i]) != targetOf(all[j]));
		}
	}
}

} // namespace

TEST_CASE("the_pad_that_started_this_gets_the_mapping_it_should_have_had") {
	// The exact device: vendor 0x081F product 0xE401, reporting itself as nothing
	// more helpful than "USB gamepad". Ten buttons, two axes, and no hat -- so its
	// d-pad is on the axes, which is the part a guess has to get right.
	const std::string m = guessPadMapping(
			"0300cb231f08000001e4000000000000", "USB gamepad", 10, 2, 0);

	// Select and Start on the top two, which is where a pad of this kind puts
	// them, leaving the eight below for everything else.
	CHECK(has(m, "back:b8"));
	CHECK(has(m, "start:b9"));
	CHECK(has(m, "a:b0"));
	CHECK(has(m, "b:b1"));
	CHECK(has(m, "x:b2"));
	CHECK(has(m, "y:b3"));

	// The directions off the axes, both ways on each.
	CHECK(has(m, "dpleft:-a0"));
	CHECK(has(m, "dpright:+a0"));
	CHECK(has(m, "dpup:-a1"));
	CHECK(has(m, "dpdown:+a1"));

	// And *not* also as a stick. The emulator accepts the left stick as a
	// direction already, so naming the same two axes twice would deliver every
	// press twice over.
	CHECK_FALSE(binds(m, "leftx"));
	CHECK_FALSE(binds(m, "lefty"));

	checkNothingIsBoundTwice(m);
}

TEST_CASE("a_pad_with_a_hat_keeps_its_stick_as_a_stick") {
	// The other shape, and the reason the axes are only ever the d-pad when there
	// is nothing else for the d-pad to be. Here the hat is the d-pad, which frees
	// the axes to be what they are.
	const std::string m = guessPadMapping("abc", "Pad", 12, 4, 1);

	CHECK(has(m, "dpup:h0.1"));
	CHECK(has(m, "dpright:h0.2"));
	CHECK(has(m, "dpdown:h0.4"));
	CHECK(has(m, "dpleft:h0.8"));
	CHECK(has(m, "leftx:a0"));
	CHECK(has(m, "lefty:a1"));
	CHECK(has(m, "rightx:a2"));
	CHECK(has(m, "righty:a3"));

	checkNothingIsBoundTwice(m);
}

TEST_CASE("a_small_pad_keeps_its_face_buttons_rather_than_inventing_a_start") {
	// The case that was wrong first time round. Taking Select and Start off the
	// top unconditionally means a four button pad binds Start to the same button
	// as Y -- or worse, on a two button pad, to the same button as A. A control
	// that does two things at once is worse than a control that is missing, since
	// a player can see a missing one and rebind around it.
	for (int buttons = 2; buttons < 6; buttons++) {
		CAPTURE(buttons);
		const std::string m = guessPadMapping("abc", "Tiny", buttons, 2, 0);
		CHECK_FALSE(binds(m, "back"));
		CHECK_FALSE(binds(m, "start"));
		CHECK(has(m, "a:b0"));
		CHECK(has(m, "b:b1"));
		checkNothingIsBoundTwice(m);
	}

	// Six is where they start to fit: four faces and the two above them.
	const std::string six = guessPadMapping("abc", "Six", 6, 2, 0);
	CHECK(has(six, "back:b4"));
	CHECK(has(six, "start:b5"));
	CHECK(has(six, "y:b3"));
	checkNothingIsBoundTwice(six);
}

TEST_CASE("nothing_is_bound_to_hardware_the_pad_does_not_have") {
	// Across every shape a device might report, including the silly ones. A
	// mapping naming a button that does not exist is a mapping SDL may reject
	// outright, which would put us back where we started.
	for (int buttons = 0; buttons <= 16; buttons++) {
		for (int axes = 0; axes <= 6; axes++) {
			for (int hats = 0; hats <= 2; hats++) {
				CAPTURE(buttons);
				CAPTURE(axes);
				CAPTURE(hats);
				const std::string m = guessPadMapping("abc", "Odd", buttons, axes, hats);
				const std::vector<std::string> all = pairs(m);
				for (std::size_t i = 0; i < all.size(); i++) {
					const std::string source = sourceOf(all[i]);
					CAPTURE(all[i]);
					if (source.size() > 1 && source[0] == 'b') {
						CHECK(std::atoi(source.c_str() + 1) < buttons);
					} else if (!source.empty() && source[0] == 'h') {
						CHECK(hats > 0);
					} else if (source.size() > 1) {
						// An axis, with or without a direction in front of it.
						const std::size_t digit = source.find_first_of("0123456789");
						REQUIRE(digit != std::string::npos);
						CHECK(std::atoi(source.c_str() + digit) < axes);
					}
				}
				checkNothingIsBoundTwice(m);
			}
		}
	}
}

TEST_CASE("a_comma_in_the_name_cannot_break_the_mapping_apart") {
	// Commas separate a mapping's fields, and devices really do report them. One
	// left in the name would shift every field after it by one and turn the whole
	// string into nonsense SDL would refuse.
	const std::string m = guessPadMapping("abc", "Acme, Inc. Pad", 10, 2, 0);
	CHECK(m.find("Acme, Inc.") == std::string::npos);
	CHECK(m.find("Acme  Inc. Pad") != std::string::npos);
	CHECK(has(m, "a:b0"));
	CHECK(has(m, "start:b9"));

	// And a device with no name at all still gets a usable one, rather than an
	// empty field where SDL expects text.
	const std::string blank = guessPadMapping("abc", "", 10, 2, 0);
	CHECK(blank.find(",,") == std::string::npos);
}
