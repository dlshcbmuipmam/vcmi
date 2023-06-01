/*
 * WaterProxy.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"
#include "WaterProxy.h"
#include "../CMapGenerator.h"
#include "../RmgMap.h"
#include "../../TerrainHandler.h"
#include "../../mapping/CMap.h"
#include "../../mapping/CMapEditManager.h"
#include "../../mapObjects/CObjectClassesHandler.h"
#include "../RmgPath.h"
#include "../RmgObject.h"
#include "ObjectManager.h"
#include "../Functions.h"
#include "RoadPlacer.h"
#include "TreasurePlacer.h"
#include "TownPlacer.h"
#include "ConnectionsPlacer.h"
#include "../TileInfo.h"
#include "WaterAdopter.h"
#include "../RmgArea.h"

VCMI_LIB_NAMESPACE_BEGIN

void WaterProxy::process()
{
	for(const auto & t : zone.area().getTilesVector())
	{
		map.setZoneID(t, zone.getId());
		map.setOccupied(t, ETileType::POSSIBLE);
	}
	
	auto v = zone.getArea().getTilesVector();
	mapProxy->drawTerrain(zone.getRand(), v, zone.getTerrainType());
	
	//check terrain type
	for([[maybe_unused]] const auto & t : zone.area().getTilesVector())
	{
		assert(map.isOnMap(t));
		assert(map.getTile(t).terType->getId() == zone.getTerrainType());
	}

	for(const auto & z : map.getZones())
	{
		if(z.second->getId() == zone.getId())
			continue;

		Zone::Lock lock(z.second->areaMutex);
		for(const auto & t : z.second->area().getTilesVector())
		{
			if(map.getTile(t).terType->getId() == zone.getTerrainType())
			{
				z.second->areaPossible().erase(t);
				z.second->area().erase(t);
				zone.area().add(t);
				zone.areaPossible().add(t);
				map.setZoneID(t, zone.getId());
				map.setOccupied(t, ETileType::POSSIBLE);
			}
		}
	}
	
	if(!zone.area().contains(zone.getPos()))
	{
		zone.setPos(zone.area().getTilesVector().front());
	}
	
	zone.initFreeTiles();
	
	collectLakes();
}

void WaterProxy::init()
{
	for(auto & z : map.getZones())
	{
		dependency(z.second->getModificator<TownPlacer>());
		dependency(z.second->getModificator<WaterAdopter>());
		postfunction(z.second->getModificator<ConnectionsPlacer>());
		postfunction(z.second->getModificator<ObjectManager>());
	}
	POSTFUNCTION(TreasurePlacer);
}

const std::vector<WaterProxy::Lake> & WaterProxy::getLakes() const
{
	RecursiveLock lock(externalAccessMutex);
	return lakes;
}

void WaterProxy::collectLakes()
{
	RecursiveLock lock(externalAccessMutex);
	int lakeId = 0;
	for(const auto & lake : connectedAreas(zone.getArea(), true))
	{
		lakes.push_back(Lake{});
		lakes.back().area = lake;
		lakes.back().distanceMap = lake.computeDistanceMap(lakes.back().reverseDistanceMap);
		for(const auto & t : lake.getBorderOutside())
			if(map.isOnMap(t))
				lakes.back().neighbourZones[map.getZoneID(t)].add(t);
		for(const auto & t : lake.getTiles())
			lakeMap[t] = lakeId;
		
		//each lake must have at least one free tile
		if(!lake.overlap(zone.freePaths()))
			zone.freePaths().add(*lakes.back().reverseDistanceMap[lakes.back().reverseDistanceMap.size() - 1].begin());
		
		++lakeId;
	}
}

RouteInfo WaterProxy::waterRoute(Zone & dst)
{
	RouteInfo result;
	
	auto * adopter = dst.getModificator<WaterAdopter>();
	if(!adopter)
		return result;
	
	if(adopter->getCoastTiles().empty())
		return result;
	
	//block zones are not connected by template
	for(auto& lake : lakes)
	{
		if(lake.neighbourZones.count(dst.getId()))
		{
			if(!lake.keepConnections.count(dst.getId()))
			{
				for(const auto & ct : lake.neighbourZones[dst.getId()].getTiles())
				{
					if(map.isPossible(ct))
						map.setOccupied(ct, ETileType::BLOCKED);
				}

				Zone::Lock lock(dst.areaMutex);
				dst.areaPossible().subtract(lake.neighbourZones[dst.getId()]);
				continue;
			}

			//Don't place shipyard or boats on the very small lake
			if (lake.area.getTiles().size() < 25)
			{
				logGlobal->info("Skipping very small lake at zone %d", dst.getId());
				continue;
			}
						
			int zoneTowns = 0;
			if(auto * m = dst.getModificator<TownPlacer>())
				zoneTowns = m->getTotalTowns();
			
			if(dst.getType() == ETemplateZoneType::PLAYER_START || dst.getType() == ETemplateZoneType::CPU_START || zoneTowns)
			{
				if(placeShipyard(dst, lake, generator.getConfig().shipyardGuard, result))
				{
					logGlobal->info("Shipyard successfully placed at zone %d", dst.getId());
				}
				else
				{
					logGlobal->warn("Shipyard placement failed, trying boat at zone %d", dst.getId());
					if(placeBoat(dst, lake, result))
					{
						logGlobal->warn("Boat successfully placed at zone %d", dst.getId());
					}
					else
					{
						logGlobal->error("Boat placement failed at zone %d", dst.getId());
					}
				}
			}
			else
			{
				if(placeBoat(dst, lake, result))
				{
					logGlobal->info("Boat successfully placed at zone %d", dst.getId());
				}
				else
				{
					logGlobal->error("Boat placement failed at zone %d", dst.getId());
				}
			}
		}
	}
	
	return result;
}

bool WaterProxy::waterKeepConnection(TRmgTemplateZoneId zoneA, TRmgTemplateZoneId zoneB)
{
	for(auto & lake : lakes)
	{
		if(lake.neighbourZones.count(zoneA) && lake.neighbourZones.count(zoneB))
		{
			lake.keepConnections.insert(zoneA);
			lake.keepConnections.insert(zoneB);
			return true;
		}
	}
	return false;
}

bool WaterProxy::placeBoat(Zone & land, const Lake & lake, RouteInfo & info)
{
	auto * manager = zone.getModificator<ObjectManager>();
	if(!manager)
		return false;

	auto subObjects = VLC->objtypeh->knownSubObjects(Obj::BOAT);
	std::set<si32> sailingBoatTypes; //RMG shall place only sailing boats on water
	for(auto subObj : subObjects)
	{
		//making a temporary object
		std::unique_ptr<CGObjectInstance> obj(VLC->objtypeh->getHandlerFor(Obj::BOAT, subObj)->create());
		if(auto * testBoat = dynamic_cast<CGBoat *>(obj.get()))
		{
			if(testBoat->layer == EPathfindingLayer::SAIL)
				sailingBoatTypes.insert(subObj);
		}
	}
			
	if(sailingBoatTypes.empty())
		return false;
	
	auto * boat = dynamic_cast<CGBoat *>(VLC->objtypeh->getHandlerFor(Obj::BOAT, *RandomGeneratorUtil::nextItem(sailingBoatTypes, zone.getRand()))->create());

	rmg::Object rmgObject(*boat);
	rmgObject.setTemplate(zone.getTerrainType());

	auto waterAvailable = zone.areaPossible() + zone.freePaths();
	rmg::Area coast = lake.neighbourZones.at(land.getId()); //having land tiles
	coast.intersect(land.areaPossible() + land.freePaths()); //having only available land tiles
	auto boardingPositions = coast.getSubarea([&waterAvailable, this](const int3 & tile) //tiles where boarding is possible
		{
			//We don't want place boat right to any land object, especiallly the zone guard
			if (map.getTileInfo(tile).getNearestObjectDistance() <= 3)
				return false;

			rmg::Area a({tile});
			a = a.getBorderOutside();
			a.intersect(waterAvailable);
			return !a.empty();
		});

	while(!boardingPositions.empty())
	{
		auto boardingPosition = *boardingPositions.getTiles().begin();
		rmg::Area shipPositions({boardingPosition});
		auto boutside = shipPositions.getBorderOutside();
		shipPositions.assign(boutside);
		shipPositions.intersect(waterAvailable);
		if(shipPositions.empty())
		{
			boardingPositions.erase(boardingPosition);
			continue;
		}

		//try to place boat at water, create paths on water and land
		auto path = manager->placeAndConnectObject(shipPositions, rmgObject, 4, false, true, ObjectManager::OptimizeType::NONE);
		auto landPath = land.searchPath(boardingPosition, false);
		if(!path.valid() || !landPath.valid())
		{
			boardingPositions.erase(boardingPosition);
			continue;
		}

		info.blocked = rmgObject.getArea();
		info.visitable = rmgObject.getVisitablePosition();
		info.boarding = boardingPosition;
		info.water = shipPositions;

		zone.connectPath(path);
		land.connectPath(landPath);
		manager->placeObject(rmgObject, false, true);
		land.getModificator<ObjectManager>()->updateDistances(rmgObject); //Keep land objects away from the boat
		break;
	}

	return !boardingPositions.empty();
}

bool WaterProxy::placeShipyard(Zone & land, const Lake & lake, si32 guard, RouteInfo & info)
{
	auto * manager = land.getModificator<ObjectManager>();
	if(!manager)
		return false;
	
	int subtype = chooseRandomAppearance(zone.getRand(), Obj::SHIPYARD, land.getTerrainType());
	auto * shipyard = dynamic_cast<CGShipyard *>(VLC->objtypeh->getHandlerFor(Obj::SHIPYARD, subtype)->create());
	shipyard->tempOwner = PlayerColor::NEUTRAL;
	
	rmg::Object rmgObject(*shipyard);
	rmgObject.setTemplate(land.getTerrainType());
	bool guarded = manager->addGuard(rmgObject, guard);
	
	auto waterAvailable = zone.areaPossible() + zone.freePaths();
	waterAvailable.intersect(lake.area);
	rmg::Area coast = lake.neighbourZones.at(land.getId()); //having land tiles
	coast.intersect(land.areaPossible() + land.freePaths()); //having only available land tiles
	auto boardingPositions = coast.getSubarea([&waterAvailable](const int3 & tile) //tiles where boarding is possible
	{
		rmg::Area a({tile});
		a = a.getBorderOutside();
		a.intersect(waterAvailable);
		return !a.empty();
	});
	
	while(!boardingPositions.empty())
	{
		auto boardingPosition = *boardingPositions.getTiles().begin();
		rmg::Area shipPositions({boardingPosition});
		auto boutside = shipPositions.getBorderOutside();
		shipPositions.assign(boutside);
		shipPositions.intersect(waterAvailable);
		if(shipPositions.empty())
		{
			boardingPositions.erase(boardingPosition);
			continue;
		}
		
		//try to place shipyard close to boarding position and appropriate water access
		auto path = manager->placeAndConnectObject(land.areaPossible(), rmgObject, [&rmgObject, &shipPositions, &boardingPosition](const int3 & tile)
		{
			//Must only check the border of shipyard and not the added guard
			rmg::Area shipyardOut = rmgObject.instances().front()->getBlockedArea().getBorderOutside();

			if(!shipyardOut.contains(boardingPosition) || (shipyardOut * shipPositions).empty())
				return -1.f;
			
			return 1.0f;
		}, guarded, true, ObjectManager::OptimizeType::NONE);
		
		//search path to boarding position
		auto searchArea = land.areaPossible() - rmgObject.getArea();
		rmg::Path pathToBoarding(searchArea);
		pathToBoarding.connect(land.freePaths());
		pathToBoarding.connect(path);
		pathToBoarding = pathToBoarding.search(boardingPosition, false);
		
		//make sure shipyard places ship at position we defined
		rmg::Area shipyardOutToBlock(rmgObject.getArea().getBorderOutside());
		shipyardOutToBlock.intersect(waterAvailable);
		shipyardOutToBlock.subtract(shipPositions);
		shipPositions.subtract(shipyardOutToBlock);
		auto pathToBoat = zone.searchPath(shipPositions, true);
		
		if(!path.valid() || !pathToBoarding.valid() || !pathToBoat.valid())
		{
			boardingPositions.erase(boardingPosition);
			continue;
		}
		
		land.connectPath(path);
		land.connectPath(pathToBoarding);
		zone.connectPath(pathToBoat);
		
		info.blocked = rmgObject.getArea();
		info.visitable = rmgObject.getVisitablePosition();
		info.boarding = boardingPosition;
		info.water = shipPositions;
		
		manager->placeObject(rmgObject, guarded, true);
		
		zone.areaPossible().subtract(shipyardOutToBlock);
		for(const auto & i : shipyardOutToBlock.getTilesVector())
			if(map.isOnMap(i) && map.isPossible(i))
				map.setOccupied(i, ETileType::BLOCKED);
		
		break;
	}
	
	return !boardingPositions.empty();
}

char WaterProxy::dump(const int3 & t)
{
	auto lakeIter = lakeMap.find(t);
	if(lakeIter == lakeMap.end())
		return '?';
	
	Lake & lake = lakes[lakeMap.at(t)];
	for(const auto & i : lake.neighbourZones)
	{
		if(i.second.contains(t))
			return lake.keepConnections.count(i.first) ? std::to_string(i.first)[0] : '=';
	}
	
	return '~';
}

VCMI_LIB_NAMESPACE_END