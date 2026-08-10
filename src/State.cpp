#include "nes/State.h"

namespace nes {

State State::forWriting() {
	State state;
	state.m_writing = true;
	return state;
}

State State::forReading(std::vector<std::uint8_t> bytes) {
	State state;
	state.m_writing = false;
	state.m_data = std::move(bytes);
	return state;
}

void State::bytes(void* data, std::size_t size) {
	if (m_failed)
		return;
	if (m_writing) {
		const std::uint8_t* from = static_cast<const std::uint8_t*>(data);
		m_data.insert(m_data.end(), from, from + size);
		return;
	}
	if (m_at + size > m_data.size()) {
		// Truncated, or not one of ours. Everything after this does nothing, so
		// a caller can run the whole serialize() and check once at the end.
		m_failed = true;
		return;
	}
	std::memcpy(data, m_data.data() + m_at, size);
	m_at += size;
}

void State::blob(std::vector<std::uint8_t>& v) {
	std::uint32_t size = static_cast<std::uint32_t>(v.size());
	value(size);
	if (m_failed)
		return;
	if (!m_writing) {
		// A length from a corrupt file could be enormous, so it is checked
		// against what is actually left rather than trusted into a resize.
		if (m_at + size > m_data.size()) {
			m_failed = true;
			return;
		}
		v.resize(size);
	}
	if (size > 0)
		bytes(v.data(), size);
}

void State::tag(const char* fourCharacters) {
	char label[4] = { 0, 0, 0, 0 };
	for (int i = 0; i < 4 && fourCharacters[i]; i++)
		label[i] = fourCharacters[i];

	if (m_writing) {
		bytes(label, 4);
		return;
	}
	char found[4] = { 0, 0, 0, 0 };
	bytes(found, 4);
	if (m_failed)
		return;
	for (int i = 0; i < 4; i++) {
		if (found[i] != label[i]) {
			m_failed = true;
			return;
		}
	}
}

} // namespace nes
