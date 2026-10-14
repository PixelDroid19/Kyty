#include "Emulator/Ports/KeyboardTextPort.h"

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <deque>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Ports {

namespace {

std::mutex                  g_mutex;
bool                        g_guest_open  = false;
bool                        g_dialog_open = false;
std::u16string              g_dialog_text;
std::deque<KeyboardTextKey> g_queue;

} // namespace

void KeyboardTextPort::SetGuestOpen(bool open)
{
	std::lock_guard lock(g_mutex);
	g_guest_open = open;
	g_queue.clear();
}

bool KeyboardTextPort::GuestOpen()
{
	std::lock_guard lock(g_mutex);
	return g_guest_open;
}

void KeyboardTextPort::SetDialogOpen(bool open)
{
	std::lock_guard lock(g_mutex);
	g_dialog_open = open;
	g_dialog_text.clear();
	g_queue.clear();
}

bool KeyboardTextPort::DialogOpen()
{
	std::lock_guard lock(g_mutex);
	return g_dialog_open;
}

void KeyboardTextPort::SetDialogText(const std::u16string& text)
{
	std::lock_guard lock(g_mutex);
	g_dialog_text = text;
}

std::u16string KeyboardTextPort::DialogText()
{
	std::lock_guard lock(g_mutex);
	return g_dialog_text;
}

bool KeyboardTextPort::Push(const KeyboardTextKey& key)
{
	std::lock_guard lock(g_mutex);
	if ((!g_guest_open && !g_dialog_open) || g_queue.size() >= QUEUE_MAX)
	{
		return false;
	}
	g_queue.push_back(key);
	return true;
}

size_t KeyboardTextPort::Drain(KeyboardTextKey* out, size_t max)
{
	std::lock_guard lock(g_mutex);
	size_t          count = 0;
	while (out != nullptr && count < max && !g_queue.empty())
	{
		out[count++] = g_queue.front();
		g_queue.pop_front();
	}
	return count;
}

} // namespace Kyty::Emulator::Ports

#endif // KYTY_EMU_ENABLED
