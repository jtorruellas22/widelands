/*
 * Copyright (C) 2004, 2006-2010, 2012 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "ai/defaultai.h"

#include <algorithm>
#include <ctime>
#include <memory>
#include <queue>
#include <typeinfo>
#include <unordered_set>

#include "ai/ai_hints.h"
#include "base/log.h"
#include "base/macros.h"
#include "economy/economy.h"
#include "economy/flag.h"
#include "economy/road.h"
#include "logic/constructionsite.h"
#include "logic/findbob.h"
#include "logic/findimmovable.h"
#include "logic/findnode.h"
#include "logic/map.h"
#include "logic/militarysite.h"
#include "logic/player.h"
#include "logic/productionsite.h"
#include "logic/trainingsite.h"
#include "logic/tribe.h"
#include "logic/warehouse.h"
#include "logic/world/world.h"
#include "profile/profile.h"

// Building of new military buildings can be restricted
constexpr int kPushExpansion = 1;
constexpr int kResourcesOrDefense = 2;
constexpr int kDefenseOnly = 3;
constexpr int kNoNewMilitary = 4;

// following is in miliseconds (widelands counts time in ms)
constexpr int kFieldUpdateInterval = 2000;
constexpr int kIdleMineUpdateInterval = 22000;
constexpr int kBusyMineUpdateInterval = 2000;
// building of the same building can be started after 25s at earliest
constexpr int kBuildingMinInterval = 25 * 1000;
constexpr int kMinBFCheckInterval = 6 * 1000;
// Some buildings have to be built close to borders and their
// priority might be decreased below 0, so this is to
// compensate
constexpr int32_t kDefaultPrioBoost = 12;

using namespace Widelands;

DefaultAI::AggressiveImpl DefaultAI::aggressiveImpl;
DefaultAI::NormalImpl DefaultAI::normalImpl;
DefaultAI::DefensiveImpl DefaultAI::defensiveImpl;

/// Constructor of DefaultAI
DefaultAI::DefaultAI(Game& ggame, PlayerNumber const pid, uint8_t const t)
   : ComputerPlayer(ggame, pid),
     type_(t),
     m_buildable_changed(true),
     m_mineable_changed(true),
     player_(nullptr),
     tribe_(nullptr),
     num_constructionsites_(0),
     num_milit_constructionsites(0),
     num_prod_constructionsites(0),
     next_road_due_(2000),
     next_stats_update_due_(30000),
     next_construction_due_(1000),
     next_mine_construction_due_(0),
     next_productionsite_check_due_(0),
     next_mine_check_due_(0),
     next_militarysite_check_due_(0),
     next_attack_consideration_due_(300000),
     next_helpersites_check_due_(180000),
     next_bf_check_due_(1000),
     inhibit_road_building_(0),
     time_of_last_construction_(0),
     enemy_last_seen_(-2 * 60 * 1000),
     numof_warehouses_(0),
     new_buildings_stop_(false),
     resource_necessity_territory_(255),
     resource_necessity_mines_(255),
     resource_necessity_stones_(255),
     resource_necessity_water_(0),
     resource_necessity_water_needed_(false),
     unstationed_milit_buildings_(0),
     military_last_dismantle_(0),
     military_last_build_(-60 * 1000),

     spots_(0) {

	// Subscribe to NoteFieldPossession.
	field_possession_subscriber_ =
	   Notifications::subscribe<NoteFieldPossession>([this](const NoteFieldPossession& note) {
			if (note.player != player_) {
			   return;
		   }
			if (note.ownership == NoteFieldPossession::Ownership::GAINED) {
			   unusable_fields.push_back(note.fc);
		   }
		});

	// Subscribe to NoteImmovables.
	immovable_subscriber_ =
	   Notifications::subscribe<NoteImmovable>([this](const NoteImmovable& note) {
			if (note.pi->owner().player_number() != player_->player_number()) {
			   return;
		   }
			if (note.ownership == NoteImmovable::Ownership::GAINED) {
			   gain_immovable(*note.pi);
		   } else {
			   lose_immovable(*note.pi);
		   }
		});

	// Subscribe to ProductionSiteOutOfResources.
	outofresource_subscriber_ = Notifications::subscribe<NoteProductionSiteOutOfResources>(
	   [this](const NoteProductionSiteOutOfResources& note) {
			if (note.ps->owner().player_number() != player_->player_number()) {
			   return;
		   }

		   out_of_resources_site(*note.ps);

		});
}

DefaultAI::~DefaultAI() {
	while (!buildable_fields.empty()) {
		delete buildable_fields.back();
		buildable_fields.pop_back();
	}

	while (!mineable_fields.empty()) {
		delete mineable_fields.back();
		mineable_fields.pop_back();
	}

	while (!economies.empty()) {
		delete economies.back();
		economies.pop_back();
	}
}

/**
 * Main loop of computer player_ "defaultAI"
 *
 * General behaviour is defined here.
 */
void DefaultAI::think() {

	if (tribe_ == nullptr) {
		late_initialization();
	}

	const int32_t gametime = game().get_gametime();

	if (m_buildable_changed || next_bf_check_due_ < gametime) {
		// update statistics about buildable fields
		update_all_buildable_fields(gametime);
		next_bf_check_due_ = gametime + kMinBFCheckInterval;
	}

	m_buildable_changed = false;

	// perpetually tries to improve roads
	if (next_road_due_ <= gametime) {
		next_road_due_ = gametime + 1000;

		if (improve_roads(gametime)) {
			m_buildable_changed = true;
			return;
		}
	} else {
		// only go on, after defaultAI tried to improve roads.
		return;
	}

	// NOTE Because of the check above, the following parts of think() are used
	// NOTE only once every second at maximum. This increases performance and as
	// NOTE human player_s can not even react that fast, it should not be a
	// NOTE disadvantage for the defaultAI.
	// This must be checked every time as changes of bobs in AI area aren't
	// handled by the AI itself.
	update_all_not_buildable_fields();

	// considering attack
	if (next_attack_consideration_due_ <= gametime) {
		consider_attack(gametime);
	}

	// check if anything in the economies changed.
	// This needs to be done before new buildings are placed, to ensure that no
	// empty economy is left.
	if (check_economies()) {
		return;
	}

	// Before thinking about a new construction, update current stats, to have
	// a better view on current economy.
	if (next_stats_update_due_ <= gametime) {
		update_productionsite_stats(gametime);
	}

	// Now try to build something if possible
	if (next_construction_due_ <= gametime) {
		next_construction_due_ = gametime + 2000;

		if (construct_building(gametime)) {
			time_of_last_construction_ = gametime;
			m_buildable_changed = true;
			return;
		}
	}

	// verify that our production sites are doing well
	if (check_productionsites(gametime)) {
		return;
	}

	// Check the mines and consider upgrading or destroying one
	if (check_mines_(gametime)) {
		return;
	}

	// consider whether a change of the soldier capacity of some militarysites
	// would make sense.
	if (check_militarysites(gametime)) {
		return;
	}

	// improve existing roads!
	// main part of this improvment is creation 'shortcut roads'
	// this includes also connection of new buildings
	if (improve_roads(gametime)) {
		m_buildable_changed = true;
		m_mineable_changed = true;
		return;
	}
}

/**
 * Cares for all variables not initialised during construction
 *
 * When DefaultAI is constructed, some information is not yet available (e.g.
 * world), so this is done after complete loading of the map.
 */
void DefaultAI::late_initialization() {
	player_ = game().get_player(player_number());
	tribe_ = &player_->tribe();
	log("ComputerPlayer(%d): initializing (%u)\n", player_number(), type_);
	WareIndex const nr_wares = tribe_->get_nrwares();
	wares.resize(nr_wares);

	for (WareIndex i = 0; i < nr_wares; ++i) {
		wares.at(i).producers_ = 0;
		wares.at(i).consumers_ = 0;
		wares.at(i).preciousness_ = tribe_->get_ware_descr(i)->preciousness();
	}

	// collect information about the different buildings our tribe can construct
	BuildingIndex const nr_buildings = tribe_->get_nrbuildings();
	const World& world = game().world();

	for (BuildingIndex i = 0; i < nr_buildings; ++i) {
		const BuildingDescr& bld = *tribe_->get_building_descr(i);
		const std::string& building_name = bld.name();
		const BuildingHints& bh = bld.hints();
		buildings_.resize(buildings_.size() + 1);
		BuildingObserver& bo = buildings_.back();
		bo.name = building_name.c_str();
		bo.id = i;
		bo.desc = &bld;
		bo.type = BuildingObserver::BORING;
		bo.cnt_built_ = 0;
		bo.cnt_under_construction_ = 0;
		bo.cnt_target_ = 1;  // default for everything
		bo.stocklevel_ = 0;
		bo.stocklevel_time = 0;
		bo.last_dismantle_time_ = 0;
		// this is set to negative number, otherwise the AI would wait 25 sec
		// after game start not building anything
		bo.construction_decision_time_ = -60 * 60 * 1000;
		bo.production_hint_ = -1;
		bo.current_stats_ = 0;
		bo.unoccupied_ = false;
		bo.is_buildable_ = bld.is_buildable();
		bo.need_trees_ = bh.is_logproducer();
		bo.need_stones_ = bh.is_stoneproducer();
		bo.need_water_ = bh.get_needs_water();
		bo.mines_water_ = bh.mines_water();
		bo.recruitment_ = bh.for_recruitment();
		bo.space_consumer_ = bh.is_space_consumer();
		bo.expansion_type_ = bh.is_expansion_type();
		bo.fighting_type_ = bh.is_fighting_type();
		bo.mountain_conqueror_ = bh.is_mountain_conqueror();
		bo.prohibited_till_ = bh.get_prohibited_till() * 1000;  // value in conf is in seconds
		bo.forced_after_ = bh.get_forced_after() * 1000;        // value in conf is in seconds
		if (char const* const s = bh.get_renews_map_resource()) {
			bo.production_hint_ = tribe_->safe_ware_index(s);
		}

		// I just presume cut wood is named "log" in the game
		if (tribe_->safe_ware_index("log") == bo.production_hint_) {
			bo.plants_trees_ = true;
		} else {
			bo.plants_trees_ = false;
		}

		// Read all interesting data from ware producing buildings
		if (typeid(bld) == typeid(ProductionSiteDescr)) {
			const ProductionSiteDescr& prod =
			   ref_cast<ProductionSiteDescr const, BuildingDescr const>(bld);
			bo.type = bld.get_ismine() ? BuildingObserver::MINE : BuildingObserver::PRODUCTIONSITE;
			for (const WareAmount& temp_input : prod.inputs()) {
				bo.inputs_.push_back(temp_input.first);
			}
			for (const WareIndex& temp_output : prod.output_ware_types()) {
				bo.outputs_.push_back(temp_output);
			}

			if (bo.type == BuildingObserver::MINE) {
				// get the resource needed by the mine
				if (char const* const s = bh.get_mines()) {
					bo.mines_ = world.get_resource(s);
				}

				bo.mines_percent_ = bh.get_mines_percent();
			}

			// here we identify hunters
			if (bo.outputs_.size() == 1 && tribe_->safe_ware_index("meat") == bo.outputs_.at(0)) {
				bo.is_hunter_ = true;
			} else {
				bo.is_hunter_ = false;
			}

			// and fishers
			if (bo.outputs_.size() == 1 && tribe_->safe_ware_index("fish") == bo.outputs_.at(0)) {
				bo.is_fisher_ = true;
			} else {
				bo.is_fisher_ = false;
			}
			continue;
		}

		if (typeid(bld) == typeid(MilitarySiteDescr)) {
			bo.type = BuildingObserver::MILITARYSITE;
			continue;
		}

		if (typeid(bld) == typeid(WarehouseDescr)) {
			bo.type = BuildingObserver::WAREHOUSE;
			continue;
		}

		if (typeid(bld) == typeid(TrainingSiteDescr)) {
			bo.type = BuildingObserver::TRAININGSITE;
			continue;
		}

		if (typeid(bld) == typeid(ConstructionSiteDescr)) {
			bo.type = BuildingObserver::CONSTRUCTIONSITE;
			continue;
		}
	}

	num_constructionsites_ = 0;
	num_milit_constructionsites = 0;
	num_prod_constructionsites = 0;
	next_construction_due_ = 0;
	next_road_due_ = 1000;
	next_productionsite_check_due_ = 0;
	inhibit_road_building_ = 0;
	// atlanteans they consider water as a resource
	// (together with mines, stones and wood)
	if (tribe_->name() == "atlanteans") {
		resource_necessity_water_needed_ = true;
	}

	// Add all fields that we own
	Map& map = game().map();
	std::set<OPtr<PlayerImmovable>> found_immovables;

	for (int16_t y = 0; y < map.get_height(); ++y) {
		for (int16_t x = 0; x < map.get_width(); ++x) {
			FCoords f = map.get_fcoords(Coords(x, y));

			if (f.field->get_owned_by() != player_number()) {
				continue;
			}

			unusable_fields.push_back(f);

			if (upcast(PlayerImmovable, imm, f.field->get_immovable())) {

				//  Guard by a set - immovables might be on several nodes at once.
				if (&imm->owner() == player_ && !found_immovables.count(imm)) {
					found_immovables.insert(imm);
					gain_immovable(*imm);
				}
			}
		}
	}
}

/**
 * Checks ALL available buildable fields.
 *
 * this shouldn't be used often, as it might hang the game for some 100
 * milliseconds if the area the computer owns is big.
 */
