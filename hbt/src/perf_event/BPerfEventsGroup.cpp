// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "hbt/src/perf_event/BPerfEventsGroup.h"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <sys/file.h>
#include <unistd.h>
#include <algorithm>
#include <optional>
#include "hbt/src/perf_event/BPerfEventsGroup.h"
#include "hbt/src/perf_event/BuiltinMetrics.h"
#include "hbt/src/perf_event/bperf_leader_cgroup.skel.h"

namespace facebook::hbt::perf_event {

constexpr auto kAttrMapVersion = 1;
constexpr auto kAttrMapSize = 16;
constexpr auto kMaxAttrPerMetric = 8;

static inline __u32 bpf_link_get_id(int fd) {
  struct bpf_link_info link_info = {
      .id = 0,
  };
  __u32 link_info_len = sizeof(link_info);

  bpf_obj_get_info_by_fd(fd, &link_info, &link_info_len);
  return link_info.id;
}

static inline __u32 bpf_link_get_prog_id(int fd) {
  struct bpf_link_info link_info = {
      .id = 0,
  };
  __u32 link_info_len = sizeof(link_info);

  bpf_obj_get_info_by_fd(fd, &link_info, &link_info_len);
  return link_info.prog_id;
}

static inline __u32 bpf_map_get_id(int fd) {
  struct bpf_map_info map_info = {
      .id = 0,
  };
  __u32 map_info_len = sizeof(map_info);

  bpf_obj_get_info_by_fd(fd, &map_info, &map_info_len);
  return map_info.id;
}

static inline __u32 bpf_prog_get_id(int fd) {
  struct bpf_prog_info prog_info = {
      .id = 0,
  };
  __u32 prog_info_len = sizeof(prog_info);

  bpf_obj_get_info_by_fd(fd, &prog_info, &prog_info_len);
  return prog_info.id;
}

/// name: Path of ebpf map
BPerfEventsGroup::BPerfEventsGroup(
    const std::string& name,
    const EventConfs& confs,
    int cgroup_update_level)
    : name_(name), confs_(confs), cgroup_update_level_(cgroup_update_level) {
  HBT_ARG_CHECK_LE(name.length(), BPERF_METRIC_NAME_SIZE)
      << "bpf map name is too long, max size is " << BPERF_METRIC_NAME_SIZE;
  for (const auto& conf : confs_) {
    struct perf_event_attr attr = {
        .size = sizeof(attr),
        .type = conf.configs.type,
        .config = conf.configs.config,
        .config1 = conf.configs.config1,
        .config2 = conf.configs.config2,
        .read_format =
            PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING};
    attrs_.push_back(attr);
  }
  cpu_cnt_ = ::libbpf_num_possible_cpus();
}

BPerfEventsGroup::BPerfEventsGroup(
    const std::string& name,
    const MetricDesc& metric,
    const PmuDeviceManager& pmu_manager,
    int cgroup_update_level)
    : BPerfEventsGroup(
          name,
          metric.makeNoCpuTopologyConfs(pmu_manager),
          cgroup_update_level) {}
inline auto mapFdWrapperPtrIntoInode(
    const std::shared_ptr<FdWrapper>& fd_wrapper) {
  if (fd_wrapper == nullptr) {
    return 0ull;
  }
  return fd_wrapper->getInode();
}

BPerfEventsGroup::~BPerfEventsGroup() {
  this->close();
}

std::string BPerfEventsGroup::attrMapPath() {
  std::stringstream ss;

  ss << "/sys/fs/bpf/bperf_attr_map_v";
  ss << std::setw(3) << std::setfill('0') << kAttrMapVersion;
  return ss.str();
}

// TODO: deprecate attr map?
bperf_attr_map_key::bperf_attr_map_key(std::string n, int s) : size(s) {
  ::memset(name, 0, BPERF_METRIC_NAME_SIZE);
  ::memcpy(
      name,
      n.c_str(),
      std::min(static_cast<int>(n.size()), BPERF_METRIC_NAME_SIZE - 1));
}

size_t BPerfEventsGroup::getNumEvents() const {
  return attrs_.size();
}

bool BPerfEventsGroup::addCgroup(
    std::shared_ptr<hbt::FdWrapper> fd,
    int cgroup_update_level) {
  if (cgroup_update_level != cgroup_update_level_) {
    HBT_LOG_ERROR() << "BPerfEventsGroup will only track cgroups at level "
                    << cgroup_update_level_
                    << ", but addCgroup() ask to add a cgroup at level "
                    << cgroup_update_level;
    return false;
  }
  auto id = fd->getInode();
  if (cgroup_fds_.count(id)) {
    HBT_LOG_WARNING() << "Cgroup " << id << " is already being monitored";
    return false;
  }
  std::vector<bpf_perf_event_value> val(
      (size_t)cpu_cnt_ * BPERF_MAX_GROUP_SIZE, (struct bpf_perf_event_value){});
  auto err = ::bpf_map_update_elem(cgroup_output_fd_, &id, val.data(), BPF_ANY);
  if (err) {
    HBT_LOG_ERROR() << "Failed to initlize map elem: " << err;
    return false;
  }

  cgroup_fds_.insert({id, std::move(fd)});
  return true;
}

bool BPerfEventsGroup::removeCgroup(__u64 id) {
  if (!cgroup_fds_.count(id)) {
    HBT_LOG_WARNING() << "Cgroup " << id << " is not being tracked";
    return false;
  }
  auto err = ::bpf_map_delete_elem(cgroup_output_fd_, &id);
  if (err) {
    HBT_LOG_ERROR() << "Failed to delete cgroup: " << err;
    return false;
  }

  cgroup_fds_.erase(id);
  return true;
}

bool BPerfEventsGroup::disable() {
  syncGlobal_();
  ::close(leader_link_fd_);
  leader_link_fd_ = -1;
  for (auto& fd : pe_fds_) {
    ::ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
  }
  enabled_ = false;
  return true;
}

bool BPerfEventsGroup::open() {
  if (opened_) {
    HBT_LOG_WARNING() << "BPerfEventsGroup is already opened";
    return true;
  }
  HBT_LOG_INFO() << "Loading leader program.";
  struct bperf_attr_map_elem entry = {};
  int err = reloadSkel_(&entry);
  if (err < 0) {
    HBT_LOG_ERROR() << "Failed to load leader program.";
    return false;
  }
  leader_prog_fd_ =
      ::bpf_prog_get_fd_by_id(bpf_link_get_prog_id(leader_link_fd_));
  HBT_DCHECK(leader_link_fd_ >= 0);
  HBT_DCHECK(global_output_fd_ >= 0);
  HBT_DCHECK(cgroup_output_fd_ >= 0);
  HBT_DCHECK(leader_prog_fd_ >= 0);
  opened_ = true;
  return true;
}

void BPerfEventsGroup::close() {
  ::close(leader_link_fd_);
  leader_link_fd_ = -1;
  ::close(leader_prog_fd_);
  leader_prog_fd_ = -1;
  ::close(trigger_prog_fd_);
  trigger_prog_fd_ = -1;
  ::close(cgroup_output_fd_);
  cgroup_output_fd_ = -1;
  ::close(global_output_fd_);
  global_output_fd_ = -1;
  for (auto& fd : pe_fds_) {
    ::close(fd);
    fd = -1;
  }
  opened_ = false;
}
bool BPerfEventsGroup::isOpen() const {
  return opened_;
}

[[nodiscard]] bool BPerfEventsGroup::enable() {
  if (enabled_) {
    HBT_LOG_WARNING() << "BPerfEventsGroup is already enabled.";
    return true;
  }
  for (auto& fd : pe_fds_) {
    ::ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
  }

  if (leader_link_fd_ < 0) {
    leader_link_fd_ =
        bpf_link_create(leader_prog_fd_, 0, BPF_TRACE_RAW_TP, nullptr);
  }

  HBT_DCHECK(leader_link_fd_ >= 0);
  // set proper offset_
  ::memset(offsets_, 0, sizeof(offsets_));
  readGlobal(offsets_, true);
  enabled_ = true;
  return true;
}

// trigger the BPF program on a given CPU, so that value the counter is
// saved to the output maps
int BPerfEventsGroup::syncCpu_(__u32 cpu, int leader_fd) {
  DECLARE_LIBBPF_OPTS(
      bpf_test_run_opts,
      opts,
      .ctx_in = NULL,
      .ctx_size_in = 0,
      .flags = BPF_F_TEST_RUN_ON_CPU,
      .cpu = cpu,
      .retval = 0, );
  return ::bpf_prog_test_run_opts(leader_fd, &opts);
}

void BPerfEventsGroup::syncGlobal_() {
  int err;

  for (int cpu = 0; cpu < cpu_cnt_; cpu++) {
    err = syncCpu_(cpu, trigger_prog_fd_);

    if (err) {
      HBT_LOG_WARNING() << "Failed to sync event on cpu " << cpu;
    }
  }
}

void bperf_attr_map_elem::loadFromSkelLink(
    struct bperf_leader_cgroup* skel,
    struct bpf_link* link) {
  auto fd = ::bpf_map__fd(skel->maps.events);

