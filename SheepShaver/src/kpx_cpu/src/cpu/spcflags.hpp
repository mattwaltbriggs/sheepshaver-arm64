/*
 *  spcflags.hpp - CPU special flags
 *
 *  Kheperix (C) 2003-2005 Gwenole Beauchesne
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef SPCFLAGS_H
#define SPCFLAGS_H

/**
 *		Basic special flags
 **/

enum {
	SPCFLAG_CPU_EXEC_RETURN			= 1 << 0,	// Return from emulation loop
	SPCFLAG_CPU_TRIGGER_INTERRUPT	= 1 << 1,	// Trigger user interrupt
	SPCFLAG_CPU_HANDLE_INTERRUPT	= 1 << 2,	// Call user interrupt handler
	SPCFLAG_CPU_ENTER_MON			= 1 << 3,	// Enter cxmon
	SPCFLAG_JIT_EXEC_RETURN			= 1 << 4,	// Return from compiled code
};

class basic_spcflags
{
	uint32 mask;
	spinlock_t lock;

public:

	basic_spcflags()
		: mask(0), lock(SPIN_LOCK_UNLOCKED)
		{ }

	void reset(uint32 v = 0)
	{
#if defined(__clang__) || defined(__GNUC__)
		__atomic_store_n(&mask, v, __ATOMIC_SEQ_CST);
#else
		mask = v;
#endif
		lock = SPIN_LOCK_UNLOCKED;
	}

	bool empty() const
	{
#if defined(__clang__) || defined(__GNUC__)
		return (__atomic_load_n(const_cast<uint32 *>(&mask), __ATOMIC_SEQ_CST) == 0);
#else
		return (mask == 0);
#endif
	}

	bool test(uint32 v) const
	{
#if defined(__clang__) || defined(__GNUC__)
		return (__atomic_load_n(const_cast<uint32 *>(&mask), __ATOMIC_SEQ_CST) & v);
#else
		return (mask & v);
#endif
	}

	void init(uint32 v)
	{
#if defined(__clang__) || defined(__GNUC__)
		__atomic_store_n(&mask, v, __ATOMIC_SEQ_CST);
#else
		spin_lock(&lock); mask = v; spin_unlock(&lock);
#endif
	}

	uint32 get() const
	{
#if defined(__clang__) || defined(__GNUC__)
		return __atomic_load_n(const_cast<uint32 *>(&mask), __ATOMIC_SEQ_CST);
#else
		return mask;
#endif
	}

	void set(uint32 v)
	{
#if defined(__clang__) || defined(__GNUC__)
		__atomic_fetch_or(&mask, v, __ATOMIC_SEQ_CST);
#else
		spin_lock(&lock); mask |= v; spin_unlock(&lock);
#endif
	}

	void clear(uint32 v)
	{
#if defined(__clang__) || defined(__GNUC__)
		__atomic_fetch_and(&mask, ~v, __ATOMIC_SEQ_CST);
#else
		spin_lock(&lock); mask &= ~v; spin_unlock(&lock);
#endif
	}
};

#endif /* SPCFLAGS_H */
