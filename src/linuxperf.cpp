// SPDX-FileCopyrightText: 2025 CERN
// SPDX-License-Identifier: GPL-2.0-only

#include "linuxperf_profiling.hpp"
#include <adaptyst/output.hpp>
#include <string>
#include <vector>
#include <fstream>
#include <boost/algorithm/string.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/predef.h>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <regex>
#include <variant>

#define ADAPTYST_MODULE_ENTRYPOINT
#include <adaptyst/hw.h>

volatile const char *name = "linuxperf";
volatile const char *version = "0.1.0-dev.2026.02a";
volatile int version_nums[] = {0, 1, 0, 3, -1};

volatile const unsigned int max_count_per_entity = 1;

volatile const char *options[] = {
  "buffer_size",
  "warmup",
  "freq",
  "buffer",
  "off_cpu_freq",
  "off_cpu_buffer",
  "events",
  "filter",
  "filter_mark",
  "capture_mode",
  "perf_path",
  "perf_script_path",
#if defined(ADAPTYST_ROOFLINE) && defined(BOOST_ARCH_X86) && defined(BOOST_COMP_GNUC)
  "roofline",
  "roofline_benchmark_path",
  "carm_tool_path",
#endif
  NULL};

volatile const char *tags[] = {
  "compute",
  NULL
};

volatile const char *log_types[] {
  NULL
};

volatile const char *buffer_size_help =
  "Internal communication buffer size in bytes (default: 1024)";
volatile const option_type buffer_size_type = UNSIGNED_INT;
volatile const unsigned int buffer_size_default = 1024;

volatile const char *warmup_help =
  "Warmup time in seconds between "
  "all profilers signalling their readiness and starting "
  "the profiled program. Increase this "
  "value if you see missing information after profiling. "
  "(default: 1)";
volatile const option_type warmup_type = UNSIGNED_INT;
volatile const unsigned int warmup_default = 1;

volatile const char *freq_help =
  "Sampling frequency per second for "
  "on-CPU time profiling (default: 10)";
volatile const option_type freq_type = UNSIGNED_INT;
volatile const unsigned int freq_default = 10;

volatile const char *buffer_help =
  "Buffer up to this number of "
  "events before sending data for processing "
  "(1 effectively disables buffering) (default: 1)";
volatile const option_type buffer_type = UNSIGNED_INT;
volatile const unsigned int buffer_default = 1;

volatile const char *off_cpu_freq_help =
  "Sampling frequency "
  "per second for off-CPU time profiling "
  "(0 disables off-CPU profiling, -1 makes linuxperf "
  "capture *all* off-CPU events) (default: 1000)";
volatile const option_type off_cpu_freq_type = INT;
volatile const int off_cpu_freq_default = 1000;

volatile const char *off_cpu_buffer_help =
  "Buffer up to "
  "this number of off-CPU events before sending data "
  "for processing (0 leaves the default "
  "adaptive buffering, 1 effectively disables buffering) "
  "(default: 0)";
volatile const option_type off_cpu_buffer_type = UNSIGNED_INT;
volatile const unsigned int off_cpu_buffer_default = 0;

volatile const char *events_help =
  "Extra perf events to be used "
  "for sampling with a given period (i.e. do a sample on "
  "every PERIOD occurrences of an event and display the "
  "results under the title TITLE with a unit UNIT in a "
  "website). This option accepts a list of strings of form "
  "\"EVENT,PERIOD,TITLE,UNIT\". Run \"perf list\" for the list of "
  "possible values for EVENT.";
volatile const option_type events_array_type = STRING;
volatile const char *events_array_default[] = {};
volatile const unsigned int events_array_default_size = 0;

volatile const char *filter_help =
  "Set stack trace filtering "
  "options. deny:<FILE> cuts all stack elements "
  "matching a set of conditions specified in a given "
  "text file. allow:<FILE> accepts "
  "only stack elements matching a set of conditions "
  "specified in a given text file. "
  "python:<FILE> sends all stack trace elements to "
  "a given Python script for filtering. Unless filter_mark is "
  "used, all filtered out elements are deleted "
  "completely. See the Adaptyst documentation to check "
  "in detail how to use filtering.";
volatile const option_type filter_type = STRING;
volatile const char *filter_default = "";

volatile const char *filter_mark_help =
  "When filter is used, mark "
  "filtered out stack trace elements as \"(cut)\" and "
  "squash any consecutive \"(cut)\"'s into one rather "
  "than deleting them completely";
volatile const option_type filter_mark_type = BOOL;
volatile const bool filter_mark_default = false;

volatile const char *capture_mode_help =
  "Capture only kernel (\"kernel\"), only "
  "user (i.e. non-kernel, \"user\"), or both stack trace types "
  "(\"both\") (default: \"user\")";
volatile const option_type capture_mode_type = STRING;
volatile const char *capture_mode_default = "user";

volatile const char *perf_path_help =
  "Path to the patched \"perf\" installation. Change it only "
  "if you know what you’re doing. Relative paths have the "
  "loaded library path as the parent.";
volatile const option_type perf_path_type = STRING;
volatile const char *perf_path_default = "perf";

volatile const char *perf_script_path_help =
  "Path to the linuxperf scripts interacting with \"perf\". Change "
  "it only if you know what you’re doing. Relative paths have "
  "the loaded library path as the parent.";
volatile const option_type perf_script_path_type = STRING;
volatile const char *perf_script_path_default = "";

#if defined(ADAPTYST_ROOFLINE) && defined(BOOST_ARCH_X86) && defined(BOOST_COMP_GNUC)
volatile const char *roofline_help =
  "Run also "
  "cache-aware roofline profiling with the specified sampling "
  "frequency per second";
volatile const option_type roofline_type = UNSIGNED_INT;
volatile const unsigned int roofline_default = 0;

volatile const char *roofline_benchmark_path_help =
  "Path to the cache-aware roofline benchmarking results "
  "produced by the CARM Tool. Either this option or carm_tool_path "
  "must be set if roofline > 0.";
volatile const option_type roofline_benchmark_path_type = STRING;
volatile const char *roofline_benchmark_path_default = "";

volatile const char *carm_tool_path_help =
  "Path to the CARM Tool cloned repository. Either this option or "
  "roofline_benchmark_path must be set if roofline > 0.";
volatile const option_type carm_tool_path_type = STRING;
volatile const char *carm_tool_path_default = "";
#endif

using namespace adaptyst;
using namespace std::chrono_literals;
namespace ch = std::chrono;

typedef struct {
  std::unordered_map<std::string, std::unordered_set<std::string> > dso_offsets;
  bool perf_maps_expected;
  bool error;
  ConnectionException exception;
} ConnectionResult;