void DefaultAI::update_all_buildable_fields(const int32_t gametime) {

	uint16_t i = 0;

	while (!buildable_fields.empty() && buildable_fields.front()->next_update_due_ <= gametime &&
	       i < 25) {
		BuildableField& bf = *buildable_fields.front();

		//  check whether we lost ownership of the node
		if (bf.coords.field->get_owned_by() != player_number()) {
			delete &bf;
			buildable_fields.pop_front();
			continue;
		}

		//  check whether we can still construct regular buildings on the node
		if ((player_->get_buildcaps(bf.coords) & BUILDCAPS_SIZEMASK) == 0) {
			unusable_fields.push_back(bf.coords);
			delete &bf;
			buildable_fields.pop_front();
			continue;
		}

		update_buildable_field(bf);
		bf.next_update_due_ = gametime + kFieldUpdateInterval;
		buildable_fields.push_back(&bf);
		buildable_fields.pop_front();

		i += 1;
	}
}

/**
 * Checks ALL available mineable fields.
 *
 * this shouldn't be used often, as it might hang the game for some 100
 * milliseconds if the area the computer owns is big.
 */
void DefaultAI::update_all_mineable_fields(const int32_t gametime) {

	uint16_t i = 0;  // counter, used to track # of checked fields

	while (!mineable_fields.empty() && mineable_fields.front()->next_update_due_ <= gametime &&
	       i < 40) {
		MineableField* mf = mineable_fields.front();

		//  check whether we lost ownership of the node
		if (mf->coords.field->get_owned_by() != player_number()) {
			delete mf;
			mineable_fields.pop_front();
			continue;
		}

		//  check whether we can still construct regular buildings on the node
		if ((player_->get_buildcaps(mf->coords) & BUILDCAPS_MINE) == 0) {
			unusable_fields.push_back(mf->coords);
			delete mf;
			mineable_fields.pop_front();
			continue;
		}

		update_mineable_field(*mf);
		mf->next_update_due_ = gametime + kFieldUpdateInterval;  // in fact this has very small effect
		mineable_fields.push_back(mf);
		mineable_fields.pop_front();

		i += 1;
	}
}

/**
 * Checks up to 50 fields that weren't buildable the last time.
 *
 * milliseconds if the area the computer owns is big.
 */
void DefaultAI::update_all_not_buildable_fields() {
	int32_t const pn = player_number();
	uint32_t maxchecks = unusable_fields.size();

	if (maxchecks > 50) {
		maxchecks = 50;
	}

	for (uint32_t i = 0; i < maxchecks; ++i) {
		//  check whether we lost ownership of the node
		if (unusable_fields.front().field->get_owned_by() != pn) {
			unusable_fields.pop_front();
			continue;
		}

		// check whether building capabilities have improved
		if (player_->get_buildcaps(unusable_fields.front()) & BUILDCAPS_SIZEMASK) {
			buildable_fields.push_back(new BuildableField(unusable_fields.front()));
			unusable_fields.pop_front();
			update_buildable_field(*buildable_fields.back());
			continue;
		}

		if (player_->get_buildcaps(unusable_fields.front()) & BUILDCAPS_MINE) {
			mineable_fields.push_back(new MineableField(unusable_fields.front()));
			unusable_fields.pop_front();
			update_mineable_field(*mineable_fields.back());
			continue;
		}

		unusable_fields.push_back(unusable_fields.front());
		unusable_fields.pop_front();
	}
}

/// Updates one buildable field
void DefaultAI::update_buildable_field(BuildableField& field, uint16_t range, bool military) {
	// look if there is any unowned land nearby
	Map& map = game().map();
	FindNodeUnowned find_unowned(player_, game());
	FindNodeUnownedMineable find_unowned_mines_pots(player_, game());
	PlayerNumber const pn = player_->player_number();
	const World& world = game().world();
	field.unowned_land_nearby_ =
	   map.find_fields(Area<FCoords>(field.coords, range), nullptr, find_unowned);

	field.near_border_ = false;
	if (field.unowned_land_nearby_ > 0) {
		if (map.find_fields(Area<FCoords>(field.coords, 4), nullptr, find_unowned) > 0) {
			field.near_border_ = true;
		}
	}

	// to save some CPU
	if ((mines_.size() > 8 && game().get_gametime() % 3 > 0) || field.unowned_land_nearby_ == 0) {
		field.unowned_mines_pots_nearby_ = 0;
	} else {
		uint32_t close_mines =
		   map.find_fields(Area<FCoords>(field.coords, 4), nullptr, find_unowned_mines_pots);
		uint32_t distant_mines =
		   map.find_fields(Area<FCoords>(field.coords, (range + 6 < 12) ? 12 : range + 6),
		                   nullptr,
		                   find_unowned_mines_pots);
		distant_mines = distant_mines - close_mines;
		field.unowned_mines_pots_nearby_ = 3 * close_mines + distant_mines / 2;
		if (distant_mines > 0) {
			field.unowned_mines_pots_nearby_ += 15;
		}
	}

	// collect information about resources in the area
	std::vector<ImmovableFound> immovables;
	// Search in a radius of range
	map.find_immovables(Area<FCoords>(field.coords, range), &immovables);

	// Is this a general update or just for military consideration
	// (second is used in check_militarysites)
	if (!military) {
		int32_t const tree_attr = MapObjectDescr::get_attribute_id("tree");
		field.preferred_ = false;
		field.enemy_nearby_ = false;
		field.military_capacity_ = 0;
		field.military_loneliness_ = 1000;  // instead of floats(v-
		field.military_presence_ = 0;
		field.military_stationed_ = 0;
		field.trees_nearby_ = 0;
		field.space_consumers_nearby_ = 0;
		field.producers_nearby_.clear();
		field.producers_nearby_.resize(wares.size());
		field.consumers_nearby_.clear();
		field.consumers_nearby_.resize(wares.size());
		std::vector<Coords> water_list;
		std::vector<Coords> resource_list;
		std::vector<Bob*> critters_list;

		if (field.water_nearby_ == -1) {  //-1 means "value has never been calculated"

			FindNodeWater find_water(game().world());
			map.find_fields(Area<FCoords>(field.coords, 5), &water_list, find_water);
			field.water_nearby_ = water_list.size();

			if (resource_necessity_water_needed_) {  // for atlanteans
				map.find_fields(Area<FCoords>(field.coords, 14), &water_list, find_water);
				field.distant_water_ = water_list.size() - field.water_nearby_;
			}
		}

		// counting fields with fish
		if (field.water_nearby_ > 0 && (field.fish_nearby_ = -1 || game().get_gametime() % 10 == 0)) {
			map.find_fields(Area<FCoords>(field.coords, 6),
			                &resource_list,
			                FindNodeResource(world.get_resource("fish")));
			field.fish_nearby_ = resource_list.size();
		}

		// counting fields with critters (game)
		// not doing this always, this does not change fast
		if (game().get_gametime() % 10 == 0) {
			map.find_bobs(Area<FCoords>(field.coords, 6), &critters_list, FindBobCritter());
			field.critters_nearby_ = critters_list.size();
		}

		FCoords fse;
		map.get_neighbour(field.coords, WALK_SE, &fse);

		if (BaseImmovable const* const imm = fse.field->get_immovable()) {
			if (dynamic_cast<Flag const*>(imm) ||
			    (dynamic_cast<Road const*>(imm) && (fse.field->nodecaps() & BUILDCAPS_FLAG))) {
				field.preferred_ = true;
			}
		}

		for (uint32_t i = 0; i < immovables.size(); ++i) {
			const BaseImmovable& base_immovable = *immovables.at(i).object;

			if (upcast(PlayerImmovable const, player_immovable, &base_immovable)) {

				// TODO(unknown): Only continue; if this is an opposing site
				// allied sites should be counted for military influence
				if (player_immovable->owner().player_number() != pn) {
					if (player_->is_hostile(player_immovable->owner())) {
						field.enemy_nearby_ = true;
					}
					enemy_last_seen_ = game().get_gametime();

					continue;
				}
			}

			if (upcast(Building const, building, &base_immovable)) {
				if (upcast(ConstructionSite const, constructionsite, building)) {
					const BuildingDescr& target_descr = constructionsite->building();

					if (dynamic_cast<ProductionSiteDescr const*>(&target_descr)) {
						consider_productionsite_influence(
						   field,
						   immovables.at(i).coords,
						   get_building_observer(constructionsite->descr().name().c_str()));
					}
				}

				if (dynamic_cast<const ProductionSite*>(building)) {
					consider_productionsite_influence(
					   field,
					   immovables.at(i).coords,
					   get_building_observer(building->descr().name().c_str()));
				}
			}

			if (immovables.at(i).object->has_attribute(tree_attr)) {
				++field.trees_nearby_;
			}
		}

		// stones are not renewable, we will count them only if previous state si nonzero
		if (field.stones_nearby_ > 0) {

			int32_t const stone_attr = MapObjectDescr::get_attribute_id("granite");
			field.stones_nearby_ = 0;

			for (uint32_t j = 0; j < immovables.size(); ++j) {
				// const BaseImmovable & base_immovable = *immovables.at(i).object;
				if (immovables.at(j).object->has_attribute(stone_attr)) {
					++field.stones_nearby_;
				}
			}
		}

		// ground water is not renewable and its amount can only fall, we will count them only if
		// previous state si nonzero
		if (field.ground_water_ > 0) {
			field.ground_water_ = field.coords.field->get_resources_amount();
		}
	}

	// the following is done always (regardless of military or not)

	// we get immovables with higher radius
	immovables.clear();
	map.find_immovables(Area<FCoords>(field.coords, (range < 10) ? 10 : range), &immovables);
	field.military_stationed_ = 0;
	field.military_in_constr_nearby_ = 0;
	field.military_capacity_ = 0;
	field.military_loneliness_ = 1000;
	field.military_presence_ = 0;

	for (uint32_t i = 0; i < immovables.size(); ++i) {

		const BaseImmovable& base_immovable = *immovables.at(i).object;

		// testing if it is enemy-owned field
		// TODO(unknown): count such fields...
		if (upcast(PlayerImmovable const, player_immovable, &base_immovable)) {

			// TODO(unknown): Only continue; if this is an opposing site
			// allied sites should be counted for military influence
			if (player_immovable->owner().player_number() != pn) {
				if (player_->is_hostile(player_immovable->owner())) {
					field.enemy_nearby_ = true;
				}

				continue;
			}
		}

		if (upcast(Building const, building, &base_immovable)) {
			if (upcast(ConstructionSite const, constructionsite, building)) {
				const BuildingDescr& target_descr = constructionsite->building();

				if (upcast(MilitarySiteDescr const, target_ms_d, &target_descr)) {
					const int32_t dist = map.calc_distance(field.coords, immovables.at(i).coords);
					const int32_t radius = target_ms_d->get_conquers() + 4;
					const int32_t v = radius - dist;

					if (v > 0) {
						field.military_capacity_ += target_ms_d->get_max_number_of_soldiers() / 2 + 1;
						field.military_loneliness_ *= static_cast<double_t>(dist) / radius;
						field.military_in_constr_nearby_ += 1;
					}
				}
			}

			if (upcast(MilitarySite const, militarysite, building)) {
				const int32_t dist = map.calc_distance(field.coords, immovables.at(i).coords);
				const int32_t radius = militarysite->descr().get_conquers() + 4;
				const int32_t v = radius - dist;

				if (v > 0 && dist > 0) {

					field.military_capacity_ += militarysite->max_soldier_capacity();
					field.military_presence_ += militarysite->stationed_soldiers().size();

					if (!militarysite->stationed_soldiers().empty()) {
						field.military_stationed_ += 1;
					} else {
						// the name does not match much
						field.military_in_constr_nearby_ += 1;
					}

					field.military_loneliness_ *= static_cast<double_t>(dist) / radius;
				}
			}
		}
	}
}

/// Updates one mineable field
void DefaultAI::update_mineable_field(MineableField& field) {
	// collect information about resources in the area
	std::vector<ImmovableFound> immovables;
	Map& map = game().map();
	map.find_immovables(Area<FCoords>(field.coords, 5), &immovables);
	field.preferred_ = false;
	field.mines_nearby_ = 1;
	FCoords fse;
	map.get_brn(field.coords, &fse);

	if (BaseImmovable const* const imm = fse.field->get_immovable()) {
		if (dynamic_cast<Flag const*>(imm) ||
		    (dynamic_cast<Road const*>(imm) && (fse.field->nodecaps() & BUILDCAPS_FLAG))) {
			field.preferred_ = true;
		}
	}

	for (const ImmovableFound& temp_immovable : immovables) {
		if (upcast(Building const, bld, temp_immovable.object)) {
			if (bld->descr().get_ismine()) {
				++field.mines_nearby_;
			} else if (upcast(ConstructionSite const, cs, bld)) {
				if (cs->building().get_ismine()) {
					++field.mines_nearby_;
				}
			}
		}
	}
}