  perf_event_array_id = bpf_map_get_id(fd);
  fd = ::bpf_link__fd(link);
  leader_prog_link_id = bpf_link_get_id(fd);
  bpf_map__fd(skel->maps.diff_readings);
  diff_reading_map_id = bpf_map_get_id(fd);
  fd = ::bpf_map__fd(skel->maps.global_output);
  global_output_map_id = bpf_map_get_id(fd);
  fd = ::bpf_map__fd(skel->maps.cgroup_output);
  cgroup_output_map_id = bpf_map_get_id(fd);
  fd = ::bpf_program__fd(skel->progs.bperf_read_trigger);
  trigger_prog_id = bpf_prog_get_id(fd);
}

[[nodiscard]] int BPerfEventsGroup::reloadSkel_(
    struct bperf_attr_map_elem* entry) {
  struct bperf_leader_cgroup* skel = bperf_leader_cgroup__open();
  int event_cnt = confs_.size();
  ::bpf_link* link;
  int err;

  if (skel == nullptr) {
    HBT_LOG_ERROR() << "Failed to open skeleton.";
    return -1;
  }

  ::bpf_map__set_max_entries(skel->maps.events, cpu_cnt_ * event_cnt);
  if (err = bperf_leader_cgroup__load(skel); err) {
    HBT_LOG_ERROR() << "Failed to load skeleton.";
    return err;
  }

