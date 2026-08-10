#ifndef NES_STATE_H
#define NES_STATE_H

//
// Saving and restoring the whole machine.
//
// The one design decision worth explaining: every class describes its state
// *once*, in a single serialize() that runs in both directions. A save routine
// and a separate load routine drift apart -- somebody adds a field to one and
// not the other, and the bug surfaces days later as a game that resumes almost
// correctly. With one description there is nothing to keep in step.
//
// The format is not portable and does not pretend to be. It is a dump of this
// build's state, guarded by a version and by the size of every structure it
// writes, so a state from a different build is refused rather than misread. If
// it ever needs to travel between builds, that is a different job with a
// different cost, and the guards are what will make the refusal loud.
//

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace nes {

/**
 * A stream that is being written, or read, depending on how it was made.
 *
 * Reading past the end, or hitting a structure whose size has changed, sets
 * failed() and every later call does nothing -- so a truncated or foreign file
 * cannot leave the machine half-loaded before anyone notices.
 */
class State {
public:
	static State forWriting();
	static State forReading(std::vector<std::uint8_t> bytes);

	bool writing() const { return m_writing; }
	bool failed() const { return m_failed; }

	/** Raw bytes: arrays, RAM, anything already flat. */
	void bytes(void* data, std::size_t size);

	/** One arithmetic or enum value. */
	template<class T>
	void value(T& v) {
		static_assert(std::is_arithmetic<T>::value || std::is_enum<T>::value,
				"State::value is for scalars; use structure() or bytes()");
		bytes(&v, sizeof(T));
	}

	/**
	 * A structure with no pointers in it, written whole.
	 *
	 * Its size goes into the stream first, so adding a member to one of these
	 * makes an older file fail cleanly instead of being read as nonsense. That
	 * check is what buys the right to dump a struct at all.
	 */
	template<class T>
	void structure(T& v) {
		static_assert(std::is_trivially_copyable<T>::value,
				"State::structure needs a trivially copyable type");
		std::uint32_t size = static_cast<std::uint32_t>(sizeof(T));
		value(size);
		if (size != sizeof(T)) {
			m_failed = true;
			return;
		}
		bytes(&v, sizeof(T));
	}

	/** A length-prefixed byte vector, resized to match when reading. */
	void blob(std::vector<std::uint8_t>& v);

	/**
	 * A label that must match, which turns a desynchronised stream into an
	 * error at the point it happened rather than a wrong value much later.
	 */
	void tag(const char* fourCharacters);

	const std::vector<std::uint8_t>& data() const { return m_data; }

private:
	State() : m_writing(true), m_at(0), m_failed(false) { }

	std::vector<std::uint8_t> m_data;
	bool m_writing;
	std::size_t m_at;
	bool m_failed;
};

/** Bumped whenever the shape of a saved state changes. */
const std::uint32_t STATE_VERSION = 1;

} // namespace nes

#endif // NES_STATE_H