/// Updates the production and MINE sites statistics needed for construction decision.
void DefaultAI::update_productionsite_stats(int32_t const gametime) {
	// Updating the stats every 10 seconds should be enough
	next_stats_update_due_ = gametime + 10000;
	uint16_t fishers_count = 0;  // used for atlanteans only

	// Reset statistics for all buildings
	for (uint32_t i = 0; i < buildings_.size(); ++i) {
		if (buildings_.at(i).cnt_built_ > 0) {
			buildings_.at(i).current_stats_ = 0;
		} else {
			buildings_.at(i).current_stats_ = 0;
		}

		buildings_.at(i).unoccupied_ = false;
	}

	// Check all available productionsites
	for (uint32_t i = 0; i < productionsites.size(); ++i) {
		assert(productionsites.front().bo->cnt_built_ > 0);
		// Add statistics value
		productionsites.front().bo->current_stats_ +=
		   productionsites.front().site->get_crude_statistics();

		// counting fishers
		if (productionsites.front().bo->is_fisher_) {
			fishers_count += 1;
		}

		// Check whether this building is completely occupied
		productionsites.front().bo->unoccupied_ |= !productionsites.front().site->can_start_working();

		// Now reorder the buildings
		productionsites.push_back(productionsites.front());
		productionsites.pop_front();
	}

	if (resource_necessity_water_needed_) {
		if (fishers_count == 0) {
			resource_necessity_water_ = 255;
		} else if (fishers_count == 1) {
			resource_necessity_water_ = 150;
		} else {
			resource_necessity_water_ = 18;
		}
	}

	// for mines_ also
	// Check all available mines
	for (uint32_t i = 0; i < mines_.size(); ++i) {
		assert(mines_.front().bo->cnt_built_ > 0);
		// Add statistics value
		mines_.front().bo->current_stats_ += mines_.front().site->get_statistics_percent();
		// Check whether this building is completely occupied
		mines_.front().bo->unoccupied_ |= !mines_.front().site->can_start_working();
		// Now reorder the buildings
		mines_.push_back(mines_.front());
		mines_.pop_front();
	}

	// Scale statistics down
	for (uint32_t i = 0; i < buildings_.size(); ++i) {
		if (buildings_.at(i).cnt_built_ > 0) {
			buildings_.at(i).current_stats_ /= buildings_.at(i).cnt_built_;
		}
	}
}