  skel->bss->cpu_cnt = cpu_cnt_;
  skel->bss->event_cnt = event_cnt;
  skel->bss->cgroup_update_level = cgroup_update_level_;
  link = ::bpf_program__attach(skel->progs.bperf_on_sched_switch);
  if (!link) {
    HBT_LOG_ERROR() << "Failed to attach leader program";
    err = -1;
    goto out;
  }

  // fill the attr build map
  entry->loadFromSkelLink(skel, link);

  // open another fd to the link and the output map, so we can destroy the skel
  leader_link_fd_ = ::bpf_link_get_fd_by_id(entry->leader_prog_link_id);
  trigger_prog_fd_ = ::bpf_prog_get_fd_by_id(entry->trigger_prog_id);
  global_output_fd_ = ::bpf_map_get_fd_by_id(entry->global_output_map_id);
  cgroup_output_fd_ = ::bpf_map_get_fd_by_id(entry->cgroup_output_map_id);

  err = loadPerfEvent_(skel);
  if (err != 0) {
    HBT_LOG_ERROR() << "Failed to load perf events";
  }

out:
  bperf_leader_cgroup__destroy(skel);
  ::bpf_link__destroy(link);
  return err;
}

[[nodiscard]] int BPerfEventsGroup::loadPerfEvent_(
    struct bperf_leader_cgroup* skel) {
  int err = 0, e;