class CPULinuxModule {
private:
  unsigned int buf_size;
  unsigned int warmup;
  unsigned int freq;
  unsigned int buffer;
  int off_cpu_freq;
  unsigned int off_cpu_buffer;
  std::vector<PerfEvent> events;
  Perf::Filter filter;
  Perf::CaptureMode capture_mode;
  CPUConfig cpu_config;
  fs::path perf_bin_path;
  fs::path perf_python_path;
  fs::path perf_script_path;
  unsigned long long profile_start;
  bool profile_start_set = false;
  amod_t module_id;
#if defined(ADAPTYST_ROOFLINE) && defined(BOOST_ARCH_X86) && defined(BOOST_COMP_GNUC)
  unsigned int roofline_freq;
  fs::path roofline_benchmark_path;
#endif

  void save_sample(nlohmann::json *data,
                   std::vector<std::pair<std::string, std::string> > &callchain_parts,
                   unsigned long long period,
                   bool time_ordered, bool offcpu) {
    bool last_block;

    if (time_ordered) {
      nlohmann::json *cur_elem = data;

      const std::string key = offcpu ? "cold_value" : "hot_value";
      (*cur_elem)[key] = (unsigned long long)(*cur_elem)[key] + period;
      (*cur_elem)["value"] = (unsigned long long)(*cur_elem)["value"] + period;

      int index = 0;

      if (!callchain_parts.empty()) {
        do {
          last_block = index == callchain_parts.size() - 1;
          bool elem_assigned = false;

          if ((*cur_elem)["children"].size() > 0) {
            nlohmann::json *child = &(*cur_elem)["children"][(*cur_elem)["children"].size() - 1];
            std::string candidate_name = (*child)["name"];

            if (candidate_name == callchain_parts[index].first) {
              if ((last_block && (*child)["children"].size() == 0) ||
                  (!last_block && (*child)["children"].size() > 0)) {
                cur_elem = child;
                elem_assigned = true;
              }
            }
          }

          if (!elem_assigned) {
            (*cur_elem)["children"].push_back(nlohmann::json::object());
            nlohmann::json &obj = (*cur_elem)["children"][(*cur_elem)["children"].size() - 1];
            obj["name"] = callchain_parts[index].first;
            obj["value"] = 0;
            obj["hot_value"] = 0;
            obj["cold_value"] = 0;
            obj["offsets"] = nlohmann::json::object();
            obj["children"] = nlohmann::json::array();
            cur_elem = &obj;
          }

          (*cur_elem)[key] = (unsigned long long)(*cur_elem)[key] + period;
          (*cur_elem)["value"] = (unsigned long long)(*cur_elem)["value"] + period;

          std::string &offset = callchain_parts[index].second;
          std::string offset_key = offcpu ? "cold_value" : "hot_value";

          if (!(*cur_elem)["offsets"].contains(offset)) {
            (*cur_elem)["offsets"][offset] = nlohmann::json::object();
            (*cur_elem)["offsets"][offset]["cold_value"] = 0;
            (*cur_elem)["offsets"][offset]["hot_value"] = 0;
          }

          (*cur_elem)["offsets"][offset][offset_key] =
            (unsigned long long)(*cur_elem)["offsets"][offset][offset_key] + period;

          index++;
        } while (!last_block);
      }
    } else {
      nlohmann::json *cur_elem = data;

      std::string key = offcpu ? "cold_value" : "hot_value";
      (*cur_elem)[key] = (unsigned long long)(*cur_elem)[key] + period;
      (*cur_elem)["value"] = (unsigned long long)(*cur_elem)["value"] + period;

      int index = 0;

      if (!callchain_parts.empty()) {
        do {
          last_block = index == callchain_parts.size() - 1;

          if (!(*cur_elem)["children"].contains(callchain_parts[index].first)) {
            nlohmann::json &obj = (*cur_elem)["children"][callchain_parts[index].first] = nlohmann::json::object();
            obj["name"] = callchain_parts[index].first;
            obj["value"] = 0;
            obj["hot_value"] = 0;
            obj["cold_value"] = 0;
            obj["offsets"] = nlohmann::json::object();
            obj["children"] = nlohmann::json::object();
          }

          cur_elem = &(*cur_elem)["children"][callchain_parts[index].first];
          (*cur_elem)[key] = (unsigned long long)(*cur_elem)[key] + period;
          (*cur_elem)["value"] = (unsigned long long)(*cur_elem)["value"] + period;

          std::string &offset = callchain_parts[index].second;
          std::string offset_key = offcpu ? "cold_value" : "hot_value";

          if (!(*cur_elem)["offsets"].contains(offset)) {
            (*cur_elem)["offsets"][offset] = nlohmann::json::object();
            (*cur_elem)["offsets"][offset]["cold_value"] = 0;
            (*cur_elem)["offsets"][offset]["hot_value"] = 0;
          }

          (*cur_elem)["offsets"][offset][offset_key] =
            (unsigned long long)(*cur_elem)["offsets"][offset][offset_key] + period;
          index++;
        } while (!last_block);
      }
    }
  }