// * Constructs the most needed building
//   algorithm goes over all available spots and all allowed buildings,
//   scores every combination and one with highest and positive score
//   is built.
// * Buildings are split into categories
// * The logic is complex but aproximatelly:
// - buildings producing building material are preffered
// - buildings identified as basic are preffered
// - first bulding of a type is preffered
// - buildings identified as 'direct food supplier' as built after 15 min.
//   from game start
// - if a bulding is upgradeable, second building is also preffered
//   (there should be no upgrade when there are not two buildings of the same type)
// - algorigthm is trying to take into account actual utlization of buildings
//   (the one shown in GUI/game is not reliable, it calculates own statistics)
// * military buildings have own strategy, split into two situations:
// - there is no enemy
// - there is an enemy
//   Currently more military buildings are built then needed
//   and "optimalization" (dismantling not needed buildings) is done afterwards
bool DefaultAI::construct_building(int32_t gametime) {  // (int32_t gametime)
	//  Just used for easy checking whether a mine or something else was built.
	bool mine = false;
	bool field_blocked = false;
	uint32_t consumers_nearby_count = 0;
	std::vector<int32_t> spots_avail;
	spots_avail.resize(4);
	// uint16_t const pn = player_number();

	for (int32_t i = 0; i < 4; ++i)
		spots_avail.at(i) = 0;

	for (std::list<BuildableField*>::iterator i = buildable_fields.begin();
	     i != buildable_fields.end();
	     ++i)
		++spots_avail.at((*i)->coords.field->nodecaps() & BUILDCAPS_SIZEMASK);

	spots_ = spots_avail.at(BUILDCAPS_SMALL);
	spots_ += spots_avail.at(BUILDCAPS_MEDIUM);
	spots_ += spots_avail.at(BUILDCAPS_BIG);

	// here we possible stop building of new buildings
	new_buildings_stop_ = false;
	uint8_t expansion_mode = kResourcesOrDefense;

	// there are many reasons why to stop building production buildings
	// (note there are numerous exceptions)
	// 1. to not have too many constructionsites
	if (num_prod_constructionsites > productionsites.size() / 7 + 2) {
		new_buildings_stop_ = true;
	}
	// 2. to not exhaust all free spots
	if (spots_ * 3 / 2 + 5 < static_cast<int32_t>(productionsites.size())) {
		new_buildings_stop_ = true;
	}
	// 3. too keep some proportions production sites vs military sites
	if ((num_prod_constructionsites + productionsites.size()) >
	    (num_milit_constructionsites + militarysites.size()) * 3) {
		new_buildings_stop_ = true;
	}
	// 4. if we do not have 3 mines at least
	if (mines_.size() < 3) {
		new_buildings_stop_ = true;
	}
	// BUT if enemy is nearby, we cancel above stop
	if (new_buildings_stop_ && enemy_last_seen_ + 2 * 60 * 1000 > gametime) {
		new_buildings_stop_ = false;
	}

	// sometimes there is too many military buildings in construction, so we must
	// prevent initialization of further buildings start
	const uint32_t treshold = militarysites.size() / 40 + 2;

	if (unstationed_milit_buildings_ + num_milit_constructionsites > 3 * treshold) {
		expansion_mode = kNoNewMilitary;
	} else if (unstationed_milit_buildings_ + num_milit_constructionsites > 2 * treshold) {
		expansion_mode = kDefenseOnly;
	} else if (unstationed_milit_buildings_ + num_milit_constructionsites >= 1) {
		expansion_mode = kResourcesOrDefense;
	} else {
		expansion_mode = kPushExpansion;
	}

	// we must consider need for mines
	// set necessity for mines
	// we use 'virtual mines', because also mine spots can be changed
	// to mines when AI decides so
	const int32_t virtual_mines = mines_.size() + mineable_fields.size() / 10;
	if (virtual_mines <= 5) {
		resource_necessity_mines_ = std::numeric_limits<uint8_t>::max();
	} else if (virtual_mines > 14) {
		resource_necessity_mines_ = 0;
	} else {
		const uint32_t tmp = ((16 - virtual_mines) * 255) / 12;
		resource_necessity_mines_ = tmp;
	}

	// here we calculate a need for expansion and reduce necessity for new land
	// the game has two stages:
	// First: virtual mines<=5 - stage of building the economics
	// Second: virtual mines>5 - teritorial expansion
	if (virtual_mines <= 5) {
		if (spots_avail.at(BUILDCAPS_BIG) <= 4) {
			resource_necessity_territory_ = 255;
		} else {
			resource_necessity_territory_ = 0;
		}
	} else {  // or we have enough mines and regulate speed of expansion
		if (spots_ == 0) {
			resource_necessity_territory_ = 255;
		} else {
			const uint32_t tmp = 255 * 4 * productionsites.size() / spots_;
			if (tmp > 255) {
				resource_necessity_territory_ = 255;
			} else {
				resource_necessity_territory_ = tmp;
			}
		}
	}

	BuildingObserver* best_building = nullptr;
	int32_t proposed_priority = 0;
	Coords proposed_coords;

	// Remove outdated fields from blocker list
	for (std::list<BlockedField>::iterator i = blocked_fields.begin(); i != blocked_fields.end();)
		if (i->blocked_until_ < game().get_gametime()) {
			i = blocked_fields.erase(i);
		} else {
			++i;
		}

	// these are 3 helping variables
	bool output_is_needed = false;
	int16_t max_preciousness = 0;         // preciousness_ of most precious output
	int16_t max_needed_preciousness = 0;  // preciousness_ of most precious NEEDED output

	// first scan all buildable fields for regular buildings
	for (std::list<BuildableField*>::iterator i = buildable_fields.begin();
	     i != buildable_fields.end();
	     ++i) {
		BuildableField* const bf = *i;

		// if 'buildable field' update is overdue for more then 8 seconds
		// (= bf has not been updated for about 15 seconds)
		// skip the bf in evaluation, bacause information
		// contained in bf are too old
		if (bf->next_update_due_ < gametime - 8000) {
			continue;
		}

		// Continue if field is blocked at the moment
		field_blocked = false;

		for (std::list<BlockedField>::iterator j = blocked_fields.begin(); j != blocked_fields.end();
		     ++j) {
			if (j->coords == bf->coords) {
				field_blocked = true;
			}
		}

		// continue;
		if (field_blocked) {
			continue;
		}

		assert(player_);
		int32_t const maxsize = player_->get_buildcaps(bf->coords) & BUILDCAPS_SIZEMASK;

		// For every field test all buildings
		for (uint32_t j = 0; j < buildings_.size(); ++j) {
			BuildingObserver& bo = buildings_.at(j);

			if (!bo.buildable(*player_)) {
				continue;
			}

			if (bo.prohibited_till_ > gametime) {
				continue;
			}

			// if current field is not big enough
			if (bo.desc->get_size() > maxsize) {
				continue;
			}

			if (time(nullptr) % 3 == 0 && bo.total_count() > 0) {
				continue;
			}  // add randomnes and ease AI

			if (bo.type == BuildingObserver::MINE) {
				continue;
			}

			// here we do an exemption for lumberjacks, mainly in early stages of game
			// sometimes the first one is not built and AI waits too long for second attempt
			if (gametime - bo.construction_decision_time_ < kBuildingMinInterval && !bo.need_trees_) {
				continue;
			}

			if (bo.unoccupied_) {
				continue;
			}

			if (!(bo.type == BuildingObserver::MILITARYSITE) && bo.cnt_under_construction_ >= 2) {
				continue;
			}

			// so we are going to seriously evaluate this building on this field,
			// first some base info - is its output needed at all?
			check_ware_necessity(bo, &output_is_needed, &max_preciousness, &max_needed_preciousness);

			int32_t prio = 0;  // score of a bulding on a field

			if (bo.type == BuildingObserver::PRODUCTIONSITE) {

				// exclude spots on border
				if (bf->near_border_ && !bo.need_trees_ && !bo.need_stones_ && !bo.is_fisher_) {
					continue;
				}

				// this can be only a well (as by now)
				if (bo.mines_water_) {
					if (bf->ground_water_ < 2) {
						continue;
					}

					if (bo.cnt_under_construction_ + bo.unoccupied_ > 0) {
						continue;
					}

					prio = 0;
					// one well is forced
					if (bo.total_count() == 0) {
						prio = 200;
					}  // boost for first/only well
					else if (new_buildings_stop_) {
						continue;
					}

					bo.cnt_target_ = 1 + productionsites.size() / 50;

					if (bo.stocklevel_time < game().get_gametime() - 30 * 1000) {
						bo.stocklevel_ = get_stocklevel(bo);
						bo.stocklevel_time = game().get_gametime();
					}
					if (bo.stocklevel_ > 40) {
						continue;
					}
					prio += bf->ground_water_ - 2;
					prio = recalc_with_border_range(*bf, prio);

				} else if (bo.need_trees_) {  // LUMBERJACS

					bo.cnt_target_ =
					   3 + static_cast<int32_t>(mines_.size() + productionsites.size()) / 15;

					if (bo.total_count() == 0) {
						prio = 500 + bf->trees_nearby_;
					}

					else if (bo.total_count() == 1) {
						prio = 400 + bf->trees_nearby_;
					}

					else if (bf->trees_nearby_ < 2) {
						continue;
					}

					else {

						if (bo.total_count() < bo.cnt_target_) {
							prio = 75;
						} else {
							prio = 0;
						}

						if (bf->producers_nearby_.at(bo.outputs_.at(0)) > 1) {
							continue;
						}

						prio += 2 * bf->trees_nearby_ - 10 -
						        bf->producers_nearby_.at(bo.outputs_.at(0)) * 5 -
						        new_buildings_stop_ * 15;

						if (bf->near_border_) {
							prio = prio / 2;
						}
					}

				} else if (bo.need_stones_) {

					// quaries are generally to be built everywhere where stones are
					// no matter the need for stones, as stones are considered an obstacle
					// to expansion
					if (bo.cnt_under_construction_ > 0) {
						continue;
					}
					prio = bf->stones_nearby_;

					if (prio <= 0) {
						continue;
					}

					if (bo.total_count() == 0) {
						prio += 150;
					}

					if (bo.stocklevel_time < game().get_gametime() - 5 * 1000) {
						bo.stocklevel_ = get_stocklevel_by_hint(static_cast<size_t>(bo.production_hint_));
						bo.stocklevel_time = game().get_gametime();
					}

					if (bo.stocklevel_ == 0) {
						prio *= 2;
					}

					// to prevent to many quaries on one spot
					prio = prio - 50 * bf->producers_nearby_.at(bo.outputs_.at(0));

					if (bf->near_border_) {
						prio = prio / 2;
					}

				} else if (bo.is_hunter_) {
					if (bf->critters_nearby_ < 5) {
						continue;
					}

					if (new_buildings_stop_) {
						continue;
					}

					prio +=
					   (bf->critters_nearby_ * 2) - 8 - 5 * bf->producers_nearby_.at(bo.outputs_.at(0));

				} else if (bo.is_fisher_) {  // fisher

					// ~are fishes needed?
					if (max_needed_preciousness == 0) {
						continue;
					}

					if (bo.cnt_under_construction_ + bo.unoccupied_ > 0) {
						continue;
					}

					if (bf->water_nearby_ < 2) {
						continue;
					}

					// we use preciousness to allow atlanteans to build the fishers huts
					// atlanteans have preciousness 4, other tribes 3
					if (max_needed_preciousness < 4 && new_buildings_stop_) {
						continue;
					}

					if (bo.stocklevel_time < game().get_gametime() - 5 * 1000) {
						bo.stocklevel_ = get_stocklevel_by_hint(static_cast<size_t>(bo.production_hint_));
						bo.stocklevel_time = game().get_gametime();
					}

					if (bo.stocklevel_ > 50) {
						continue;
					}

					if (bf->producers_nearby_.at(bo.outputs_.at(0)) >= 1) {
						continue;
					}

					prio = bf->fish_nearby_ - new_buildings_stop_ * 15 * bo.total_count();

				} else if (bo.production_hint_ >= 0) {
					// first setting targets (needed also for dismantling)
					if (bo.plants_trees_) {
						bo.cnt_target_ =
						   2 + static_cast<int32_t>(mines_.size() + productionsites.size()) / 15;
					} else {
						bo.cnt_target_ =
						   1 + static_cast<int32_t>(mines_.size() + productionsites.size()) / 20;
					}

					if ((bo.cnt_under_construction_ + bo.unoccupied_) > 1) {
						continue;
					}

					if (bo.plants_trees_) {  // RANGERS

						// if there are too many trees nearby
						if (bf->trees_nearby_ > 25 && bo.total_count() >= 1) {
							continue;
						}

						// sometimes all area is blocked by trees so this is to prevent this
						if (buildable_fields.size() < 4) {
							continue;
						}

						if (bo.stocklevel_time < game().get_gametime() - 5 * 1000) {
							bo.stocklevel_ =
							   get_stocklevel_by_hint(static_cast<size_t>(bo.production_hint_));
							bo.stocklevel_time = game().get_gametime();
						}

						if (bo.total_count() == 0) {
							prio = 200;
						}
						if (bo.total_count() > 2 * bo.cnt_target_) {
							continue;
						}
						// we can go above target if there is shortage of logs on stock
						else if (bo.total_count() >= bo.cnt_target_ && bo.stocklevel_ > 40) {
							continue;
						}

						// considering near trees and producers
						prio += (30 - bf->trees_nearby_) * 2 +
						        bf->producers_nearby_.at(bo.production_hint_) * 5 -
						        new_buildings_stop_ * 15;

						// considering space consumers nearby
						prio -= bf->space_consumers_nearby_ * 5;

					} else {  // FISH BREEDERS and GAME KEEPERS
						if (new_buildings_stop_ && bo.total_count() > 0) {
							continue;
						}

						// especially for fish breeders
						if (bo.need_water_ && bf->water_nearby_ < 2) {
							continue;
						}
						if (bo.need_water_) {
							prio += bf->water_nearby_ / 5;
						}

						if (bo.total_count() > bo.cnt_target_) {
							continue;
						}

						if (bo.stocklevel_time < game().get_gametime() - 5 * 1000) {
							bo.stocklevel_ =
							   get_stocklevel_by_hint(static_cast<size_t>(bo.production_hint_));
							bo.stocklevel_time = game().get_gametime();
						}
						if (bo.stocklevel_ > 50) {
							continue;
						}

						if (bo.total_count() == 0 && gametime > 45 * 1000) {
							prio += 100 + bf->producers_nearby_.at(bo.production_hint_) * 10;
						} else if (bf->producers_nearby_.at(bo.production_hint_) == 0) {
							continue;
						} else {
							prio += bf->producers_nearby_.at(bo.production_hint_) * 10;
						}

						if (bf->enemy_nearby_) {
							prio -= 10;
						}
					}

				} else if (bo.recruitment_ && !new_buildings_stop_) {
					// this will depend on number of mines_ and productionsites
					if (static_cast<int32_t>((productionsites.size() + mines_.size()) / 30) >
					       bo.total_count() &&
					    bo.cnt_under_construction_ == 0)
						prio = 4 + kDefaultPrioBoost;
				} else {  // finally normal productionsites
					if (bo.production_hint_ >= 0) {
						continue;
					}

					if ((bo.cnt_under_construction_ + bo.unoccupied_) > 0) {
						continue;
					}

					if (bo.forced_after_ < gametime && bo.total_count() == 0) {
						prio += 150;
					} else if (bo.cnt_built_ == 1 && game().get_gametime() > 40 * 60 * 1000 &&
					           bo.desc->enhancement() != INVALID_INDEX && !mines_.empty()) {
						prio += 10;
					} else if (!output_is_needed) {
						continue;
					} else if (bo.cnt_built_ == 0 && game().get_gametime() > 40 * 60 * 1000) {
						prio += kDefaultPrioBoost;
					} else if (bo.cnt_built_ > 1 && bo.current_stats_ > 97) {
						prio -= kDefaultPrioBoost * (new_buildings_stop_);
					} else if (new_buildings_stop_)
						continue;

					// we check separatelly buildings with no inputs and some inputs
					if (bo.inputs_.empty()) {

						prio += max_needed_preciousness + kDefaultPrioBoost;

						if (bo.space_consumer_) {  // need to consider trees nearby
							prio += 20 - (bf->trees_nearby_ / 3);
						}

						// we attempt to cluster space consumers together
						if (bo.space_consumer_) {  // need to consider trees nearby
							prio += bf->space_consumers_nearby_ * 2;
						}

						if (bo.space_consumer_ && !bf->water_nearby_) {  // not close to water
							prio += 1;
						}

						if (bo.space_consumer_ &&
						    !bf->unowned_mines_pots_nearby_) {  // not close to mountains
							prio += 1;
						}

						if (!bo.space_consumer_) {
							prio -= bf->producers_nearby_.at(bo.outputs_.at(0)) * 20;
						}  // leave some free space between them

						prio -= bf->space_consumers_nearby_ * 3;
					}

					if (!bo.inputs_.empty()) {
						if (bo.total_count() == 0) {
							prio += max_needed_preciousness + kDefaultPrioBoost;
						}
						if (bo.cnt_built_ > 0 && bo.current_stats_ > 70) {
							prio += max_needed_preciousness + kDefaultPrioBoost - 3 +
							        (bo.current_stats_ - 70) / 5;
						}
					}

					if (prio <= 0) {
						continue;
					}

					//+1 if any consumers_ are nearby
					consumers_nearby_count = 0;

					for (size_t k = 0; k < bo.outputs_.size(); ++k)
						consumers_nearby_count += bf->consumers_nearby_.at(bo.outputs_.at(k));

					if (consumers_nearby_count > 0) {
						prio += 1;
					}
				}
			}  // production sites done
			else if (bo.type == BuildingObserver::MILITARYSITE) {

				if (!bf->unowned_land_nearby_) {
					continue;
				}

				if (military_last_build_ > gametime - 10 * 1000) {
					continue;
				}

				if (expansion_mode == kNoNewMilitary) {
					continue;
				}

				if (expansion_mode == kDefenseOnly && !bf->enemy_nearby_) {
					continue;
				}

				if (bf->enemy_nearby_ && bo.fighting_type_) {
					;
				}  // it is ok, go on
				else if (bf->unowned_mines_pots_nearby_ > 2 &&
				         (bo.mountain_conqueror_ || bo.expansion_type_)) {
					;
				}  // it is ok, go on
				else if (bf->unowned_land_nearby_ && bo.expansion_type_ &&
				         num_milit_constructionsites <= 1) {
					{;}  // we allow big buildings now
				} else if (bf->unowned_land_nearby_ &&
				           bo.expansion_type_) {  // decreasing probability for big buidlings
					if (bo.desc->get_size() == 2 && gametime % 15 >= 1) {
						continue;
					}
					if (bo.desc->get_size() == 3 && gametime % 40 >= 1) {
						continue;
					}
				}
				// it is ok, go on
				else {
					continue;
				}  // the building is not suitable for situation

				// not to build so many military buildings nearby
				if (!bf->enemy_nearby_ && bf->military_in_constr_nearby_ > 0) {
					continue;
				}

				// a boost to prevent an expansion halt
				int32_t local_boost = 0;
				if (expansion_mode == kPushExpansion) {
					local_boost = 200;
				}

				prio = (bf->unowned_land_nearby_ * 2 * resource_necessity_territory_ / 255 +
				        bf->unowned_mines_pots_nearby_ * resource_necessity_mines_ / 255 +
				        bf->stones_nearby_ / 2 + bf->military_loneliness_ / 10 - 60 + local_boost +
				        bf->water_nearby_ * resource_necessity_water_ / 255);

				// special bonus due to remote water for atlanteans
				if (resource_necessity_water_needed_)
					prio += bf->distant_water_ * resource_necessity_water_ / 255;

				if (bo.desc->get_size() < maxsize) {
					prio = prio - 5;
				}  // penalty

				// we need to prefer military building near to borders
				// with enemy
				if (bf->enemy_nearby_ && bf->military_capacity_ < 12) {
					prio += 50 + (12 - bf->military_capacity_) * 20;
				}

			} else if (bo.type == BuildingObserver::WAREHOUSE) {

				// exclude spots on border
				if (bf->near_border_) {
					continue;
				}

				//  Build one warehouse for ~every 35 productionsites and mines_.
				//  Militarysites are slightly important as well, to have a bigger
				//  chance for a warehouses (containing waiting soldiers or wares
				//  needed for soldier training) near the frontier.
				if ((static_cast<int32_t>(productionsites.size() + mines_.size()) + 20) / 35 >
				       static_cast<int32_t>(numof_warehouses_) &&
				    bo.cnt_under_construction_ == 0) {
					prio = 20;
				}

				// take care about borders and enemies
				if (bf->enemy_nearby_) {
					prio /= 2;
				}

				if (bf->unowned_land_nearby_) {
					prio /= 2;
				}

				// TODO(unknown): introduce check that there is no warehouse nearby
				// to prevent too close placing

			} else if (bo.type == BuildingObserver::TRAININGSITE) {

				// exclude spots on border
				if (bf->near_border_) {
					continue;
				}

				// build after 20 production sites and then after each 50 production site
				if (static_cast<int32_t>((productionsites.size() + 30) / 50) > bo.total_count() &&
				    bo.cnt_under_construction_ == 0) {
					prio = 4 + kDefaultPrioBoost;
				}

				// take care about borders and enemies
				if (bf->enemy_nearby_) {
					prio /= 2;
				}

				if (bf->unowned_land_nearby_) {
					prio /= 2;
				}
			}

			// think of space consuming buildings nearby like farms or vineyards
			prio -= bf->space_consumers_nearby_ * 10;

			// Stop here, if priority is 0 or less.
			if (prio <= 0) {
				continue;
			}

			// Prefer road side fields
			prio += bf->preferred_ ? 1 : 0;
			// don't waste good land for small huts
			prio -= (maxsize - bo.desc->get_size()) * 5;

			if (prio > proposed_priority) {
				best_building = &bo;
				proposed_priority = prio;
				proposed_coords = bf->coords;
			}
		}  // ending loop over buildings
	}     // ending loop over fields

	// then try all mines_ - as soon as basic economy is build up.
	if (gametime > next_mine_construction_due_) {

		update_all_mineable_fields(gametime);
		next_mine_construction_due_ = gametime + kIdleMineUpdateInterval;

		if (!mineable_fields.empty()) {

			for (uint32_t i = 0; i < buildings_.size() && productionsites.size() > 8; ++i) {
				BuildingObserver& bo = buildings_.at(i);

				if (!bo.buildable(*player_) || bo.type != BuildingObserver::MINE) {
					continue;
				}

				if (bo.prohibited_till_ > gametime) {
					continue;
				}

				if (gametime - bo.construction_decision_time_ < kBuildingMinInterval) {
					continue;
				}

				// Don't build another building of this type, if there is already
				// one that is unoccupied_ at the moment
				// or under construction
				if ((bo.cnt_under_construction_ + bo.unoccupied_) > 0) {
					continue;
				}

				// testing if building's output is needed
				check_ware_necessity(
				   bo, &output_is_needed, &max_preciousness, &max_needed_preciousness);

				if (!output_is_needed && bo.total_count() > 0) {
					continue;
				}

				// if current one(s) are performing badly
				if (bo.total_count() >= 1 && bo.current_stats_ < 50) {
					continue;
				}

				// this is penalty if there are existing mines too close
				// it is treated as multiplicator for count of near mines
				uint32_t nearness_penalty = 0;
				if ((bo.cnt_built_ + bo.cnt_under_construction_) == 0) {
					nearness_penalty = 0;
				} else {
					nearness_penalty = 10;
				}

				// iterating over fields
				for (std::list<MineableField*>::iterator j = mineable_fields.begin();
				     j != mineable_fields.end();
				     ++j) {

					if ((*j)->coords.field->get_resources() != bo.mines_) {
						continue;
					}

					int32_t prio = (*j)->coords.field->get_resources_amount();

					// applying nearnes penalty
					prio = prio - (*j)->mines_nearby_ * nearness_penalty;

					// Only build mines_ on locations where some material can be mined
					if (prio < 2) {
						continue;
					}

					// Continue if field is blocked at the moment
					bool blocked = false;

					for (std::list<BlockedField>::iterator k = blocked_fields.begin();
					     k != blocked_fields.end();
					     ++k)
						if ((*j)->coords == k->coords) {
							blocked = true;
							break;
						}

					if (blocked) {

						continue;
					}

					// Prefer road side fields
					prio += (*j)->preferred_ ? 1 : 0;

					if (prio > proposed_priority) {
						// proposed_building = bo.id;
						best_building = &bo;
						proposed_priority = prio;
						proposed_coords = (*j)->coords;
						mine = true;
					}
				}  // end of evaluation of field
			}

		}  // section if mine size >0
	}     // end of mines_ section

	// if there is no winner:
	if (best_building == nullptr) {
		return false;
	}

	// send the command to construct a new building
	game().send_player_build(player_number(), proposed_coords, best_building->id);
	BlockedField blocked(
	   game().map().get_fcoords(proposed_coords), game().get_gametime() + 120000);  // two minutes
	blocked_fields.push_back(blocked);

	// we block also nearby fields
	// if farms and so on, for quite a long time
	// if military sites only for short time for AI can update information on near buildable fields
	if ((best_building->space_consumer_ && !best_building->plants_trees_) ||
	    best_building->type == BuildingObserver::MILITARYSITE) {
		uint32_t block_time = 0;
		uint32_t block_area = 0;
		if (best_building->space_consumer_) {
			block_time = 45 * 60 * 1000;
			block_area = 3;
		} else {  // militray buildings for a very short time
			block_time = 25 * 1000;
			block_area = 6;
		}
		Map& map = game().map();

		MapRegion<Area<FCoords>> mr(map, Area<FCoords>(map.get_fcoords(proposed_coords), block_area));
		do {
			BlockedField blocked2(
			   map.get_fcoords(*(mr.location().field)), game().get_gametime() + block_time);
			blocked_fields.push_back(blocked2);
		} while (mr.advance(map));
	}

	if (!(best_building->type == BuildingObserver::MILITARYSITE)) {
		best_building->construction_decision_time_ = gametime;
	} else {  // very ugly hack here
		military_last_build_ = gametime;
		best_building->construction_decision_time_ = gametime - kBuildingMinInterval / 2;
	}

	// set the type of update that is needed
	if (mine) {
		next_mine_construction_due_ = gametime + kBusyMineUpdateInterval;

	} else {
		m_buildable_changed = true;
	}

	return true;
}

