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

// VitaNoVideoPlayer.h ////////////////////////////////////////////////////////
// Video player for builds without a decoder. It keeps the list of movies the
// game data declares, so lookups by name work, and opens none of them: the
// engine treats a movie that cannot be opened as one that has finished.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include "GameClient/VideoPlayer.h"

// VitaNoVideoPlayer ----------------------------------------------------------
class VitaNoVideoPlayer : public VideoPlayer
{
public:
	virtual void setVolume(Real volume) override {}
};