  for (e = 0; e < attrs_.size(); e++) {
    for (int cpu = 0; cpu < cpu_cnt_; cpu++) {
      int group_fd = e == 0 ? -1 : pe_fds_[cpu];
      int key = e * cpu_cnt_ + cpu;
      auto fd = static_cast<int>(::syscall(
          __NR_perf_event_open, &attrs_[e], -1 /* pid */, cpu, group_fd, 0));
      if (fd < 0) {
        err = -1;
        continue;
      }
      if (::bpf_map_update_elem(
              ::bpf_map__fd(skel->maps.events), &key, &fd, BPF_ANY) != 0) {
        err = -1;
      }
      pe_fds_.push_back(fd);
    }
  }
  return err;
}

int BPerfEventsGroup::read(
    struct bpf_perf_event_value* output,
    int fd,
    __u64 id,
    bool skip_offset) {
  auto event_cnt = confs_.size();
  std::vector<struct bpf_perf_event_value> values(
      (size_t)cpu_cnt_ * BPERF_MAX_GROUP_SIZE);

  if (!enabled_) {
    return -1;
  }

  syncGlobal_();
  if (int ret = ::bpf_map_lookup_elem(fd, &id, values.data()); ret) {
    HBT_LOG_ERROR() << "cannot look up key " << id
                    << " from output map. Return value: " << ret;
    return -1;
  }

  ::memset(output, 0, sizeof(*output) * BPERF_MAX_GROUP_SIZE);

  for (size_t e = 0; e < event_cnt; e++) {
    for (int c = 0; c < cpu_cnt_; c++) {
      int idx = c * BPERF_MAX_GROUP_SIZE + e;
      output[e].counter += values[idx].counter;
      output[e].enabled += values[idx].enabled;
      output[e].running += values[idx].running;
    }
    if (!skip_offset) {
      output[e].counter -= offsets_[e].counter;
      output[e].enabled -= offsets_[e].enabled;
      output[e].running -= offsets_[e].running;
    }
  }
  return event_cnt;
}

int BPerfEventsGroup::readGlobal(
    struct bpf_perf_event_value* output,
    bool skip_offset) {
  return read(output, global_output_fd_, 0, skip_offset);
}

bool BPerfEventsGroup::readGlobal(ReadValues& rv, bool skip_offset) {
  const auto num_events = rv.getNumEvents();
  HBT_ARG_CHECK_EQ(confs_.size(), rv.getNumEvents());

  struct bpf_perf_event_value values[BPERF_MAX_GROUP_SIZE];
  if (readGlobal(values, skip_offset) == num_events) {
    toReadValues(rv, values);
    return true;
  } else {
    return false;
  }
}

int BPerfEventsGroup::readCgroup(
    struct bpf_perf_event_value* output,
    __u64 id) {
  return read(output, cgroup_output_fd_, id, true /* skip_offset */);
}

bool BPerfEventsGroup::readCgroup(ReadValues& rv, __u64 id) {
  const auto num_events = rv.getNumEvents();
  HBT_ARG_CHECK_EQ(confs_.size(), rv.getNumEvents());

  struct bpf_perf_event_value values[BPERF_MAX_GROUP_SIZE];
  if (readCgroup(values, id) == num_events) {
    toReadValues(rv, values);
    return true;
  } else {
    return false;
  }
}

// TODO replace flock with mkdirmutex:
//   https://github.com/cdown/mkdirmutex
[[nodiscard]] int BPerfEventsGroup::lockAttrMap_() {
  auto path = BPerfEventsGroup::attrMapPath();
  int map_fd;
  int err = -1;

  if (::access(path.c_str(), F_OK)) {
    HBT_LOG_INFO() << "Creating " << path;

    map_fd = ::bpf_map_create(
        BPF_MAP_TYPE_HASH,
        nullptr,
        sizeof(struct bperf_attr_map_key),
        sizeof(struct bperf_attr_map_elem),
        kAttrMapSize,
        nullptr);
    if (map_fd < 0) {
      return -1;
    }

    if (err = ::bpf_obj_pin(map_fd, path.c_str()); err) {
      HBT_LOG_WARNING() << "Someone pinned the map at " << path << ". "
                        << "Error: " << toErrorCode(-err).message();
      ::close(map_fd);
    }
  }
  if (err) {
    map_fd = ::bpf_obj_get(path.c_str());
    if (map_fd < 0) {
      return -1;
    }
  }
  ::flock(map_fd, LOCK_EX);

  return map_fd;
}

void BPerfEventsGroup::toReadValues(
    ReadValues& rv,
    struct bpf_perf_event_value* values) {
  // BPerf does not really create groups in the sense perf_event does.
  // There is no simultaneous enable and disable of events, therefore,
  // the enabled (and running) time of events in the same group may be
  // slightly different (usually by a few nanoseconds).
  // We abstract this detail by averaging the times for all events
  // in the group, providing a close enough approximation.
  const auto num_events = rv.getNumEvents();
  uint64_t sum_time_enabled = 0;
  uint64_t sum_time_running = 0;

  for (auto e = 0; e < num_events; e++) {
    // 64 bits unsigned value will not overflow in several years.
    sum_time_enabled += values[e].enabled;
    sum_time_running += values[e].running;

    rv.t->count[e] = values[e].counter;
  }
  // Integer round-up division to get the average time enabled/running.
  rv.t->time_enabled = (sum_time_enabled + num_events - 1) / num_events;
  rv.t->time_running = (sum_time_running + num_events - 1) / num_events;
}

} // namespace facebook::hbt::perf_event