// improves current road system
bool DefaultAI::improve_roads(int32_t gametime) {

	// first force a split on roads that are longer than 3 parts
	// with exemption when there is too few building spots
	if (spots_ > 20 && !roads.empty()) {
		const Path& path = roads.front()->get_path();

		if (path.get_nsteps() > 3) {
			const Map& map = game().map();
			CoordPath cp(map, path);
			// try to split after two steps
			CoordPath::StepVector::size_type i = cp.get_nsteps() - 1, j = 1;

			for (; i >= j; --i, ++j) {
				{
					const Coords c = cp.get_coords().at(i);

					if (map[c].nodecaps() & BUILDCAPS_FLAG) {
						game().send_player_build_flag(player_number(), c);
						return true;
					}
				}
				{
					const Coords c = cp.get_coords().at(j);

					if (map[c].nodecaps() & BUILDCAPS_FLAG) {
						game().send_player_build_flag(player_number(), c);
						return true;
					}
				}
			}

			// Unable to set a flag - perhaps the road was build stupid
			game().send_player_bulldoze(*const_cast<Road*>(roads.front()));
		}

		roads.push_back(roads.front());
		roads.pop_front();

		// occasionaly we test if the road can be dismounted
		if (gametime % 25 == 0) {
			const Road& road = *roads.front();
			if (dispensable_road_test(*const_cast<Road*>(&road))) {
				game().send_player_bulldoze(*const_cast<Road*>(&road));
				return true;
			}
		}
	}

	if (inhibit_road_building_ >= gametime) {
		return false;
	}

	// now we rotate economies and flags to get one flag to go on with
	if (economies.empty()) {
		return check_economies();
	}

	if (economies.size() >= 2) {  // rotating economies
		economies.push_back(economies.front());
		economies.pop_front();
	}

	EconomyObserver* eco = economies.front();
	if (eco->flags.empty()) {
		return check_economies();
	}
	if (eco->flags.size() > 1) {
		eco->flags.push_back(eco->flags.front());
		eco->flags.pop_front();
	}

	const Flag& flag = *eco->flags.front();

	// now we test if it is dead end flag, if yes, destroying it
	if (flag.is_dead_end() && flag.current_wares() == 0) {
		game().send_player_bulldoze(*const_cast<Flag*>(&flag));
		eco->flags.pop_front();
		return true;
	}

	// if this is end flag (or sole building) or just randomly
	if (flag.nr_of_roads() <= 1 || gametime % 200 == 0) {
		create_shortcut_road(flag, 13, 20);
		inhibit_road_building_ = gametime + 800;
	}
	// this is when a flag is full
	else if (flag.current_wares() > 6 && gametime % 10 == 0) {
		create_shortcut_road(flag, 9, 0);
		inhibit_road_building_ = gametime + 400;
	}

	return false;
}

// the function takes a road (road is smallest section of roads with two flags on the ends)
// and tries to find alternative route from one flag to another.
// if route exists, it is not too long, and current road is not intensively used
// the road can be dismantled
bool DefaultAI::dispensable_road_test(const Road& road) {

	Flag& roadstartflag = road.get_flag(Road::FlagStart);
	Flag& roadendflag = road.get_flag(Road::FlagEnd);

	if (roadstartflag.current_wares() > 0 || roadendflag.current_wares() > 0) {
		return false;
	}

	std::priority_queue<NearFlag> queue;
	// only used to collect flags reachable walking over roads
	std::vector<NearFlag> reachableflags;
	queue.push(NearFlag(roadstartflag, 0, 0));
	uint8_t pathcounts = 0;
	uint8_t checkradius = 8;
	Map& map = game().map();

	// algorithm to walk on roads
	while (!queue.empty()) {

		// testing if we stand on the roadendflag
		// if is is for first time, just go on,
		// if second time, the goal is met, function returns true
		if (roadendflag.get_position().x == queue.top().flag->get_position().x &&
		    roadendflag.get_position().y == queue.top().flag->get_position().y) {
			pathcounts += 1;
			if (pathcounts > 1) {
				// OK, this is a second route how to get to roadendflag
				return true;
			}
			queue.pop();
			continue;
		}

		std::vector<NearFlag>::iterator f =
		   find(reachableflags.begin(), reachableflags.end(), queue.top().flag);

		if (f != reachableflags.end()) {
			queue.pop();
			continue;
		}

		reachableflags.push_back(queue.top());
		queue.pop();
		NearFlag& nf = reachableflags.back();

		for (uint8_t i = 1; i <= 6; ++i) {
			Road* const near_road = nf.flag->get_road(i);

			if (!near_road) {
				continue;
			}

			Flag* endflag = &near_road->get_flag(Road::FlagStart);

			if (endflag == nf.flag) {
				endflag = &near_road->get_flag(Road::FlagEnd);
			}

			int32_t dist = map.calc_distance(roadstartflag.get_position(), endflag->get_position());

			if (dist > checkradius) {  //  out of range of interest
				continue;
			}

			queue.push(NearFlag(*endflag, 0, dist));
		}
	}
	return false;
}

// trying to connect the flag to another one, be it from own economy
// or other economy
bool DefaultAI::create_shortcut_road(const Flag& flag, uint16_t checkradius, uint16_t minred) {

	// Increasing the failed_connection_tries counter
	// At the same time it indicates a time an economy is without a warehouse
	EconomyObserver* eco = get_economy_observer(flag.economy());
	if (flag.get_economy()->warehouses().empty()) {
		eco->failed_connection_tries += 1;
	} else {
		eco->failed_connection_tries = 0;
	}

	// explanation for 'eco->flags.size() * eco->flags.size()'
	// The AI is able to dismantle whole economy without warehouse as soon as single
	// building not connected anywhere. But so fast dismantling is not deserved (probably)
	// so the bigger economy the longer it takes to be dismantled
	if (eco->failed_connection_tries > 3 + eco->flags.size() * eco->flags.size()) {

		Building* bld = flag.get_building();

		if (bld) {
			// first we block the field for 15 minutes, probably it is not good place to build a
			// building on
			BlockedField blocked(
			   game().map().get_fcoords(bld->get_position()), game().get_gametime() + 15 * 60 * 1000);
			blocked_fields.push_back(blocked);
			eco->flags.remove(&flag);
			game().send_player_bulldoze(*const_cast<Flag*>(&flag));
		}
		return true;
	}

	Map& map = game().map();

	// 1. first we collect all reachange points
	std::vector<NearFlag> nearflags;
	std::unordered_set<int32_t> lookuptable;

	FindNodeWithFlagOrRoad functor;
	CheckStepRoadAI check(player_, MOVECAPS_WALK, true);
	std::vector<Coords> reachable;

	// vector reachable now contains all suitable fields
	map.find_reachable_fields(
	   Area<FCoords>(map.get_fcoords(flag.get_position()), checkradius), &reachable, check, functor);

	if (reachable.empty()) {
		return false;
	}

	for (const Coords& reachable_coords : reachable) {

		// first make sure there is an immovable (shold be, but still)
		if (upcast(PlayerImmovable const, player_immovable, map[reachable_coords].get_immovable())) {

			// if it is the road, make a flag there
			if (dynamic_cast<const Road*>(map[reachable_coords].get_immovable())) {
				game().send_player_build_flag(player_number(), reachable_coords);
			}

			// do not go on if it is not a flag
			if (!dynamic_cast<const Flag*>(map[reachable_coords].get_immovable())) {
				continue;
			}

			// testing if a flag/road's economy has a warehouse, if not we are not
			// interested to connect to it
			if (player_immovable->economy().warehouses().size() == 0) {
				continue;
			}

			// now make sure that this field has not been processed yet
			const int32_t hash = reachable_coords.x << 16 | reachable_coords.y;
			if (lookuptable.count(hash) == 0) {
				lookuptable.insert(hash);

				// adding flag into NearFlags if road is possible
				std::unique_ptr<Path> path2(new Path());

				if (map.findpath(flag.get_position(), reachable_coords, 0, *path2, check) >= 0) {

					// path is possible, but for now we presume connection
					//'walking on existing roads' is not possible
					// so we assign 'virtual distance'
					int32_t virtual_distance = 0;
					// the same economy, but connection not spotted above via "walking on roads"
					// algorithm
					if (player_immovable->get_economy() == flag.get_economy()) {
						virtual_distance = 50;
					} else  // and now different economies
					{
						virtual_distance = 100;
					}

					// distance as the crow flies
					const int32_t dist = map.calc_distance(flag.get_position(), reachable_coords);

					nearflags.push_back(
					   NearFlag(*dynamic_cast<const Flag*>(map[reachable_coords].get_immovable()),
					            virtual_distance,
					            dist));
				}
			}
		}
	}

	// now we walk over roads and if field is reachable by roads, we change distance asigned before
	std::priority_queue<NearFlag> queue;
	std::vector<NearFlag> nearflags_tmp;  // only used to collect flags reachable walk over roads
	queue.push(NearFlag(flag, 0, 0));

	// algorithm to walk on roads
	while (!queue.empty()) {
		std::vector<NearFlag>::iterator f =
		   find(nearflags_tmp.begin(), nearflags_tmp.end(), queue.top().flag);

		if (f != nearflags_tmp.end()) {
			queue.pop();
			continue;
		}

		nearflags_tmp.push_back(queue.top());
		queue.pop();
		NearFlag& nf = nearflags_tmp.back();

		for (uint8_t i = 1; i <= 6; ++i) {
			Road* const road = nf.flag->get_road(i);

			if (!road) {
				continue;
			}

			Flag* endflag = &road->get_flag(Road::FlagStart);

			if (endflag == nf.flag) {
				endflag = &road->get_flag(Road::FlagEnd);
			}

			int32_t dist = map.calc_distance(flag.get_position(), endflag->get_position());

			if (dist > checkradius) {  //  out of range of interest
				continue;
			}

			queue.push(NearFlag(*endflag, nf.cost_ + road->get_path().get_nsteps(), dist));
		}
	}

	// iterating over nearflags_tmp, each item in nearflags_tmp should be contained also in nearflags
	// so for each corresponding field in nearflags we update "cost" (distance on existing roads)
	// to actual value
	for (std::vector<NearFlag>::iterator nf_walk_it = nearflags_tmp.begin();
	     nf_walk_it != nearflags_tmp.end();
	     ++nf_walk_it) {
		int32_t const hash_walk =
		   nf_walk_it->flag->get_position().x << 16 | nf_walk_it->flag->get_position().y;
		if (lookuptable.count(hash_walk) > 0) {
			// iterting over nearflags
			for (std::vector<NearFlag>::iterator nf_it = nearflags.begin(); nf_it != nearflags.end();
			     ++nf_it) {
				int32_t const hash =
				   nf_it->flag->get_position().x << 16 | nf_it->flag->get_position().y;
				if (hash == hash_walk) {
					// decreasing "cost" (of walking via roads)
					if (nf_it->cost_ > nf_walk_it->cost_) {
						nf_it->cost_ = nf_walk_it->cost_;
					}
				}
			}
		}
	}

	// ordering nearflags
	std::sort(nearflags.begin(), nearflags.end(), CompareShortening());

	// this is just a random number, will be used later
	int32_t random_gametime = game().get_gametime();

	// the problem here is that send_player_build_road() does not return success/failed
	// if it did, we would just test the first nearflag, then go on with further flags until
	// a road is built or nearflags are exhausted
	// but now we must just randomly pick one of nearflags
	// probabililty of picking decreases with position in nearflags
	for (uint32_t i = 0; i < nearflags.size() && i < 10; ++i) {
		NearFlag& nf = nearflags.at(i);

		// terminating looping if reduction is too low (nearflags are sorted by reduction)
		if ((nf.cost_ - nf.distance_) < minred) {
			return false;
		}

		// testing the nearflag
		// usually we allow connecting only if both flags are closer then 'checkradius-2'
		// with exeption the flag belongs to a small economy (typically a new building not connected
		// yet)
		if ((nf.cost_ - nf.distance_) >= minred && nf.distance_ >= 2 &&
		    nf.distance_ < checkradius - 2) {

			// sometimes the shortest road is not the buildable, even if map.findpath claims so
			// best so we add some randomness
			random_gametime /= 3;
			if (random_gametime % 3 > 1) {
				continue;
			}

			Path& path = *new Path();

			// value of pathcost is not important, it just indicates, that the path can be built
			const int32_t pathcost =
			   map.findpath(flag.get_position(), nf.flag->get_position(), 0, path, check);

			if (pathcost >= 0) {
				if (static_cast<int32_t>(nf.cost_ - path.get_nsteps()) > minred) {
					game().send_player_build_road(player_number(), path);
					return true;
				}
			}
			delete &path;
		}
	}

	// if all possible roads skipped
	return false;
}

