/*
**	Command & Conquer Generals(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// FILE: RelationMap.h ////////////////////////////////////////////////////////////////////////////
// GeneralsX @performance Claude 19/11/2026 The relationships a player or team has been given towards
// particular teams or players (Team::getRelationship(), Player::getRelationship()).
//
// These were hash maps. Each object an AI unit looks at when it searches for a target asks for the
// relationship, which asks up to four of these maps, and a lookup in a hash map divides by the
// number of its buckets: the PS Vita's processor has no instruction for that, and the call to the
// division routine and the nodes spread over memory cost 0.3 to 0.8 ms of a frame in a battle. The maps hold a handful of entries, so they are kept in an array now that is
// searched from the front. Every lookup gives what the hash map gave; only the order in which a
// save game lists the entries is the order they were set in. The maps are not part of the CRC.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/GameCommon.h"

#include <utility>
#include <vector>

template <typename Key>
class RelationMap
{
public:
	typedef std::pair<Key, Relationship> value_type;
	typedef typename std::vector<value_type>::iterator iterator;
	typedef typename std::vector<value_type>::const_iterator const_iterator;

	iterator begin() { return m_entries.begin(); }
	iterator end() { return m_entries.end(); }
	const_iterator begin() const { return m_entries.begin(); }
	const_iterator end() const { return m_entries.end(); }

	bool empty() const { return m_entries.empty(); }
	size_t size() const { return m_entries.size(); }
	void clear() { m_entries.clear(); }
	void erase(iterator it) { m_entries.erase(it); }

	iterator find(const Key &key)
	{
		for (iterator it = m_entries.begin(); it != m_entries.end(); ++it)
		{
			if (it->first == key)
				return it;
		}
		return m_entries.end();
	}

	const_iterator find(const Key &key) const
	{
		for (const_iterator it = m_entries.begin(); it != m_entries.end(); ++it)
		{
			if (it->first == key)
				return it;
		}
		return m_entries.end();
	}

	/// The relationship towards the key, made (as the hash map's operator[] did) if there was none.
	Relationship &operator[](const Key &key)
	{
		iterator it = find(key);
		if (it != m_entries.end())
			return it->second;
		m_entries.push_back(value_type(key, Relationship()));
		return m_entries.back().second;
	}

private:
	std::vector<value_type> m_entries;
};
