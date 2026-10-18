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

// VitaGameEngine.h ///////////////////////////////////////////////////////////
// GameEngine for the PS Vita: creates the W3D device subsystems on top of the
// portable file system and services the console's controls once per frame.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/GameEngine.h"

// VitaGameEngine -------------------------------------------------------------
class VitaGameEngine : public GameEngine
{
public:
	VitaGameEngine();
	virtual ~VitaGameEngine() override;

	virtual void init() override;				///< initialization
	virtual void reset() override;				///< reset engine
	virtual void update() override;				///< update the game engine
	virtual void serviceWindowsOS() override;	///< read the controls; the Vita has no message queue to pump

protected:
	virtual GameLogic *createGameLogic() override;
	virtual GameClient *createGameClient() override;
	virtual ModuleFactory *createModuleFactory() override;
	virtual ThingFactory *createThingFactory() override;
	virtual FunctionLexicon *createFunctionLexicon() override;
	virtual LocalFileSystem *createLocalFileSystem() override;
	virtual ArchiveFileSystem *createArchiveFileSystem() override;
	virtual Radar *createRadar(Bool dummy) override;
	virtual WebBrowser *createWebBrowser() override;
	virtual ParticleSystemManager *createParticleSystemManager(Bool dummy) override;
	virtual AudioManager *createAudioManager(Bool dummy) override;
};