/**
 * Checks if anything in one of the economies changed and takes care for these
 * changes.
 *
 * \returns true, if something was changed.
 */
bool DefaultAI::check_economies() {
	while (!new_flags.empty()) {
		const Flag& flag = *new_flags.front();
		new_flags.pop_front();
		get_economy_observer(flag.economy())->flags.push_back(&flag);
	}

	for (std::list<EconomyObserver*>::iterator obs_iter = economies.begin();
	     obs_iter != economies.end();
	     ++obs_iter) {
		// check if any flag has changed its economy
		std::list<Flag const*>& fl = (*obs_iter)->flags;

		for (std::list<Flag const*>::iterator j = fl.begin(); j != fl.end();) {
			if (&(*obs_iter)->economy != &(*j)->economy()) {
				get_economy_observer((*j)->economy())->flags.push_back(*j);
				j = fl.erase(j);
			} else {
				++j;
			}
		}

		// if there are no more flags in this economy,
		// we no longer need it's observer
		if ((*obs_iter)->flags.empty()) {
			delete *obs_iter;
			economies.erase(obs_iter);
			return true;
		}
	}
	return false;
}

/**
 * checks the first productionsite in list, takes care if it runs out of
 * resources and finally reenqueues it at the end of the list.
 *
 * \returns true, if something was changed.
 */
bool DefaultAI::check_productionsites(int32_t gametime) {
	if ((next_productionsite_check_due_ > gametime) || productionsites.empty()) {
		return false;
	}

	next_productionsite_check_due_ = gametime + 4000;

	bool changed = false;
	// Reorder and set new values; - better now because there are multiple returns in the function
	productionsites.push_back(productionsites.front());
	productionsites.pop_front();

	// Get link to productionsite that should be checked
	ProductionSiteObserver& site = productionsites.front();

	// first we werify if site is working yet (can be unoccupied since the start)
	if (!site.site->can_start_working()) {
		site.unoccupied_till_ = game().get_gametime();
	}

	// do not dismantle or upgrade the same type of building too soon - to give some time to update
	// statistics
	if (site.bo->last_dismantle_time_ > game().get_gametime() - 30 * 1000) {
		return false;
	}

	// Get max radius of recursive workarea
	WorkareaInfo::size_type radius = 0;
	const WorkareaInfo& workarea_info = site.bo->desc->m_workarea_info;
	for (const std::pair<uint32_t, std::set<std::string>>& temp_info : workarea_info) {
		if (radius < temp_info.first) {
			radius = temp_info.first;
		}
	}

	Map& map = game().map();

	// first we try to upgrade
	// Upgrading policy
	// a) if there are two buildings and none enhanced and there are workers
	// available, one is to be enhanced
	// b) if there are two buildings
	// statistics percents are decisive
	const BuildingIndex enhancement = site.site->descr().enhancement();
	if (enhancement != INVALID_INDEX && (site.bo->cnt_built_ - site.bo->unoccupied_) > 1) {

		BuildingIndex enbld = INVALID_INDEX;  // to get rid of this

		// Only enhance buildings that are allowed (scenario mode)
		// do not do decisions to fast
		if (player_->is_building_type_allowed(enhancement)) {

			const BuildingDescr& bld = *tribe_->get_building_descr(enhancement);
			BuildingObserver& en_bo = get_building_observer(bld.name().c_str());
			BuildingObserver* bestbld = nullptr;

			if (gametime - en_bo.construction_decision_time_ >= kBuildingMinInterval &&
			    (en_bo.cnt_under_construction_ + en_bo.unoccupied_) == 0) {

				// don't upgrade without workers
				if (site.site->has_workers(enhancement, game())) {

					// forcing first upgrade
					if (en_bo.cnt_built_ == 0 && !mines_.empty()) {
						enbld = enhancement;
						bestbld = &en_bo;
					}

					// if the decision was not made yet, consider normal upgrade
					if (enbld == INVALID_INDEX) {
						// compare the performance %
						if (static_cast<int32_t>(en_bo.current_stats_) -
						       static_cast<int32_t>(site.bo->current_stats_) >
						    20) {

							enbld = enhancement;
							bestbld = &en_bo;
						}
					}
				}
			}

			// Enhance if enhanced building is useful
			// additional: we dont want to lose the old building
			if (enbld != INVALID_INDEX) {
				game().send_player_enhance_building(*site.site, enbld);
				bestbld->construction_decision_time_ = gametime;

				return true;
			}
		}
	}

	// Lumberjack / Woodcutter handling
	if (site.bo->need_trees_) {

		// Do not destruct the last few lumberjacks
		if (site.bo->cnt_built_ <= site.bo->cnt_target_) {
			return false;
		}

		if (site.site->get_statistics_percent() > 20) {
			return false;
		}

		const uint32_t remaining_trees =
		   map.find_immovables(Area<FCoords>(map.get_fcoords(site.site->get_position()), radius),
		                       nullptr,
		                       FindImmovableAttribute(MapObjectDescr::get_attribute_id("tree")));

		// do not dismantle if there are some trees remaining
		if (remaining_trees > 5) {
			return false;
		}

		if (site.bo->stocklevel_time < game().get_gametime() - 10 * 1000) {
			site.bo->stocklevel_ = get_stocklevel(*site.bo);
			site.bo->stocklevel_time = game().get_gametime();
		}

		// if we need wood badly
		if (remaining_trees > 0 && site.bo->stocklevel_ <= 50) {
			return false;
		}

		// so finally we dismantle the lumberjac
		site.bo->last_dismantle_time_ = game().get_gametime();
		flags_to_be_removed.push_back(site.site->base_flag().get_position());
		game().send_player_dismantle(*site.site);

		return true;
	}

	// Wells handling
	if (site.bo->mines_water_) {
		if (site.unoccupied_till_ + 6 * 60 * 1000 < game().get_gametime() &&
		    site.site->get_statistics_percent() == 0) {
			site.bo->last_dismantle_time_ = game().get_gametime();
			flags_to_be_removed.push_back(site.site->base_flag().get_position());
			game().send_player_dismantle(*site.site);

			return true;
		}

		// do not consider dismantling if we are under target
		if (site.bo->last_dismantle_time_ + 90 * 1000 > game().get_gametime()) {
			return false;
		}

		// now we test the stocklevel and dismantle the well if we have enough water
		// but first we make sure we do not dismantle a well too soon
		// after dismantling previous one
		if (site.bo->stocklevel_time < game().get_gametime() - 5 * 1000) {
			site.bo->stocklevel_ = get_stocklevel(*site.bo);
			site.bo->stocklevel_time = game().get_gametime();
		}
		if (site.bo->stocklevel_ > 250) {  // dismantle
			site.bo->last_dismantle_time_ = game().get_gametime();
			flags_to_be_removed.push_back(site.site->base_flag().get_position());
			game().send_player_dismantle(*site.site);
			return true;
		}

		return false;
	}

	// Quarry handling
	if (site.bo->need_stones_) {

		if (map.find_immovables(
		       Area<FCoords>(map.get_fcoords(site.site->get_position()), radius),
		       nullptr,

		       FindImmovableAttribute(MapObjectDescr::get_attribute_id("granite"))) == 0) {
			// destruct the building and it's flag (via flag destruction)
			// the destruction of the flag avoids that defaultAI will have too many
			// unused roads - if needed the road will be rebuild directly.
			flags_to_be_removed.push_back(site.site->base_flag().get_position());
			game().send_player_dismantle(*site.site);
			return true;
		}

		if (site.unoccupied_till_ + 6 * 60 * 1000 < game().get_gametime() &&
		    site.site->get_statistics_percent() == 0) {
			// it is possible that there are stones but quary is not able to mine them
			site.bo->last_dismantle_time_ = game().get_gametime();
			flags_to_be_removed.push_back(site.site->base_flag().get_position());
			game().send_player_dismantle(*site.site);

			return true;
		}

		return false;
	}

	// All other SPACE_CONSUMERS without input and above target_count
	if (site.bo->inputs_.empty()  // does not consume anything
	    &&
	    site.bo->production_hint_ == -1  // not a renewing building (forester...)
	    &&
	    site.unoccupied_till_ + 10 * 60 * 1000 < game().get_gametime()  // > 10 minutes old
	    &&
	    site.site->can_start_working()  // building is occupied
	    &&
	    site.bo->space_consumer_ && !site.bo->plants_trees_) {

		// if we have more buildings then target
		if (site.bo->cnt_built_ > site.bo->cnt_target_) {
			if (site.bo->stocklevel_time < game().get_gametime() - 5 * 1000) {
				site.bo->stocklevel_ = get_stocklevel(*site.bo);
				site.bo->stocklevel_time = game().get_gametime();
			}

			if (site.site->get_statistics_percent() < 30 &&
			    site.bo->stocklevel_ > 100) {  // production stats == 0%
				site.bo->last_dismantle_time_ = game().get_gametime();
				flags_to_be_removed.push_back(site.site->base_flag().get_position());
				game().send_player_dismantle(*site.site);
				return true;
			}
		}

		// a building can be dismanteld if it performs too bad, if it is not the last one
		if (site.site->get_statistics_percent() <= 10 && site.bo->cnt_built_ > 1) {

			flags_to_be_removed.push_back(site.site->base_flag().get_position());
			game().send_player_dismantle(*site.site);
			return true;
		}

		return false;
	}

	// buildings with inputs_, checking if we can a dismantle some due to low performance
	if (!site.bo->inputs_.empty() && (site.bo->cnt_built_ - site.bo->unoccupied_) >= 3 &&
	    site.site->can_start_working() &&
	    site.site->get_statistics_percent() < 20 &&  // statistics for the building
	    site.bo->current_stats_ < 30 &&              // overall statistics
	    (game().get_gametime() - site.unoccupied_till_) > 10 * 60 * 1000) {

		site.bo->last_dismantle_time_ = game().get_gametime();
		flags_to_be_removed.push_back(site.site->base_flag().get_position());
		game().send_player_dismantle(*site.site);
		return true;
	}

	// remaining buildings without inputs and not supporting ones (fishers only left probably and
	// huters)

	if (site.bo->inputs_.empty() && site.bo->production_hint_ < 0 &&
	    site.site->can_start_working() && !site.bo->space_consumer_ &&
	    site.site->get_statistics_percent() < 10 &&
	    ((game().get_gametime() - site.built_time_) > 10 * 60 * 1000)) {

		site.bo->last_dismantle_time_ = game().get_gametime();
		flags_to_be_removed.push_back(site.site->base_flag().get_position());
		game().send_player_dismantle(*site.site);
		return true;
	}

	// supporting productionsites (rangers)
	// stop/start them based on stock avaiable
	if (site.bo->production_hint_ >= 0) {

		if (site.bo->stocklevel_time < game().get_gametime() - 5 * 1000) {
			site.bo->stocklevel_ = get_stocklevel_by_hint(site.bo->production_hint_);
			site.bo->stocklevel_time = game().get_gametime();
		}

		uint16_t score = site.bo->stocklevel_;

		if (score > 200 && site.bo->cnt_built_ > site.bo->cnt_target_) {

			site.bo->last_dismantle_time_ = game().get_gametime();
			flags_to_be_removed.push_back(site.site->base_flag().get_position());
			game().send_player_dismantle(*site.site);
			return true;
		}

		if (score > 120 && !site.site->is_stopped()) {

			game().send_player_start_stop_building(*site.site);
		}

		if (score < 80 && site.site->is_stopped()) {

			game().send_player_start_stop_building(*site.site);
		}
	}

	return changed;
}

/**
 * checks the first mine in list, takes care if it runs out of
 * resources and finally reenqueues it at the end of the list.
 *
 * \returns true, if something was changed.
 */
