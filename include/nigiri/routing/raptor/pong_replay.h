#pragma once

#include <cstdint>
#include <vector>

#include "nigiri/routing/journey.h"
#include "nigiri/types.h"

namespace nigiri {
struct timetable;
}

namespace nigiri::routing {

struct pong_replay_leg {
  route_idx_t route_;
  stop_idx_t board_;
  stop_idx_t alight_;
  duration_t non_transit_time_before_;
};

struct pong_replay_pattern {
  std::vector<pong_replay_leg> legs_;
  duration_t non_transit_time_after_;
  std::uint8_t transfers_;
};

struct pong_replay_bound {
  unixtime_t time_;
  std::uint8_t transfers_;
};

struct pong_replay_cache {
  void add_journey(timetable const&, journey const&);

  std::vector<pong_replay_bound> get_destination_bounds(timetable const&,
                                                        direction,
                                                        unixtime_t,
                                                        unixtime_t) const;

private:
  std::vector<pong_replay_pattern> patterns_;
};

}  // namespace nigiri::routing