  ConnectionResult process_connection(Path &dir,
                                      std::unique_ptr<Profiler> &profiler,
                                      std::unique_ptr<Connection> &connection) {
    ConnectionResult result;
    result.perf_maps_expected = false;
    result.error = false;

    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string> > > tid_dict;
    std::unordered_map<std::string, std::string> combo_dict;
    std::unordered_map<std::string, unsigned long long> exit_time_dict;
    std::unordered_map<std::string, std::vector<std::pair<std::string, unsigned long long> > > name_time_dict;
    std::unordered_map<std::string, std::string> tree;
    std::vector<std::pair<unsigned long long, std::string> > added_list;
    std::string extra_event_name = "";
    bool first_event_received = false;

    std::string line;
    bool thread_tree_connection = false;

    std::unordered_map<std::string, nlohmann::json> timed_data_map;
    std::unordered_map<std::string, nlohmann::json> untimed_data_map;

    try {
      while ((line = connection->read()) != "<STOP>") {
        if (line.empty()) {
          continue;
        }

        try {
          nlohmann::json parsed = nlohmann::json::parse(line);

          if (!parsed.is_object()) {
            adaptyst_print(this->module_id, ("Message received from profiler \"" +
                                             profiler->get_name() + "\" "
                                             "is not a JSON object, ignoring.").c_str(), true, false, "General");
            continue;
          }

          if (parsed.size() != 2 || !parsed.contains("type") ||
              !parsed.contains("data")) {
            adaptyst_print(this->module_id, ("Message received from profiler \"" +
                                             profiler->get_name() + "\" "
                                             "is not a JSON object with exactly 2 elements (\"type\" and "
                                             "\"data\"), ignoring.").c_str(), true, false, "General");
          }

          if (parsed["type"] == "missing_symbol_maps") {
            if (!parsed["data"].is_array()) {
              adaptyst_print(this->module_id, ("Message received from profiler \"" +
                                               profiler->get_name() + "\" "
                                               "is a JSON object of type \"missing_symbol_maps\", but its \"data\" "
                                               "element is not a JSON array, ignoring.").c_str(), true, false, "General");
              continue;
            }

            int index = -1;
            for (auto &elem : parsed["data"]) {
              index++;

              if (!elem.is_string()) {
                adaptyst_print(this->module_id, ("Element " + std::to_string(index) +
                                                 " in the array in the message "
                                                 "of type \"missing_symbol_maps\" received from profiler \"" +
                                                 profiler->get_name() +
                                                 "\" is not a string, ignoring this element.").c_str(), true, false, "General");
                continue;
              }

              fs::path perf_map_path(elem.get<std::string>());

              adaptyst_print(this->module_id, ("A symbol map is expected in " +
                                               fs::absolute(perf_map_path).string() +
                                               ", but it hasn't been found!").c_str(),
                             true, false, "General");
              result.perf_maps_expected = true;
            }
          } else if (parsed["type"] == "callchains") {
            if (!parsed["data"].is_object()) {
              adaptyst_print(this->module_id, ("Message received from profiler \"" +
                                               profiler->get_name() + "\" "
                                               "is a JSON object of type \"callchains\", "
                                               "but its \"data\" "
                                               "element is not a JSON object, ignoring.").c_str(), true, false, "General");
              continue;
            }

            File callchain_file(dir, "callchains", ".json");
            if (!(callchain_file.get_ostream()
                  << parsed["data"].dump() << std::endl)) {
              adaptyst_print(this->module_id, "Could not write data to callchains.json",
                             true, true, "General");
            }
          } else if (parsed["type"] == "sources") {
            if (!parsed["data"].is_object()) {
              adaptyst_print(this->module_id, ("Message received from profiler \"" +
                                               profiler->get_name() + "\" "
                                               "is a JSON object of type \"sources\", but its \"data\" "
                                               "element is not a JSON object, ignoring.").c_str(), true, false, "General");
              continue;
            }

            int index = -1;
            for (auto &elem : parsed["data"].items()) {
              index++;

              if (!elem.value().is_array()) {
                adaptyst_print(this->module_id, ("Element \"" + elem.key() + "\" in the data object of "
                                                 "type \"sources\" received from profiler \"" +
                                                 profiler->get_name() + "\" is not a JSON array, "
                                                 "ignoring this element.").c_str(), true, false, "General");
                continue;
              }

              if (fs::exists(elem.key())) {
                if (result.dso_offsets.find(elem.key()) == result.dso_offsets.end()) {
                  result.dso_offsets[elem.key()] = std::unordered_set<std::string>();
                }

                for (auto &offset : elem.value()) {
                  result.dso_offsets[elem.key()].insert(offset);
                }
              }
            }
          } else if (parsed["type"] == "sample" && this->profile_start_set) {
            nlohmann::json obj = parsed["data"];
            std::string event_type, pid, tid;
            unsigned long long timestamp, period;
            std::vector<std::pair<std::string, std::string> > callchain;
            try {
              event_type = obj["event_type"];
              pid = obj["pid"];
              tid = obj["tid"];
              timestamp = obj["time"];
              period = obj["period"];
              callchain = obj["callchain"].template get<
                std::vector<std::pair<std::string, std::string> > >();
            } catch (...) {
              adaptyst_print(this->module_id, "The recently received sample JSON is invalid, ignoring.",
                             true, false, "General");
              continue;
            }

            if (!first_event_received) {
              first_event_received = true;

              if (event_type == "offcpu-time" || event_type == "task-clock") {
                extra_event_name = "";

                if (timestamp - period < this->profile_start) {
                  period = timestamp - this->profile_start;
                }
              } else {
                extra_event_name = event_type;
              }
            } else if ((extra_event_name != "" && event_type != extra_event_name) ||
                       (extra_event_name == "" && event_type != "offcpu-time" && event_type != "task-clock")) {
              adaptyst_print(this->module_id, ("The recently received sample JSON is of different event type than expected "
                                               "(received: " + event_type + ", expected: " +
                                               (extra_event_name == "" ? "task-clock or offcpu-time" : extra_event_name) +
                                               "), ignoring.").c_str(), true, false, "General");
              continue;
            }

            Path pid_tid_dir = dir / pid / tid;

            if (event_type == "offcpu-time") {
              Array<std::pair<
                unsigned long long, unsigned long long> > offcpu(pid_tid_dir, "offcpu");

              if (timestamp - this->profile_start - period < 0) {
                offcpu.push_back({0, timestamp - this->profile_start});
              } else {
                offcpu.push_back(
                                 {timestamp - this->profile_start - period, period});
              }
            }

            std::string pid_tid = pid + "_" + tid;

            if (untimed_data_map.find(pid_tid) == untimed_data_map.end()) {
              untimed_data_map[pid_tid] = nlohmann::json::object();
              untimed_data_map[pid_tid]["name"] = "all";
              untimed_data_map[pid_tid]["children"] = nlohmann::json::object();
              untimed_data_map[pid_tid]["cold_value"] = 0;
              untimed_data_map[pid_tid]["hot_value"] = 0;
              untimed_data_map[pid_tid]["value"] = 0;
              untimed_data_map[pid_tid]["pid"] = pid;
              untimed_data_map[pid_tid]["tid"] = tid;
            }

            if (timed_data_map.find(pid_tid) == timed_data_map.end()) {
              timed_data_map[pid_tid] = nlohmann::json::object();
              timed_data_map[pid_tid]["name"] = "all";
              timed_data_map[pid_tid]["children"] = nlohmann::json::array();
              timed_data_map[pid_tid]["cold_value"] = 0;
              timed_data_map[pid_tid]["hot_value"] = 0;
              timed_data_map[pid_tid]["value"] = 0;
              timed_data_map[pid_tid]["pid"] = pid;
              timed_data_map[pid_tid]["tid"] = tid;
            }

            this->save_sample(&untimed_data_map[pid_tid], callchain,
                              period, false, event_type == "offcpu-time");
            this->save_sample(&timed_data_map[pid_tid], callchain,
                              period, true, event_type == "offcpu-time");

            pid_tid_dir.set_metadata<
              unsigned long long>("sampled_period",
                                  pid_tid_dir.get_metadata<
                                  unsigned long long>("sampled_period", 0) + period);
          } else if (parsed["type"] == "syscall") {
            thread_tree_connection = true;

            nlohmann::json obj = parsed["data"];
            std::string ret_value;
            std::vector<std::pair<std::string, std::string> > callchain;

            try {
              ret_value = obj["ret_value"];
              callchain = obj["callchain"].template get<
                std::vector<std::pair<std::string, std::string> > >();
            } catch (...) {
              std::cerr << "The recently-received syscall JSON is invalid, ignoring." << std::endl;
              continue;
            }

            tid_dict[ret_value] = callchain;
          } else if (parsed["type"] == "syscall_meta") {
            thread_tree_connection = true;

            nlohmann::json obj = parsed["data"];
            std::string syscall_type, comm_name, pid, tid, ret_value;
            unsigned long long time;

            try {
              syscall_type = obj["subtype"];
              comm_name = obj["comm"];
              pid = obj["pid"];
              tid = obj["tid"];
              time = obj["time"];
              ret_value = obj["ret_value"];
            } catch (...) {
              std::cerr << "The recently-received syscall tree JSON is invalid, ignoring." << std::endl;
              continue;
            }

            std::string pid_tid = pid + "/" + tid;
            bool added_to_name_time_dict = false;

            if (tree.find(tid) == tree.end()) {
              tree[tid] = "";
              added_list.push_back(std::make_pair(time, tid));

              name_time_dict[tid].push_back(std::make_pair(comm_name, time));
              added_to_name_time_dict = true;
            }

            combo_dict[tid] = pid + "/" + tid;

            if (syscall_type == "new_proc") {
              if (tree.find(ret_value) == tree.end()) {
                added_list.push_back(std::make_pair(time, ret_value));
              }

              tree[ret_value] = tid;
              combo_dict[ret_value] = "?/" + ret_value;
              name_time_dict[ret_value].push_back(std::make_pair(comm_name, time));
            } else if (syscall_type == "execve" && !added_to_name_time_dict) {
              name_time_dict[tid].push_back(std::make_pair(comm_name, time));
            } else if (syscall_type == "exit") {
              exit_time_dict[tid] = time;
            }
          }
        } catch (nlohmann::json::exception) {
          adaptyst_print(this->module_id, ("Message received from profiler \"" +
                                           profiler->get_name() +
                                           "\" "
                                           "is not valid JSON, ignoring.")
                         .c_str(), true,
                         false, "General");
        } catch (std::exception &e) {
          adaptyst_print(this->module_id, "Error", true, false, "General");
          throw e;
        }
      }
    } catch (ConnectionException &e) {
      result.error = true;
      result.exception = e;
    }

    if (thread_tree_connection) {
      nlohmann::json json_tree = nlohmann::json::object();

      json_tree["spawning_callchains"] = tid_dict;
      json_tree["tree"] = nlohmann::json::array();

      nlohmann::json &thread_results = json_tree["tree"];
      std::unordered_set<std::string> added_identifiers;

      for (int i = 0; i < added_list.size(); i++) {
        std::string k = added_list[i].second;
        std::string p = tree[k];

        if (!p.empty() && added_identifiers.find(p) == added_identifiers.end()) {
          continue;
        }

        added_identifiers.insert(k);

        nlohmann::json elem;
        elem["identifier"] = k;
        elem["tag"] = nlohmann::json::array();

        int dominant_name_index = 0;
        int dominant_name_time = 0;
        for (int i = 1; i < name_time_dict[k].size(); i++) {
          if (name_time_dict[k][i].second - name_time_dict[k][i - 1].second > dominant_name_time) {
            dominant_name_index = i - 1;
            dominant_name_time = name_time_dict[k][i].second - name_time_dict[k][i - 1].second;
          }
        }

        if (exit_time_dict.find(k) == exit_time_dict.end() ||
            exit_time_dict[k] - name_time_dict[k][name_time_dict[k].size() - 1].second > dominant_name_time) {
          dominant_name_index = name_time_dict[k].size() - 1;
        }

        elem["tag"][0] = name_time_dict[k][dominant_name_index].first;
        elem["tag"][1] = combo_dict[k];
        elem["tag"][2] = name_time_dict[k][0].second;

        if (exit_time_dict.find(k) != exit_time_dict.end()) {
          elem["tag"][3] = exit_time_dict[k] - name_time_dict[k][0].second;
        } else {
          elem["tag"][3] = -1;
        }

        if (p.empty()) {
          elem["parent"] = nullptr;
        } else {
          elem["parent"] = p;
        }

        thread_results.push_back(elem);
      }

      for (auto &elem : thread_results) {
        if (this->profile_start >= elem["tag"][2]) {
          elem["tag"][3] = (unsigned long long)elem["tag"][3] - (this->profile_start - (unsigned long long)elem["tag"][2]);
          elem["tag"][2] = 0;
        } else {
          elem["tag"][2] = (unsigned long long)elem["tag"][2] - this->profile_start;
        }
      }

      File thread_tree_file(dir, "threads", ".json");
      if (!(thread_tree_file.get_ostream() << json_tree.dump() << std::endl)) {
        adaptyst_print(this->module_id, "Could not write data to threads.json", true, true,
                       "General");
      }
    } else {
      for (auto &entry : untimed_data_map) {
        nlohmann::json &obj = entry.second;
        std::string pid = obj["pid"];
        std::string tid = obj["tid"];

        obj.erase("pid");
        obj.erase("tid");

        std::deque<nlohmann::json *> elem_queue;
        elem_queue.push_back(&obj);

        while (!elem_queue.empty()) {
          nlohmann::json *elem_ptr = elem_queue.front();
          nlohmann::json &elem = *elem_ptr;

          elem["children_tmp"] = nlohmann::json::array();

          for (auto &entry : elem["children"].items()) {
            elem["children_tmp"].push_back(nlohmann::json::object());
            elem["children_tmp"][elem["children_tmp"].size() - 1].swap(entry.value());
          }

          elem["children"].swap(elem["children_tmp"]);
          elem.erase("children_tmp");

          for (auto &entry : elem["children"]) {
            elem_queue.push_back(&entry);
          }

          elem_queue.pop_front();
        }

        fs::path path = fs::path(dir.get_path_name()) / pid / tid / "untimed.json";
        std::ofstream stream(path);

        if (!stream) {
          throw std::runtime_error(("Could not open " + path.string() + " for writing").c_str());
        }

        stream << obj.dump() << std::endl;

        if (!stream) {
          throw std::runtime_error(("Could not write to " + path.string() + ". Do you have "
                                    "enough disk space?").c_str());
        }
      }

      for (auto &entry : timed_data_map) {
        nlohmann::json &obj = entry.second;
        std::string pid = obj["pid"];
        std::string tid = obj["tid"];

        obj.erase("pid");
        obj.erase("tid");

        fs::path path = fs::path(dir.get_path_name()) / pid / tid / "timed.json";
        std::ofstream stream(path);

        if (!stream) {
          throw std::runtime_error(("Could not open " + path.string() + " for writing").c_str());
        }

        stream << obj.dump() << std::endl;

        if (!stream) {
          throw std::runtime_error(("Could not write to " + path.string() + ". Do you have "
                                    "enough disk space?").c_str());
        }
      }
    }

    return result;
  }

public:
  static CPULinuxModule *instance;

