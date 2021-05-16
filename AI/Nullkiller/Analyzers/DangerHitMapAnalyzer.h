/*
* DangerHitMapAnalyzer.h, part of VCMI engine
*
* Authors: listed in file AUTHORS in main folder
*
* License: GNU General Public License v2.0 or later
* Full text of license available in license.txt file, in main folder
*
*/
#pragma once

#include "../VCAI.h"

struct HitMapInfo
{
	uint64_t danger;
	uint8_t turn;
	const CGHeroInstance * hero;

	void reset()
	{
		danger = 0;
		turn = 255;
		hero = nullptr;
	}
};

struct HitMapNode
{
	HitMapInfo maximumDanger;
	HitMapInfo fastestDanger;

	void reset()
	{
		maximumDanger.reset();
		fastestDanger.reset();
	}
};

class DangerHitMapAnalyzer
{
private:
	boost::multi_array<HitMapNode, 3> hitMap;
	std::map<const CGHeroInstance *, std::set<const CGObjectInstance *>> enemyHeroAccessibleObjects;
	bool upToDate;
	const Nullkiller * ai;

public:
	DangerHitMapAnalyzer(const Nullkiller * ai) :ai(ai) {}

	void updateHitMap();
	uint64_t enemyCanKillOurHeroesAlongThePath(const AIPath & path) const;
	const HitMapNode & getObjectTreat(const CGObjectInstance * obj) const;
	const std::set<const CGObjectInstance *> & getOneTurnAccessibleObjects(const CGHeroInstance * enemy) const;
	void reset();
};