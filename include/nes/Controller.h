#ifndef NES_CONTROLLER_H
#define NES_CONTROLLER_H

#include "State.h"

#include <cstdint>

namespace nes {

/**
 * A standard NES controller: eight buttons behind a shift register.
 *
 * The console has only two pins per port, so the pad cannot report eight
 * buttons at once. Instead it holds a 4021 shift register that the CPU clocks
 * one bit at a time:
 *
 *   1. Write 1 to $4016. While that strobe is high the register continuously
 *      reloads from the buttons, so a read always returns A's current state.
 *   2. Write 0 to $4016. The register latches whatever was held at that instant.
 *   3. Read $4016 eight times. Each read returns the next bit and shifts, in the
 *      order A, B, Select, Start, Up, Down, Left, Right.
 *
 * A ninth read and beyond return 1 on an official Nintendo pad, because the
 * register shifts in a high bit once its eight are exhausted. Some games rely on
 * that to detect the pad, which is why the shift fills with ones rather than
 * zeroes here.
 *
 * Timing note: the latch is what makes input coherent. A game reads the pad once
 * per frame, and every button it sees comes from the same instant.
 */
class Controller {
public:
	// Bit values matching the order the shift register reports them in.
	static const std::uint8_t BUTTON_A      = 0x01;
	static const std::uint8_t BUTTON_B      = 0x02;
	static const std::uint8_t BUTTON_SELECT = 0x04;
	static const std::uint8_t BUTTON_START  = 0x08;
	static const std::uint8_t BUTTON_UP     = 0x10;
	static const std::uint8_t BUTTON_DOWN   = 0x20;
	static const std::uint8_t BUTTON_LEFT   = 0x40;
	static const std::uint8_t BUTTON_RIGHT  = 0x80;

	/**
	 * What is plugged into the port.
	 *
	 * The console cannot tell: both devices answer the same two pins, and which
	 * one is there is decided by whoever is holding it. A game knows only
	 * because it was sold with one in the box.
	 */
	enum Device {
		DEVICE_PAD,
		/**
		 * The Zapper: a phototransistor and a trigger, and no shift register at
		 * all. It answers a read of its port with two bits of level rather than
		 * a serial stream, so it ignores the strobe entirely.
		 */
		DEVICE_ZAPPER
	};

	/** $4017 bit 3, and it is inverted: clear means light was seen. */
	static const std::uint8_t ZAPPER_LIGHT   = 0x08;
	/** $4017 bit 4: set while the trigger is pulled. */
	static const std::uint8_t ZAPPER_TRIGGER = 0x10;

	Controller();

	void setDevice(Device device) { m_device = device; }
	Device device() const { return m_device; }

	/**
	 * Where the gun is pointed and whether the trigger is held.
	 *
	 * @param x,y  the pixel aimed at, or negative when the gun is off-screen --
	 *             pointed away from the television, which is how a player
	 *             cheats at Duck Hunt and has to read as "no light".
	 */
	void setZapper(int x, int y, bool trigger);
	int zapperX() const { return m_zapperX; }
	int zapperY() const { return m_zapperY; }
	bool zapperTrigger() const { return m_zapperTrigger; }
	/** True when the aim point is somewhere on the screen. */
	bool zapperOnScreen() const { return m_zapperX >= 0 && m_zapperY >= 0; }

	/**
	 * Replace the whole button state.
	 *
	 * Opposing directions are allowed through. Real hardware permits them -- a
	 * worn pad or a third-party stick can close Left and Right together -- and
	 * some games glitch when it happens. Filtering that belongs to whatever
	 * drives the input, not here.
	 */
	void setButtons(std::uint8_t mask);
	std::uint8_t buttons() const { return m_buttons; }

	void press(std::uint8_t mask) { setButtons(m_buttons | mask); }
	void release(std::uint8_t mask) { setButtons(m_buttons & ~mask); }

	/** A write to $4016: bit 0 is the strobe. */
	void writeStrobe(std::uint8_t value);

	/** A read of $4016/$4017: returns the next bit, 0 or 1, and shifts. */
	std::uint8_t read();

	/** The bit a read would return, without shifting. For debuggers. */
	std::uint8_t peek() const;

	bool strobe() const { return m_strobe; }

	/** Look up a button by name: "a", "start", "up"... @return 0 if unknown. */
	static std::uint8_t buttonFromName(const char* name);

	/** Save or restore this port. @see nes/State.h */
	void serialize(State& state);

private:
	std::uint8_t m_buttons;
	std::uint8_t m_shift;
	bool m_strobe;

	Device m_device;
	int m_zapperX;
	int m_zapperY;
	bool m_zapperTrigger;
};

} // namespace nes

#endif // NES_CONTROLLER_H
