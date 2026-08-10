#include "nes/Controller.h"

namespace nes {

Controller::Controller() :
		m_buttons(0), m_shift(0), m_strobe(false),
		m_device(DEVICE_PAD), m_zapperX(-1), m_zapperY(-1),
		m_zapperTrigger(false) { }

void Controller::setZapper(int x, int y, bool trigger) {
	m_zapperX = x;
	m_zapperY = y;
	m_zapperTrigger = trigger;
}

void Controller::setButtons(std::uint8_t mask) {
	m_buttons = mask;
	// While the strobe is high the register is transparent, so a change to the
	// buttons is visible immediately rather than at the next latch.
	if (m_strobe)
		m_shift = m_buttons;
}

void Controller::writeStrobe(std::uint8_t value) {
	const bool strobe = (value & 0x01) != 0;
	// High: the register is transparent and tracks the buttons. Falling edge: it
	// latches them. Both amount to loading the current state, so one test covers
	// the pair -- and a game that writes 1 then 0 gets its snapshot either way.
	if (strobe || m_strobe)
		m_shift = m_buttons;
	m_strobe = strobe;
}

std::uint8_t Controller::read() {
	const std::uint8_t bit = m_shift & 0x01;
	if (m_strobe) {
		// Transparent: every read reports A, and nothing shifts.
		m_shift = m_buttons;
	} else {
		// Ones shift in behind the eight real bits, so reads past the end return
		// 1 the way an official pad does.
		m_shift = static_cast<std::uint8_t>((m_shift >> 1) | 0x80);
	}
	return bit;
}

std::uint8_t Controller::peek() const {
	return m_strobe ? static_cast<std::uint8_t>(m_buttons & 0x01)
			: static_cast<std::uint8_t>(m_shift & 0x01);
}

std::uint8_t Controller::buttonFromName(const char* name) {
	if (!name)
		return 0;
	struct Entry { const char* name; std::uint8_t bit; };
	static const Entry table[] = {
		{ "a",      BUTTON_A },
		{ "b",      BUTTON_B },
		{ "select", BUTTON_SELECT },
		{ "start",  BUTTON_START },
		{ "up",     BUTTON_UP },
		{ "down",   BUTTON_DOWN },
		{ "left",   BUTTON_LEFT },
		{ "right",  BUTTON_RIGHT }
	};
	for (const Entry& entry : table) {
		const char* a = entry.name;
		const char* b = name;
		while (*a && *b) {
			const char lower = (*b >= 'A' && *b <= 'Z')
					? static_cast<char>(*b - 'A' + 'a') : *b;
			if (*a != lower)
				break;
			a++;
			b++;
		}
		if (*a == '\0' && *b == '\0')
			return entry.bit;
	}
	return 0;
}

void Controller::serialize(State& state) {
	state.tag("CTRL");
	state.value(m_buttons);
	state.value(m_shift);
	state.value(m_strobe);
	state.value(m_device);
	state.value(m_zapperX);
	state.value(m_zapperY);
	state.value(m_zapperTrigger);
}

} // namespace nes