  CPULinuxModule(amod_t module_id) {
    this->module_id = module_id;
  }

  bool init() {
    option *buf_size_opt = adaptyst_get_option(this->module_id, "buffer_size");
    option *warmup_opt = adaptyst_get_option(this->module_id, "warmup");
    option *freq_opt = adaptyst_get_option(this->module_id, "freq");
    option *buffer_opt = adaptyst_get_option(this->module_id, "buffer");
    option *off_cpu_freq_opt = adaptyst_get_option(this->module_id, "off_cpu_freq");
    option *off_cpu_buffer_opt = adaptyst_get_option(this->module_id, "off_cpu_buffer");
    option *event_strs_opt = adaptyst_get_option(this->module_id, "events");
    option *filter_opt = adaptyst_get_option(this->module_id, "filter");
    option *mark_opt = adaptyst_get_option(this->module_id, "filter_mark");
    option *capture_mode_opt = adaptyst_get_option(this->module_id, "capture_mode");
    option *perf_path_opt = adaptyst_get_option(this->module_id, "perf_path");
    option *perf_script_path_opt = adaptyst_get_option(this->module_id, "perf_script_path");

    unsigned int buf_size = *(unsigned int *)buf_size_opt->data;
    unsigned int warmup = *(unsigned int *)warmup_opt->data;
    unsigned int freq = *(unsigned int *)freq_opt->data;
    unsigned int buffer = *(unsigned int *)buffer_opt->data;
    int off_cpu_freq = *(int *)off_cpu_freq_opt->data;
    unsigned int off_cpu_buffer = *(unsigned int *)off_cpu_buffer_opt->data;

    std::vector<std::string> event_strs;
    if (event_strs_opt->len > 0) {
      const char **event_strs_cstrs = *(const char ***)event_strs_opt->data;
      for (int i = 0; i < event_strs_opt->len; i++) {
        event_strs.push_back(std::string(event_strs_cstrs[i]));
      }
    }

    std::string filter_str(*((const char **)filter_opt->data));
    bool mark = *(bool *)mark_opt->data;
    std::string capture_mode(*(const char **)capture_mode_opt->data);

    std::string cpu_mask(adaptyst_get_cpu_mask(this->module_id));
    CPUConfig cpu_config(cpu_mask);

    if (buf_size >= 1) {
      this->buf_size = buf_size;
    } else {
      adaptyst_set_error(this->module_id, "\"buffer_size\" must be greater than or equal to 1.");
      return false;
    }

    if (warmup >= 1) {
      this->warmup = warmup;
    } else {
      adaptyst_set_error(this->module_id, "\"warmup\" must be greater than or equal to 1.");
      return false;
    }

    if (freq >= 1) {
      this->freq = freq;
    } else {
      adaptyst_set_error(this->module_id, "\"freq\" must be greater than or equal to 1.");
      return false;
    }

    if (buffer >= 1) {
      this->buffer = buffer;
    } else {
      adaptyst_set_error(this->module_id, "\"buffer\" must be greater than or equal to 1.");
      return false;
    }

    if (off_cpu_freq >= -1) {
      this->off_cpu_freq = off_cpu_freq;
    } else {
      adaptyst_set_error(this->module_id, "\"off_cpu_freq\" must be greater than or equal to -1.");
      return false;
    }

    if (off_cpu_buffer >= 0) {
      this->off_cpu_buffer = off_cpu_buffer;
    } else {
      adaptyst_set_error(this->module_id, "\"off_cpu_buffer\" must be greater than or equal to 0.");
      return false;
    }

    int roofline_events = 0;

#if defined(ADAPTYST_ROOFLINE) && defined(BOOST_ARCH_X86) && defined(BOOST_COMP_GNUC)
    option *roofline_freq_opt = adaptyst_get_option(this->module_id, "roofline");
    option *roofline_benchmark_path_opt = adaptyst_get_option(this->module_id, "roofline_benchmark_path");
    option *carm_tool_path_opt = adaptyst_get_option(this->module_id, "carm_tool_path");

    unsigned int roofline_freq = *(unsigned int *)roofline_freq_opt->data;
    this->roofline_freq = roofline_freq;

    if (roofline_freq >= 1) {
      __builtin_cpu_init();

      std::string freq = std::to_string(roofline_freq);

      if (__builtin_cpu_is("intel")) {
        event_strs.push_back("fp_arith_inst_retired.scalar_single," + freq +
                             ",CARM_INTEL_SSP,unit(s)");
        event_strs.push_back("fp_arith_inst_retired.scalar_double," + freq +
                             ",CARM_INTEL_SDP,unit(s)");
        event_strs.push_back("fp_arith_inst_retired.128b_packed_single," +
                             freq + ",CARM_INTEL_SSESP,unit(s)");
        event_strs.push_back("fp_arith_inst_retired.128b_packed_double," +
                             freq + ",CARM_INTEL_SSEDP,unit(s)");
        event_strs.push_back("fp_arith_inst_retired.256b_packed_single," +
                             freq + ",CARM_INTEL_AVX2SP,unit(s)");
        event_strs.push_back("fp_arith_inst_retired.256b_packed_double," +
                             freq + ",CARM_INTEL_AVX2DP,unit(s)");
        event_strs.push_back("fp_arith_inst_retired.512b_packed_single," +
                             freq + ",CARM_INTEL_AVX512SP,unit(s)");
        event_strs.push_back("fp_arith_inst_retired.512b_packed_double," +
                             freq + ",CARM_INTEL_AVX512DP,unit(s)");
        event_strs.push_back("mem_inst_retired.any," + freq +
                             ",CARM_INTEL_MEM_LDST,unit(s)");

        roofline_events = 9;
      } else if (__builtin_cpu_is("amd")) {
        event_strs.push_back("retired_sse_avx_operations:sp_mult_add_flops," + freq +
                             ",CARM_AMD_SPFMA,unit(s)");
        event_strs.push_back("retired_sse_avx_operations:dp_mult_add_flops," + freq +
                             ",CARM_AMD_DPFMA,unit(s)");
        event_strs.push_back("retired_sse_avx_operations:sp_add_sub_flops," + freq +
                             ",CARM_AMD_SPADD,unit(s)");
        event_strs.push_back("retired_sse_avx_operations:dp_add_sub_flops," + freq +
                             ",CARM_AMD_DPADD,unit(s)");
        event_strs.push_back("retired_sse_avx_operations:sp_mult_flops," + freq +
                             ",CARM_AMD_SPMUL,unit(s)");
        event_strs.push_back("retired_sse_avx_operations:dp_mult_flops," + freq +
                             ",CARM_AMD_DPMUL,unit(s)");
        event_strs.push_back("retired_sse_avx_operations:sp_div_flops," + freq +
                             ",CARM_AMD_SPDIV,unit(s)");
        event_strs.push_back("retired_sse_avx_operations:dp_div_flops," + freq +
                             ",CARM_AMD_DPDIV,unit(s)");
        event_strs.push_back("ls_dispatch:ld_dispatch," + freq + ",CARM_AMD_LD,unit(s)");
        event_strs.push_back("ls_dispatch:store_dispatch," + freq + ",CARM_AMD_STORE,unit(s)");

        roofline_events = 10;
      } else {
        adaptyst_set_error(this->module_id, "Neither an Intel nor an AMD CPU has been detected! "
                           "Roofline profiling in Adaptyst is currently supported "
                           "only for these CPUs.");
        return false;
      }

      fs::path local_config_dir(adaptyst_get_local_config_dir(this->module_id));

      if (roofline_benchmark_path_opt->data) {
        fs::path roofline_benchmark_path(*(const char **)roofline_benchmark_path_opt->data);

        if (!fs::exists(roofline_benchmark_path)) {
          adaptyst_set_error(this->module_id, (roofline_benchmark_path.string() +
                                               " does not exist!").c_str());
          return false;
        }

        if (!fs::is_regular_file(fs::canonical(roofline_benchmark_path))) {
          adaptyst_set_error(this->module_id, (roofline_benchmark_path.string() + " does not point to "
                                               "a regular file!").c_str());
          return false;
        }

        this->roofline_benchmark_path = roofline_benchmark_path;
      } else if (fs::exists(local_config_dir / "roofline.csv") &&
                 fs::is_regular_file(fs::canonical(local_config_dir / "roofline.csv"))) {
        this->roofline_benchmark_path = local_config_dir / "roofline.csv";
      } else if (carm_tool_path_opt->data) {
        fs::path carm_tool_path(*(const char **)carm_tool_path_opt->data);
        fs::path tmp_dir(adaptyst_get_tmp_dir(this->module_id));

        std::vector<std::string> command = {
          "python3", carm_tool_path / "run.py", "-out", tmp_dir
        };

        Process process(command);
        process.set_redirect_stdout_to_terminal();
        process.start();

        int exit_code = process.join();

        if (exit_code != 0) {
          adaptyst_set_error(this->module_id, ("The CARM tool has returned a non-zero exit code " +
                                               std::to_string(exit_code) + ".").c_str());
          return false;
        }

        if (fs::copy_file(tmp_dir / "roofline" / "unnamed_roofline.csv",
                          local_config_dir / "roofline.csv")) {
          this->roofline_benchmark_path = local_config_dir / "roofline.csv";
        } else {
          adaptyst_print(this->module_id, "Could not copy the roofline benchmark results to the Adaptyst local "
                         "config directory! Continuing, but Adaptyst will have to run roofline "
                         "benchmarking again next time.", true, false, "General");
          this->roofline_benchmark_path = tmp_dir / "roofline" / "unnamed_roofline.csv";
        }
      } else {
        adaptyst_set_error(this->module_id, "\"roofline_benchmark_path\" or \"carm_tool_path\" "
                           "must be provided "
                           "when \"roofline\" is set and there's no roofline.csv in the Adaptyst "
                           "local config directory.");
        return false;
      }
    } else if (roofline_freq != 0) {
      adaptyst_set_error(this->module_id, "\"roofline\" must be greater than or equal to 1.");
      return false;
    }
#endif

    int index = 0;
    for (auto &event_str : event_strs) {
      std::smatch match;
      std::string error = "";

      if (!std::regex_match(event_str, match, std::regex("^(.+),([0-9\\.]+),(.+),(.+)$"))) {
        error = "events: "
          "The value \"" + event_str + "\" must be in form of EVENT,PERIOD,TITLE,UNIT "
          "(PERIOD must be a number).";
      }

      if (index < event_strs.size() - roofline_events &&
          std::regex_match(match[3].str(), std::regex("^CARM_.*$"))) {
        error = "events: "
          "The title in \"" + event_str + "\" starts with a reserved keyword "
          "CARM_, you cannot use it.";
      }

      if (error != "") {
        adaptyst_set_error(this->module_id, error.c_str());
        return false;
      }

      std::string event_name = match[1];
      int period = std::stoi(match[2]);
      std::string human_title = match[3];
      std::string unit = match[4];

      this->events.push_back(PerfEvent(event_name, period, buffer,
                                       human_title, unit));
      index++;
    }

    Perf::Filter filter;
    std::string allowdenylist_path;
    std::string allowdenylist_type;

    filter.mode = Perf::FilterMode::NONE;
    filter.mark = mark;

    if (filter_str != "") {
      std::smatch match;

      if (!std::regex_match(filter_str, match,
                            std::regex("^(deny|allow|python)\\:(.+)$"))) {
        adaptyst_set_error(this->module_id, "The value of \"filter\" is incorrect.");
        return false;
      }

      if (match[1] == "allow") {
        filter.mode = Perf::FilterMode::ALLOW;
        allowdenylist_path = match[2];
        allowdenylist_type = "allowlist";
      } else if (match[1] == "deny") {
        filter.mode = Perf::FilterMode::DENY;
        allowdenylist_path = match[2];
        allowdenylist_type = "denylist";
      } else {
        filter.mode = Perf::FilterMode::PYTHON;
        filter.script_path = fs::canonical(match[2].str()).string();
      }
    }

    if (allowdenylist_path != "") {
      std::vector<std::vector<std::string > > allowdenylist;
      adaptyst_print(this->module_id, ("Reading " + allowdenylist_type + "...").c_str(),
                     true, false, "General");

      auto process_stream =
        [this](std::istream &stream,
           std::vector<std::vector<std::string> > &list) {
          std::vector<std::string> elements;
          int line = 1;
          while (stream) {
            std::string input = "";
            std::getline(stream, input);

            if (input.length() > 0 && input[0] != '#') {
              if (input == "OR") {
                list.push_back(elements);
                elements.clear();
              } else if (std::regex_match(input,
                                          std::regex("^(SYM|EXEC|ANY) .+$"))) {
                elements.push_back(input);
              } else {
                adaptyst_set_error(this->module_id, ("Line " + std::to_string(line) + " is non-empty and "
                                                     "invalid!").c_str());
                return false;
              }
            }

            line++;
          }

          if (!elements.empty()) {
            list.push_back(elements);
          }

          return true;
        };

      std::ifstream stream(allowdenylist_path);

      if (!stream) {
        adaptyst_set_error(this->module_id, ("Cannot read " + allowdenylist_path + "!").c_str());
        return false;
      }

      if (!process_stream(stream, allowdenylist)) {
        return false;
      }

      filter.data = allowdenylist;
    }

    this->filter = filter;

    if (capture_mode == "kernel") {
      this->capture_mode = Perf::CaptureMode::KERNEL;
    } else if (capture_mode == "user") {
      this->capture_mode = Perf::CaptureMode::USER;
    } else if (capture_mode == "both") {
      this->capture_mode = Perf::CaptureMode::BOTH;
    } else {
      adaptyst_set_error(this->module_id, "\"capture_mode\" can be either \"kernel\", \"user\", "
                         "or \"both\".");
      return false;
    }

    this->cpu_config = cpu_config;

    fs::path perf_path(*(const char **)perf_path_opt->data);

    if (perf_path.is_relative()) {
      perf_path = fs::path(adaptyst_get_library_dir(this->module_id)).parent_path() / perf_path;
    }

    fs::path perf_bin_path = perf_path / "bin" / "perf";
    fs::path perf_python_path = perf_path / "libexec" / "perf-core" / "scripts" /
      "python" / "Perf-Trace-Util" / "lib" / "Perf" / "Trace";

    if (!fs::exists(perf_bin_path)) {
      adaptyst_set_error(this->module_id, (perf_bin_path.string() + " does not exist!").c_str());
      return false;
    }

    if (!fs::is_regular_file(fs::canonical(perf_bin_path))) {
      adaptyst_set_error(this->module_id, (perf_bin_path.string() +
                                           " does not point to a regular file!").c_str());
      return false;
    }

    if (!fs::exists(perf_python_path)) {
      adaptyst_set_error(this->module_id, (perf_python_path.string() + " does not exist!").c_str());
      return false;
    }

    if (!fs::is_directory(fs::canonical(perf_python_path))) {
      adaptyst_set_error(this->module_id,
                         (perf_python_path.string() + " does not point to a directory!")
                         .c_str());
      return false;
    }

    this->perf_bin_path = perf_bin_path;
    this->perf_python_path = perf_python_path;

    fs::path perf_script_path(*(const char **)perf_script_path_opt->data);

    if (perf_script_path.is_relative()) {
      perf_script_path = fs::path(adaptyst_get_library_dir(this->module_id)).parent_path() / perf_script_path;
    }

    if (!fs::exists(perf_script_path)) {
      adaptyst_set_error(this->module_id, (perf_script_path.string() + " does not exist!").c_str());
      return false;
    }

    if (!fs::is_directory(fs::canonical(perf_script_path))) {
      adaptyst_set_error(this->module_id, (perf_script_path.string() +
                                           " does not point to a directory!").c_str());
      return false;
    }

    this->perf_script_path = perf_script_path;

    adaptyst_set_will_profile(this->module_id, true);

    return true;
  }