bool DefaultAI::check_mines_(int32_t const gametime) {
	if ((next_mine_check_due_ > gametime) || mines_.empty())
		return false;

	next_mine_check_due_ = gametime + 7000;  // 7 seconds is enough
	// Reorder and set new values; - due to returns within the function
	mines_.push_back(mines_.front());
	mines_.pop_front();

	// Get link to productionsite that should be checked
	ProductionSiteObserver& site = mines_.front();

	// first get rid of mines that are missing workers for some time (6 minutes),
	// released worker (if any) can be usefull elsewhere !
	if (site.built_time_ + 6 * 60 * 1000 < gametime && !site.site->can_start_working()) {
		flags_to_be_removed.push_back(site.site->base_flag().get_position());
		game().send_player_dismantle(*site.site);
		return true;
	}

	// doing nothing when failed count is too low
	if (site.no_resources_count < 4) {
		return false;
	}

	// dismantling when the failed count is too high
	if (site.no_resources_count > 12) {
		flags_to_be_removed.push_back(site.site->base_flag().get_position());
		game().send_player_dismantle(*site.site);
		site.bo->construction_decision_time_ = gametime;
		return true;
	}

	// updating statistics if needed
	if (site.bo->stocklevel_time < game().get_gametime() - 5 * 1000) {
		site.bo->stocklevel_ = get_stocklevel_by_hint(site.bo->production_hint_);
		site.bo->stocklevel_time = game().get_gametime();
	}

	// if we have enough of mined resources on stock - do not upgrade
	if (site.bo->stocklevel_ > 150) {
		return false;
	}

	// Check whether building is enhanceable. If yes consider an upgrade.
	const BuildingIndex enhancement = site.site->descr().enhancement();

	// if no enhancement is possible
	if (enhancement == INVALID_INDEX) {
		// will be destroyed when no_resource_count will overflow
		return false;
	}

	bool changed = false;
	if (player_->is_building_type_allowed(enhancement)) {
		// first exclude possibility there are enhancements in construction or unoccupied_
		const BuildingDescr& bld = *tribe_->get_building_descr(enhancement);
		BuildingObserver& en_bo = get_building_observer(bld.name().c_str());

		// if it is too soon for enhancement and making sure there are no unoccupied mines
		if (gametime - en_bo.construction_decision_time_ >= kBuildingMinInterval &&
		    en_bo.unoccupied_ + en_bo.cnt_under_construction_ == 0) {

			// now verify that there are enough workers
			if (site.site->has_workers(enhancement, game())) {  // enhancing
				game().send_player_enhance_building(*site.site, enhancement);
				en_bo.construction_decision_time_ = gametime;
				changed = true;
			}
		}
	}

	return changed;
}

// this count ware as hints
uint32_t DefaultAI::get_stocklevel_by_hint(size_t hintoutput) {
	uint32_t count = 0;
	WareIndex wt(hintoutput);
	for (EconomyObserver* observer : economies) {
		// Don't check if the economy has no warehouse.
		if (observer->economy.warehouses().empty()) {
			continue;
		}

		count += observer->economy.stock_ware(wt);
	}

	return count;
}

// calculating how much an output is needed
// 'max' is because the building can have more outputs
void DefaultAI::check_ware_necessity(BuildingObserver& bo,
                                     bool* output_is_needed,
                                     int16_t* max_preciousness,
                                     int16_t* max_needed_preciousness) {

	// reseting values
	*output_is_needed = false;
	*max_preciousness = 0;
	*max_needed_preciousness = 0;

	for (EconomyObserver* observer : economies) {
		// Don't check if the economy has no warehouse.
		if (observer->economy.warehouses().empty()) {
			continue;
		}

		for (uint32_t m = 0; m < bo.outputs_.size(); ++m) {
			WareIndex wt(static_cast<size_t>(bo.outputs_.at(m)));

			if (observer->economy.needs_ware(wt)) {
				*output_is_needed = true;

				if (wares.at(bo.outputs_.at(m)).preciousness_ > *max_needed_preciousness) {
					*max_needed_preciousness = wares.at(bo.outputs_.at(m)).preciousness_;
				}
			}

			if (wares.at(bo.outputs_.at(m)).preciousness_ > *max_preciousness) {
				*max_preciousness = wares.at(bo.outputs_.at(m)).preciousness_;
			}
		}
	}
}

// counts produced output on stock
// if multiple outputs, it returns lowest value
uint32_t DefaultAI::get_stocklevel(BuildingObserver& bo) {
	uint32_t count = std::numeric_limits<uint32_t>::max();

	if (!bo.outputs_.empty()) {
		for (EconomyObserver* observer : economies) {
			// Don't check if the economy has no warehouse.
			if (observer->economy.warehouses().empty()) {
				continue;
			}

			for (uint32_t m = 0; m < bo.outputs_.size(); ++m) {
				WareIndex wt(static_cast<size_t>(bo.outputs_.at(m)));
				if (count > observer->economy.stock_ware(wt)) {
					count = observer->economy.stock_ware(wt);
				}
			}
		}
	}

	return count;
}

/**
 * Updates the first military building in list and reenques it at the end of
 * the list afterwards. If a militarysite is in secure area but holds more than
 * one soldier, the number of stationed soldiers is decreased. If the building
 * is near a border, the number of stationed soldiers is maximized
 *
 * \returns true if something was changed
 */
bool DefaultAI::check_militarysites(int32_t gametime) {
	if (next_militarysite_check_due_ > gametime) {
		return false;
	}

	// just to be sure the value is reset
	next_militarysite_check_due_ = gametime + 5 * 1000;  // 10 seconds is really fine
	// even if there are no finished & attended military sites, probably there are ones just in
	// construction
	unstationed_milit_buildings_ = 0;

	for (std::list<MilitarySiteObserver>::iterator it = militarysites.begin();
	     it != militarysites.end();
	     ++it) {
		if (it->site->stationed_soldiers().size() == 0) {
			unstationed_milit_buildings_ += 1;
		}
	}

	// Only useable, if defaultAI owns at least one militarysite
	if (militarysites.empty()) {
		return false;
	}

	// Check next militarysite
	bool changed = false;
	Map& map = game().map();
	MilitarySite* ms = militarysites.front().site;
	uint32_t const vision = ms->descr().vision_range();
	FCoords f = map.get_fcoords(ms->get_position());
	// look if there are any enemies building
	FindNodeEnemiesBuilding find_enemy(player_, game());

	// first if there are enemies nearby, check for buildings not land
	if (map.find_fields(Area<FCoords>(f, vision + 4), nullptr, find_enemy) == 0) {
		// If no enemy in sight - decrease the number of stationed soldiers
		// as long as it is > 1 - BUT take care that there is a warehouse in the
		// same economy where the thrown out soldiers can go to.

		if (ms->economy().warehouses().size()) {
			uint32_t const j = ms->soldier_capacity();

			if (MilitarySite::kPrefersRookies != ms->get_soldier_preference()) {
				game().send_player_militarysite_set_soldier_preference(
				   *ms, MilitarySite::kPrefersRookies);
			} else if (j > 1) {
				game().send_player_change_soldier_capacity(*ms, -1);
			}
			// if the building is in inner land and other militarysites still
			// hold the miliary influence of the field, consider to destruct the
			// building to free some building space.
			else {
				// treat this field like a buildable and write military info to it.
				BuildableField bf(f);
				update_buildable_field(bf, vision, true);
				const int32_t size_penalty = ms->get_size() - 1;

				int16_t score = 0;
				score += (bf.military_capacity_ > 5);
				score += (bf.military_presence_ > 3);
				score += (bf.military_loneliness_ < 180);
				score += (bf.military_stationed_ > (2 + size_penalty));
				score -= (ms->soldier_capacity() * 2 > static_cast<uint32_t>(bf.military_capacity_));
				score += (bf.unowned_land_nearby_ < 10);

				if (score >= 4) {
					if (ms->get_playercaps() & Widelands::Building::PCap_Dismantle) {
						flags_to_be_removed.push_back(ms->base_flag().get_position());
						game().send_player_dismantle(*ms);
						military_last_dismantle_ = game().get_gametime();
					} else {
						game().send_player_bulldoze(*ms);
						military_last_dismantle_ = game().get_gametime();
					}
				}
			}
		}
	} else {

		// If an enemy is in sight and the number of stationed soldier is not
		// at maximum - set it to maximum.
		uint32_t const j = ms->max_soldier_capacity();
		uint32_t const k = ms->soldier_capacity();

		if (j > k) {
			game().send_player_change_soldier_capacity(*ms, j - k);
			changed = true;
		}

		// and also set preference to Heroes
		if (MilitarySite::kPrefersHeroes != ms->get_soldier_preference()) {
			game().send_player_militarysite_set_soldier_preference(*ms, MilitarySite::kPrefersHeroes);
			changed = true;
		}
	}

	// reorder:;
	militarysites.push_back(militarysites.front());
	militarysites.pop_front();
	next_militarysite_check_due_ = gametime + 5 * 1000;  // 10 seconds is really fine
	return changed;
}

/**
 * This function takes care about the unowned and opposing territory and
 * recalculates the priority for none military buildings depending on the
 * initialisation type of a defaultAI
 *
 * \arg bf   = BuildableField to be checked
 * \arg prio = priority until now.
 *
 * \returns the recalculated priority
 */
int32_t DefaultAI::recalc_with_border_range(const BuildableField& bf, int32_t prio) {
	// Prefer building space in the inner land.

	if (bf.unowned_land_nearby_ > 15) {
		prio -= (bf.unowned_land_nearby_ - 15);
	}

	// Especially places near the frontier to the enemies are unlikely
	//  NOTE take care about the type of computer player_. The more
	//  NOTE aggressive a computer player_ is, the more important is
	//  NOTE this check. So we add \var type as bonus.
	if (bf.enemy_nearby_ && prio > 0) {
		prio /= (3 + type_);
	}

	return prio;
}

/**
 * calculates how much a productionsite of type \arg bo is needed inside it's
 * economy. \arg prio is initial value for this calculation
 *
 * \returns the calculated priority
 */
int32_t DefaultAI::calculate_need_for_ps(BuildingObserver& bo, int32_t prio) {
	// some randomness to avoid that defaultAI is building always
	// the same (always == another game but same map with
	// defaultAI on same coords)
	prio += time(nullptr) % 3 - 1;

	// check if current economy can supply enough material for
	// production.
	for (uint32_t k = 0; k < bo.inputs_.size(); ++k) {
		prio += 2 * wares.at(bo.inputs_.at(k)).producers_;
		prio -= wares.at(bo.inputs_.at(k)).consumers_;
	}

	if (bo.inputs_.empty()) {
		prio += 4;
	}

	int32_t output_prio = 0;

	for (uint32_t k = 0; k < bo.outputs_.size(); ++k) {
		WareObserver& wo = wares.at(bo.outputs_.at(k));

		if (wo.consumers_ > 0) {
			output_prio += wo.preciousness_;
			output_prio += wo.consumers_ * 2;
			output_prio -= wo.producers_ * 2;

			if (bo.total_count() == 0) {
				output_prio += 10;  // add a big bonus
			}
		}
	}

	if (bo.outputs_.size() > 1) {
		output_prio =
		   static_cast<int32_t>(ceil(output_prio / sqrt(static_cast<double>(bo.outputs_.size()))));
	}

	prio += 2 * output_prio;

	// If building consumes some wares, multiply with current statistics of all
	// other buildings of this type to avoid constructing buildings where already
	// some are running on low resources.
	// Else at least add a part of the stats t the calculation.
	if (!bo.inputs_.empty()) {
		prio *= bo.current_stats_;
		prio /= 100;
	} else {
		prio = ((prio * bo.current_stats_) / 100) + (prio / 2);
	}

	return prio;
}

void DefaultAI::consider_productionsite_influence(BuildableField& field,
                                                  Coords coords,
                                                  const BuildingObserver& bo) {
	if (bo.space_consumer_ && !bo.plants_trees_ &&
	    game().map().calc_distance(coords, field.coords) < 8) {
		++field.space_consumers_nearby_;
	}

	for (size_t i = 0; i < bo.inputs_.size(); ++i) {
		++field.consumers_nearby_.at(bo.inputs_.at(i));
	}

	for (size_t i = 0; i < bo.outputs_.size(); ++i) {
		++field.producers_nearby_.at(bo.outputs_.at(i));
	}
}

/// \returns the economy observer containing \arg economy
EconomyObserver* DefaultAI::get_economy_observer(Economy& economy) {
	for (std::list<EconomyObserver*>::iterator i = economies.begin(); i != economies.end(); ++i)
		if (&(*i)->economy == &economy)
			return *i;

	economies.push_front(new EconomyObserver(economy));
	return economies.front();
}

// \returns the building observer
BuildingObserver& DefaultAI::get_building_observer(char const* const name) {
	if (tribe_ == nullptr) {
		late_initialization();
	}

	for (uint32_t i = 0; i < buildings_.size(); ++i) {
		if (!strcmp(buildings_.at(i).name, name)) {
			return buildings_.at(i);
		}
	}

	throw wexception("Help: I do not know what to do with a %s", name);
}

// this is called whenever we gain ownership of a PlayerImmovable
void DefaultAI::gain_immovable(PlayerImmovable& pi) {
	if (upcast(Building, building, &pi)) {
		gain_building(*building);
	} else if (upcast(Flag const, flag, &pi)) {
		new_flags.push_back(flag);
	} else if (upcast(Road const, road, &pi)) {
		roads.push_front(road);
	}
}

// this is called whenever we lose ownership of a PlayerImmovable
void DefaultAI::lose_immovable(const PlayerImmovable& pi) {
	if (upcast(Building const, building, &pi)) {
		lose_building(*building);
	} else if (upcast(Flag const, flag, &pi)) {
		for (EconomyObserver* eco_obs : economies) {
			for (std::list<Flag const*>::iterator flag_iter = eco_obs->flags.begin();
			     flag_iter != eco_obs->flags.end();
			     ++flag_iter) {
				if (*flag_iter == flag) {
					eco_obs->flags.erase(flag_iter);
					return;
				}
			}
		}
		for (std::list<Flag const*>::iterator flag_iter = new_flags.begin();
		     flag_iter != new_flags.end();
		     ++flag_iter) {
			if (*flag_iter == flag) {
				new_flags.erase(flag_iter);
				return;
			}
		}
	} else if (upcast(Road const, road, &pi)) {
		roads.remove(road);
	}
}

// this is called when a mine reports "out of resources"
void DefaultAI::out_of_resources_site(const ProductionSite& site) {

	// we must identify which mine matches the productionsite a note reffers to
	for (std::list<ProductionSiteObserver>::iterator i = mines_.begin(); i != mines_.end(); ++i)
		if (i->site == &site) {
			i->no_resources_count += 1;
			break;
		}
}

