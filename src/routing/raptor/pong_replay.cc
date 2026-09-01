#include "nigiri/routing/raptor/pong_replay.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <utility>
#include <variant>

#include "utl/helpers/algorithm.h"
#include "utl/zip.h"

#include "nigiri/routing/get_earliest_transport.h"
#include "nigiri/stop.h"
#include "nigiri/timetable.h"

namespace nigiri::routing {

static bool has_same_structure(pong_replay_pattern const& a,
                               pong_replay_pattern const& b) {
  return a.transfers_ == b.transfers_ && a.legs_.size() == b.legs_.size() &&
         utl::all_of(utl::zip(a.legs_, b.legs_), [](auto&& pair) {
           auto const [x, y] = pair;
           return x.route_ == y.route_ && x.board_ == y.board_ &&
                  x.alight_ == y.alight_;
         });
}

template <direction SearchDir>
static std::optional<unixtime_t> replay(timetable const& tt,
                                        pong_replay_pattern const& pattern,
                                        unixtime_t replay_time) {
  constexpr auto const kFwd = SearchDir == direction::kForward;
  constexpr auto const kEvent = kFwd ? event_type::kArr : event_type::kDep;

  if constexpr (!kFwd) {
    replay_time -= pattern.non_transit_time_after_;
  }

  for (auto i = 0U; i != pattern.legs_.size(); ++i) {
    auto const leg_idx = kFwd ? i : pattern.legs_.size() - i - 1U;
    auto const& leg = pattern.legs_[leg_idx];

    if constexpr (kFwd) {
      replay_time += leg.non_transit_time_before_;
    }

    auto const search_stop = kFwd ? leg.board_ : leg.alight_;
    auto const reached_stop = kFwd ? leg.alight_ : leg.board_;
    auto const search_location =
        stop{tt.route_location_seq_[leg.route_][search_stop]}.location_idx();
    auto const [day, mam] = tt.day_idx_mam(replay_time);
    auto const t = get_earliest_transport<SearchDir>(
        tt, tt, 0U, leg.route_, search_stop, day, mam, search_location,
        [](auto, auto) { return false; });
    if (!t.is_valid()) {
      return std::nullopt;
    }

    replay_time = tt.event_time(t, reached_stop, kEvent);

    if constexpr (!kFwd) {
      replay_time -= leg.non_transit_time_before_;
    }
  }

  if constexpr (kFwd) {
    replay_time += pattern.non_transit_time_after_;
  }

  return replay_time;
}

static std::optional<pong_replay_pattern> get_pattern(timetable const& tt,
                                                      journey const& j) {
  auto pattern = pong_replay_pattern{.legs_ = {},
                                     .non_transit_time_after_ = duration_t{0},
                                     .transfers_ = j.transfers_};
  auto non_transit_time = duration_t{0};
  for (auto const& leg : j.legs_) {
    auto const* const use = std::get_if<journey::run_enter_exit>(&leg.uses_);
    if (use == nullptr) {
      assert(leg.arr_time_ >= leg.dep_time_);
      non_transit_time += duration_t{(leg.arr_time_ - leg.dep_time_).count()};
      continue;
    }
    if (!use->r_.t_.is_valid()) {
      return std::nullopt;
    }
    pattern.legs_.push_back(
        {.route_ = tt.transport_route_[use->r_.t_.t_idx_],
         .board_ = use->stop_range_.from_,
         .alight_ = static_cast<stop_idx_t>(use->stop_range_.to_ - 1U),
         .non_transit_time_before_ = non_transit_time});
    non_transit_time = duration_t{0};
  }
  if (pattern.legs_.empty()) {
    return std::nullopt;
  }
  pattern.non_transit_time_after_ = non_transit_time;
  return pattern;
}

template <direction SearchDir>
static bool dominates(pong_replay_bound const& a, pong_replay_bound const& b) {
  auto const time_dominates = SearchDir == direction::kForward
                                  ? a.time_ <= b.time_
                                  : a.time_ >= b.time_;
  return a.transfers_ <= b.transfers_ && time_dominates;
}

template <direction SearchDir>
static void add_bound(std::vector<pong_replay_bound>& bounds,
                      pong_replay_bound bound) {
  if (utl::any_of(bounds, [&](auto const& x) {
        return dominates<SearchDir>(x, bound);
      })) {
    return;
  }
  std::erase_if(bounds,
                [&](auto const& x) { return dominates<SearchDir>(bound, x); });
  bounds.push_back(bound);
}

template <direction SearchDir>
static std::vector<pong_replay_bound> replay_destination_bounds(
    timetable const& tt,
    std::vector<pong_replay_pattern> const& patterns,
    unixtime_t const start_time,
    unixtime_t const worst_time_at_dest) {
  constexpr auto const is_better = [](auto a, auto b) {
    return SearchDir == direction::kForward ? a < b : a > b;
  };
  auto bounds = std::vector<pong_replay_bound>{};
  for (auto const& pattern : patterns) {
    if (auto const bound = replay<SearchDir>(tt, pattern, start_time);
        bound && is_better(*bound, worst_time_at_dest)) {
      add_bound<SearchDir>(bounds,
                           {.time_ = *bound, .transfers_ = pattern.transfers_});
    }
  }
  return bounds;
}

void pong_replay_cache::add_journey(timetable const& tt, journey const& j) {
  auto pattern = get_pattern(tt, j);
  if (!pattern) {
    return;
  }
  auto const it = utl::find_if(patterns_, [&](pong_replay_pattern const& x) {
    return has_same_structure(x, *pattern);
  });
  if (it == end(patterns_)) {
    patterns_.push_back(std::move(*pattern));
    return;
  }
  for (auto const [stored, added] : utl::zip(it->legs_, pattern->legs_)) {
    stored.non_transit_time_before_ = std::min(stored.non_transit_time_before_,
                                               added.non_transit_time_before_);
  }
  it->non_transit_time_after_ =
      std::min(it->non_transit_time_after_, pattern->non_transit_time_after_);
}

std::vector<pong_replay_bound> pong_replay_cache::get_destination_bounds(
    timetable const& tt,
    direction const search_dir,
    unixtime_t const start_time,
    unixtime_t const worst_time_at_dest) const {
  return search_dir == direction::kForward
             ? replay_destination_bounds<direction::kForward>(
                   tt, patterns_, start_time, worst_time_at_dest)
             : replay_destination_bounds<direction::kBackward>(
                   tt, patterns_, start_time, worst_time_at_dest);
}

}  // namespace nigiri::routing