  bool process(const char *sdfg) {
    try {
      adaptyst_print(this->module_id, "Preparing profilers and verifying their requirements...",
                     false, false, "General");

      bool requirements_fulfilled = true;
      std::string last_requirement = "";

      std::vector<std::pair<std::unique_ptr<Profiler>, Path> > profilers;

      PerfEvent main(this->freq,
                     this->off_cpu_freq,
                     this->buffer,
                     this->off_cpu_buffer);
      PerfEvent syscall_tree;

      PipeAcceptor::Factory generic_acceptor_factory;
      Path module_dir(adaptyst_get_module_dir(this->module_id));

      profilers.push_back({std::make_unique<Perf>(generic_acceptor_factory,
                                                  this->buf_size,
                                                  this->perf_bin_path,
                                                  this->perf_python_path,
                                                  this->perf_script_path,
                                                  syscall_tree,
                                                  this->cpu_config,
                                                  "Thread tree profiler",
                                                  this->capture_mode,
                                                  this->filter), module_dir});

      Path walltime_dir = module_dir / "walltime";
      walltime_dir.set_metadata<std::string>("title", "Wall time");
      walltime_dir.set_metadata<std::string>("unit", "ns");

      profilers.push_back({std::make_unique<Perf>(generic_acceptor_factory,
                                                  this->buf_size,
                                                  this->perf_bin_path,
                                                  this->perf_python_path,
                                                  this->perf_script_path,
                                                  main, this->cpu_config,
                                                  "On-CPU/Off-CPU profiler",
                                                  this->capture_mode,
                                                  this->filter), walltime_dir});

      for (auto &event : this->events) {
        Path metric_dir = module_dir / event.get_name();
        metric_dir.set_metadata<std::string>("title",
                                             event.get_human_title());
        metric_dir.set_metadata<std::string>("unit",
                                             event.get_unit());
        profilers.push_back({std::make_unique<Perf>(generic_acceptor_factory,
                                                    this->buf_size,
                                                    this->perf_bin_path,
                                                    this->perf_python_path,
                                                    this->perf_script_path,
                                                    event,
                                                    this->cpu_config,
                                                    event.get_name(),
                                                    this->capture_mode,
                                                    this->filter), metric_dir});
      }

#if defined(ADAPTYST_ROOFLINE) && defined(BOOST_ARCH_X86) && defined(BOOST_COMP_GNUC)
      if (this->roofline_freq > 0) {
        std::ifstream roofline(this->roofline_benchmark_path);

        try {
          if (!fs::copy_file(this->roofline_benchmark_path,
                             fs::path(adaptyst_get_module_dir(this->module_id)) / "roofline.csv",
                             fs::copy_options::overwrite_existing)) {
            adaptyst_set_error(this->module_id, "Could not copy the roofline benchmarking results: "
                               "std::filesystem::copy_file returned false");
            return false;
          }
        } catch (std::exception &e) {
          adaptyst_set_error(this->module_id, ("Could not copy the roofline benchmarking results: " +
                                               std::string(e.what())).c_str());
          return false;
        }
      }
#endif

      for (int i = 0; i < profilers.size() && requirements_fulfilled; i++) {
        std::vector<std::unique_ptr<Requirement> > &requirements = profilers[i].first->get_requirements();

        for (int j = 0; j < requirements.size() && requirements_fulfilled; j++) {
          last_requirement = requirements[j]->get_name();
          requirements_fulfilled = requirements_fulfilled && requirements[j]->check();
        }
      }

      if (!requirements_fulfilled) {
        adaptyst_set_error(this->module_id, ("Requirement \"" + last_requirement + "\" is not met!").c_str());
        return false;
      }

      adaptyst_print(this->module_id, "Starting profilers and waiting for them to signal their "
                     "readiness...", false, false, "General");

      profile_info *profile = adaptyst_get_profile_info(this->module_id);
      std::vector<std::future<ConnectionResult> > threads;

      int index = 0;

      for (auto &pair : profilers) {
        auto &profiler = pair.first;
        auto &dir = pair.second;

        profiler->start(profile->data.pid, true);
        for (auto &connection : profiler->get_connections()) {
          threads.push_back(std::async([this, &dir, &profiler, &connection]() {
            return this->process_connection(dir, profiler, connection);
          }));
          index++;
        }
      }

      adaptyst_print(this->module_id, ("All profilers have signalled their readiness, waiting " +
                                       std::to_string(this->warmup) + " second(s)...").c_str(), false, false, "General");
      std::this_thread::sleep_for(this->warmup * 1s);

      adaptyst_print(this->module_id, "The warmup has been completed.", true, false, "General");

      adaptyst_profile_notify(this->module_id);

      unsigned long long timestamp = adaptyst_get_workflow_start_time(this->module_id);

      if (adaptyst_get_internal_error_code(this->module_id) != ADAPTYST_OK) {
        adaptyst_set_error(this->module_id, "Calling adaptyst_get_workflow_start_time() to get the profile start "
                           "timestamp has failed!");
        return false;
      }

      this->profile_start = timestamp;
      this->profile_start_set = true;

      adaptyst_profile_wait(this->module_id);

      adaptyst_print(this->module_id, "Finishing processing results...", false, false, "General");

      std::vector<
        std::unordered_map<std::string, std::unordered_set<std::string> > > dso_offsets;
      bool perf_maps_expected = false;
      int dso_offsets_size = 0;

      for (auto &thread : threads) {
        ConnectionResult result = thread.get();

        if (result.perf_maps_expected) {
          perf_maps_expected = true;
        }

        dso_offsets.push_back(result.dso_offsets);
        dso_offsets_size += result.dso_offsets.size();
      }

      bool profiler_error = false;

      for (auto &pair : profilers) {
        auto &profiler = pair.first;
        if (profiler->wait() != 0) {
          profiler_error = true;
        }
      }

      if (profiler_error) {
        adaptyst_set_error(this->module_id, "One or more profilers have encountered an error!");
        return false;
      }

      nlohmann::json sources_json = nlohmann::json::object();

      // The number of threads needs to stay at 1 here because of a bug
      // (a race condition?) causing randomly addr2line not to terminate after
      // the stdin pipe is closed.
      //
      // TODO: fix this
      boost::asio::thread_pool pool(1);

      std::pair<std::string, nlohmann::json> sources[dso_offsets_size];
      std::unordered_set<fs::path> source_files[dso_offsets_size];

      index = 0;

      for (auto &map : dso_offsets) {
        for (auto &elem : map) {
          auto process_func = [index, elem, this, &sources, &source_files]() {
            std::vector<std::string> cmd = {"addr2line", "-e", elem.first};
            Process process(cmd);
            process.start(false, this->cpu_config, true);

            nlohmann::json result;
            std::unordered_set<fs::path> files;

            for (auto &offset : elem.second) {
              std::string to_write = offset + '\n';
              process.write_stdin((char *)to_write.c_str(), to_write.size());
              std::vector<std::string> parts;
              boost::split(parts, process.read_line(), boost::is_any_of(":"));

              if (parts.size() == 2) {
                try {
                  result[offset] = nlohmann::json::object();
                  result[offset]["file"] = parts[0];
                  result[offset]["line"] = std::stoi(parts[1]);
                  files.insert(parts[0]);
                } catch (...) {
                }
              }
            }

            sources[index] = std::make_pair(elem.first, result);
            source_files[index] = files;
          };

          boost::asio::post(pool, process_func);
          index++;
        }
      }

      pool.join();

      std::unordered_set<fs::path> src_paths;

      for (int i = 0; i < dso_offsets_size; i++) {
        if (!sources_json.contains(sources[i].first)) {
          sources_json[sources[i].first] = nlohmann::json::object();
        }

        sources_json[sources[i].first].swap(sources[i].second);

        for (auto &elem : source_files[i]) {
          src_paths.insert(elem);
        }
      }

      {
        fs::path sources_file_path = fs::path(adaptyst_get_module_dir(this->module_id)) / "sources.json";
        std::ofstream sources_file(sources_file_path);

        if (!sources_file) {
          adaptyst_set_error(this->module_id,
                             ("Could not open " + sources_file_path.string() + " for writing!").c_str());
          return false;
        }

        if (!(sources_file << sources_json.dump() << std::endl)) {
          adaptyst_set_error(this->module_id,
                             ("Could not write data to " + sources_file_path.string()).c_str());
          return false;
        }
      }

      if (perf_maps_expected) {
        adaptyst_print(this->module_id, "One or more expected symbol maps haven't been found! "
                       "This is not an error, but some symbol names will be unresolved and "
                       "point to the name of an expected map file instead.", true, false, "General");
        adaptyst_print(this->module_id, "If it's not desired, make sure that your profiled "
                       "program is configured to emit \"perf\" symbol maps.", true, false, "General");
      }

      const char *paths[src_paths.size()];

      int path_index = 0;
      for (const fs::path &path : src_paths) {
        paths[path_index++] = path.c_str();
      }

      adaptyst_process_src_paths(this->module_id, paths, src_paths.size());

      return true;
    } catch (std::exception &e) {
      adaptyst_set_error(this->module_id, ("An exception has occurred: " +
                                           std::string(e.what())).c_str());
      return false;
    }
  }
};

CPULinuxModule *CPULinuxModule::instance = nullptr;
amod_t module_id = 0;

extern "C" {
  bool adaptyst_module_init(amod_t mod_id) {
    try {
      CPULinuxModule::instance = new CPULinuxModule(mod_id);
    } catch (std::bad_alloc &e) {
      adaptyst_set_error(mod_id, "Could not allocate memory for CPULinuxModule");
      return false;
    } catch (std::exception &e) {
      adaptyst_set_error(mod_id, ("An exception has occurred: " +
                                  std::string(e.what())).c_str());
      return false;
    }

    module_id = mod_id;
    return CPULinuxModule::instance->init();
  }

  bool adaptyst_module_process(amod_t module_id, const char *sdfg) {
    return CPULinuxModule::instance->process(sdfg);
  }

  void adaptyst_module_close(amod_t module_id) {
    delete CPULinuxModule::instance;
  }
}