// this is called whenever we gain a new building
void DefaultAI::gain_building(Building& b) {
	BuildingObserver& bo = get_building_observer(b.descr().name().c_str());

	if (bo.type == BuildingObserver::CONSTRUCTIONSITE) {
		BuildingObserver& target_bo =
		   get_building_observer(ref_cast<ConstructionSite, Building>(b).building().name().c_str());
		++target_bo.cnt_under_construction_;
		++num_constructionsites_;
		if (target_bo.type == BuildingObserver::PRODUCTIONSITE) {
			++num_prod_constructionsites;
		}
		if (target_bo.type == BuildingObserver::MILITARYSITE) {
			++num_milit_constructionsites;
		}

		// Let defaultAI try to directly connect the constructionsite
		next_road_due_ = game().get_gametime();
	} else {
		++bo.cnt_built_;

		if (bo.type == BuildingObserver::PRODUCTIONSITE) {
			productionsites.push_back(ProductionSiteObserver());
			productionsites.back().site = &ref_cast<ProductionSite, Building>(b);
			productionsites.back().bo = &bo;
			productionsites.back().built_time_ = game().get_gametime();
			productionsites.back().unoccupied_till_ = game().get_gametime();
			productionsites.back().stats_zero_ = 0;
			productionsites.back().no_resources_count = 0;

			for (uint32_t i = 0; i < bo.outputs_.size(); ++i)
				++wares.at(bo.outputs_.at(i)).producers_;

			for (uint32_t i = 0; i < bo.inputs_.size(); ++i)
				++wares.at(bo.inputs_.at(i)).consumers_;
		} else if (bo.type == BuildingObserver::MINE) {
			mines_.push_back(ProductionSiteObserver());
			mines_.back().site = &ref_cast<ProductionSite, Building>(b);
			mines_.back().bo = &bo;
			mines_.back().built_time_ = game().get_gametime();

			for (uint32_t i = 0; i < bo.outputs_.size(); ++i)
				++wares.at(bo.outputs_.at(i)).producers_;

			for (uint32_t i = 0; i < bo.inputs_.size(); ++i)
				++wares.at(bo.inputs_.at(i)).consumers_;
		} else if (bo.type == BuildingObserver::MILITARYSITE) {
			militarysites.push_back(MilitarySiteObserver());
			militarysites.back().site = &ref_cast<MilitarySite, Building>(b);
			militarysites.back().bo = &bo;
			militarysites.back().checks = bo.desc->get_size();
			militarysites.back().enemies_nearby = true;

		} else if (bo.type == BuildingObserver::WAREHOUSE) {
			++numof_warehouses_;
		}
	}
}

// this is called whenever we lose a building
void DefaultAI::lose_building(const Building& b) {
	BuildingObserver& bo = get_building_observer(b.descr().name().c_str());

	if (bo.type == BuildingObserver::CONSTRUCTIONSITE) {
		BuildingObserver& target_bo = get_building_observer(
		   ref_cast<ConstructionSite const, Building const>(b).building().name().c_str());
		--target_bo.cnt_under_construction_;
		--num_constructionsites_;
		if (target_bo.type == BuildingObserver::PRODUCTIONSITE) {
			--num_prod_constructionsites;
		}
		if (target_bo.type == BuildingObserver::MILITARYSITE) {
			--num_milit_constructionsites;
		}

	} else {
		--bo.cnt_built_;

		if (bo.type == BuildingObserver::PRODUCTIONSITE) {

			for (std::list<ProductionSiteObserver>::iterator i = productionsites.begin();
			     i != productionsites.end();
			     ++i)
				if (i->site == &b) {
					productionsites.erase(i);
					break;
				}

			for (uint32_t i = 0; i < bo.outputs_.size(); ++i) {
				--wares.at(bo.outputs_.at(i)).producers_;
			}

			for (uint32_t i = 0; i < bo.inputs_.size(); ++i) {
				--wares.at(bo.inputs_.at(i)).consumers_;
			}
		} else if (bo.type == BuildingObserver::MINE) {
			for (std::list<ProductionSiteObserver>::iterator i = mines_.begin(); i != mines_.end();
			     ++i) {
				if (i->site == &b) {
					mines_.erase(i);
					break;
				}
			}

			for (uint32_t i = 0; i < bo.outputs_.size(); ++i) {
				--wares.at(bo.outputs_.at(i)).producers_;
			}

			for (uint32_t i = 0; i < bo.inputs_.size(); ++i) {
				--wares.at(bo.inputs_.at(i)).consumers_;
			}
		} else if (bo.type == BuildingObserver::MILITARYSITE) {

			for (std::list<MilitarySiteObserver>::iterator i = militarysites.begin();
			     i != militarysites.end();
			     ++i) {
				if (i->site == &b) {
					militarysites.erase(i);
					break;
				}
			}
		} else if (bo.type == BuildingObserver::WAREHOUSE) {
			assert(numof_warehouses_ > 0);
			--numof_warehouses_;
		}
	}

	m_buildable_changed = true;
	m_mineable_changed = true;
}

// Checks that supply line exists for given building.
// Recurcsively verify that all inputs_ have a producer.
// TODO(unknown): this function leads to periodic freezes of ~1 second on big games on my system.
// TODO(unknown): It needs profiling and optimization.
// NOTE: This is not needed anymore and it seems it is not missed neither
bool DefaultAI::check_supply(const BuildingObserver& bo) {
	size_t supplied = 0;
	for (const int16_t& temp_inputs : bo.inputs_) {
		for (const BuildingObserver& temp_building : buildings_) {
			if (temp_building.cnt_built_ &&
			    std::find(temp_building.outputs_.begin(), temp_building.outputs_.end(), temp_inputs) !=
			       temp_building.outputs_.end() &&
			    check_supply(temp_building)) {
				++supplied;
				break;
			}
		}
	}

	return supplied == bo.inputs_.size();
}

/**
 * The defaultAi "considers" via this function whether to attack an
 * enemy, if opposing military buildings are in sight. In case of an attack it
 * sends all available forces.
 *
 * \returns true, if attack was started.
 */
bool DefaultAI::consider_attack(int32_t const gametime) {
	// Only useable, if it owns at least one militarysite
	if (militarysites.empty()) {
		return false;
	}

	// First we iterate over all players and define which ones (if any)
	// are attackable (comparing overal strength)
	// counting players in game
	uint32_t plr_in_game = 0;
	std::vector<bool> player_attackable;
	PlayerNumber const nr_players = game().map().get_nrplayers();
	player_attackable.resize(nr_players);
	bool any_attackable = false;
	bool any_attacked = false;
	uint16_t const pn = player_number();

	// defining treshold ratio of own_strenght/enemy's_strength
	uint32_t treshold_ratio = 100;
	if (type_ == AGGRESSIVE) {
		treshold_ratio = 80;
	}
	if (type_ == DEFENSIVE) {
		treshold_ratio = 120;
	}

	iterate_players_existing_novar(p, nr_players, game())++ plr_in_game;

	// receiving games statistics and parsing it (reading latest entry)
	const Game::GeneralStatsVector& genstats = game().get_general_statistics();
	for (uint8_t j = 1; j <= plr_in_game; ++j) {
		if (pn == j) {
			player_attackable.at(j - 1) = false;
			continue;
		}

		try {
			// It seems that under some circumstances genstats can be empty.
			// So, to avoid crash, the AI tests its content first.
			if (genstats.at(j - 1).miltary_strength.empty()) {
				log("ComputerPlayer(%d): miltary_strength is empty\n", player_number());
				player_attackable.at(j - 1) = false;
			// Avoid division by zero
			} else if ( genstats.at(j - 1).miltary_strength.back() == 0) {
				player_attackable.at(j - 1) = true;
				any_attackable = true;
			// Check threshold
			} else if ((genstats.at(pn - 1).miltary_strength.back() * 100 /
							genstats.at(j - 1).miltary_strength.back()) > treshold_ratio) {
				player_attackable.at(j - 1) = true;
				any_attackable = true;
			} else {
				player_attackable.at(j - 1) = false;
			}
		} catch (const std::out_of_range&) {
			log("ComputerPlayer(%d): genstats entry missing - size :%d\n", player_number(), genstats.size());
			player_attackable.at(j - 1) = false;
		}
	}

	if (!any_attackable) {
		next_attack_consideration_due_ = 120 * 1000 + (gametime % 30 + 2) * 1000 + gametime;
		return false;
	}

	// the logic of attacking is to pick n military buildings - random ones
	// and test them for possible attack
	const uint16_t attempts = militarysites.size() / 6 + 1;
	Map& map = game().map();

	for (uint32_t i = 0; i < attempts && !any_attacked; ++i) {
		const uint16_t position = (game().get_gametime() + (3 * i)) % militarysites.size();

		// picking random military sites
		// using gametime as a random value, but it is constant so each next is on position +3
		// iterating over fields
		// (std::vector would be much better here)

		std::list<MilitarySiteObserver>::iterator mso = militarysites.begin();
		std::advance(mso, position);

		MilitarySite* ms = mso->site;
		Building* target = ms;  // dummy initialisation to silence the compiler
		uint32_t const vision = ms->descr().vision_range();
		FCoords f = map.get_fcoords(ms->get_position());
		int32_t chance = 0;
		uint32_t attackers = 0;
		uint32_t defenders = 0;
		uint32_t defend_ready_enemies = 0;  // enemy soldiers that can come to defend the attacked
		                                    // building (one soldier has to stay)

		// skipping if based on  "enemies nearby" there are probably no enemies nearby
		if (!mso->enemies_nearby && gametime % 8 > 0) {
			continue;  // go on with next attempt
		}

		// setting as default
		mso->enemies_nearby = false;

		// Search in a radius of the vision of the militarysite and collect
		// information about immovables in the area
		std::vector<ImmovableFound> immovables;
		map.find_immovables(Area<FCoords>(f, vision), &immovables, FindImmovableAttackable());

		for (uint32_t j = 0; j < immovables.size(); ++j) {
			if (upcast(MilitarySite, bld, immovables.at(j).object)) {
				if (!player_->is_hostile(bld->owner())) {
					continue;
				}

				mso->enemies_nearby = true;

				if (!player_attackable[bld->owner().player_number() - 1]) {
					continue;
				}

				if (bld->can_attack()) {

					// any_attackable_building=true;

					int32_t ta = player_->find_attack_soldiers(bld->base_flag());

					if (type_ == NORMAL) {
						ta = ta * 2 / 3;
					}

					if (ta < 1) {
						continue;
					}

					int32_t const tc = ta - bld->present_soldiers().size();

					if (bld->present_soldiers().size() > 1) {
						defend_ready_enemies += bld->present_soldiers().size() - 1;
					}

					if (tc > chance) {
						target = bld;
						chance = tc;
						attackers = ta;
						defenders = bld->present_soldiers().size();
					}
				}
			} else if (upcast(Warehouse, wh, immovables.at(j).object)) {
				if (!player_->is_hostile(wh->owner())) {
					continue;
				}

				if (wh->can_attack()) {
					int32_t ta = player_->find_attack_soldiers(wh->base_flag());

					if (ta < 1) {
						continue;
					}

					// extra priority push!
					int32_t tc = ta * 2;

					// we presume that there are no soldiers in warehouse
					// after long fights this tend to be true :)

					if (tc > chance) {
						target = wh;
						chance = tc;
						attackers = ta;
						defenders = 0;
					}
				}
			}

			// here we consider enemy soldiers in near buildings.
			int32_t penalty;
			if (defend_ready_enemies > 0) {
				penalty = (defenders * 100) / 5 * (defend_ready_enemies * 100) / 10 * 10 / 100;
			} else {
				penalty = 0;
			}

			// Return if chance to win is too low
			if ((chance - penalty / 100) < 2) {
				continue;
			}

			game().send_player_enemyflagaction(target->base_flag(), pn, attackers);

			any_attacked = true;
			break;
		}
	}

	//  Do not attack again too soon - returning soldiers must get healed first.
	if (any_attacked) {
		next_attack_consideration_due_ = (gametime % 40 + 10) * 1000 + gametime;
	} else {
		next_attack_consideration_due_ = (gametime % 80 + 10) * 1000 + gametime;
	}

	if (any_attacked) {
		return true;
	} else {
		return false;
	}
}

// This is used for profiling, so usually this is not used :)
void DefaultAI::print_land_stats() {
	// this will just print statistics of land size
	// intended for AI development only
	uint32_t plr_in_game = 0;
	uint32_t sum_l = 0;
	uint32_t count_l = 0;
	uint32_t sum_m = 0;
	uint32_t count_m = 0;
	PlayerNumber const nr_players = game().map().get_nrplayers();
	iterate_players_existing_novar(p, nr_players, game())++ plr_in_game;
	const Game::GeneralStatsVector& genstats = game().get_general_statistics();

	for (uint8_t j = 1; j <= plr_in_game; ++j) {
		log(" player: %1d, landsize: %5d, military strength: %3d\n",
		    j,
		    genstats[j - 1].land_size.back(),
		    genstats[j - 1].miltary_strength.back());

		sum_l += genstats[j - 1].land_size.back();
		count_l += 1;
		sum_m += genstats[j - 1].miltary_strength.back();
		count_m += 1;
	}

	assert(count_l > 0 && count_m > 0);
	log(" Average: Landsize: %5d, military strength: %3d\n", sum_l / count_l, sum_m / count_m);
}
