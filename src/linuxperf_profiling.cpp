#include "linuxperf_profiling.hpp"
#include <adaptyst/hw.h>
#include <fstream>
#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>

#ifdef BOOST_OS_UNIX
#include <sys/wait.h>
#endif

#ifdef LIBNUMA_AVAILABLE
#include <numa.h>
#endif

#define ACCEPT_TIMEOUT 5

namespace adaptyst {
  namespace ch = std::chrono;
  using namespace std::chrono_literals;

  /**
     Constructs a ServerConnInstrs object.

     @param all_connection_instrs An adaptyst-server connection
                                  instructions string sent
                                  by adaptyst-server during the initial
                                  setup phase. It is in form of
                                  "<method> <connection details>", where
                                  \<connection details\> is provided once or
                                  more than once per profiler, separated by
                                  a space character. \<connection details\>
                                  takes form of "<field1>_<field2>_..._<fieldX>"
                                  where the number of fields and their content
                                  are implementation-dependent.
  */
  ServerConnInstrs::ServerConnInstrs(std::string all_connection_instrs) {
    std::vector<std::string> parts;
    boost::split(parts, all_connection_instrs, boost::is_any_of(" "));

    if (parts.size() > 0) {
      this->type = parts[0];

      for (int i = 1; i < parts.size(); i++) {
        this->methods.push(parts[i]);
      }
    }
  }

  /**
     Gets a connection instructions string relevant to the profiler
     requesting these instructions.

     @param thread_count A number of threads expected to connect
                         to adaptyst-server from the current
                         profiler.

     @throw std::runtime_error When the sum of thread_count amongst
                               all get_instruction() calls within a
                               single ServerConnInstrs object
                               exceeds the number of \<connection details\>
                               sent by adaptyst-server.
  */
  std::string ServerConnInstrs::get_instructions(int thread_count) {
    std::string result = this->type;

    for (int i = 0; i < thread_count; i++) {
      if (this->methods.empty()) {
        throw std::runtime_error("Could not obtain server connection "
                                 "instructions for thread_count = " +
                                 std::to_string(thread_count) + ".");
      }

      result += " " + this->methods.front();
      this->methods.pop();
    }

    return result;
  }

  /**
     Constructs a PerfEventKernelSettingsReq object.

     @param max_stack  Indicates where the value of kernel.perf_event_max_stack
                       should be written to.
  */
  PerfEventKernelSettingsReq::PerfEventKernelSettingsReq(int &max_stack) : max_stack(max_stack) {}

  std::string PerfEventKernelSettingsReq::get_name() {
    return "Adequate values of kernel.perf_event settings";
  }

  bool PerfEventKernelSettingsReq::check_internal() {
    // kernel.perf_event_max_stack
    std::ifstream max_stack("/proc/sys/kernel/perf_event_max_stack");

    if (!max_stack) {
      adaptyst_print(module_id, "Could not check the value of kernel.perf_event_max_stack!",
                     true, true, "General");
      return false;
    }

    int max_stack_value;
    max_stack >> max_stack_value;

    max_stack.close();

    if (max_stack_value < 1024) {
      adaptyst_print(module_id, "kernel.perf_event_max_stack is less than 1024. Adaptyst will "
                     "crash because of this, so stopping here. Please run \"sysctl "
                     "kernel.perf_event_max_stack=1024\" (or the same command with "
                     "a number larger than 1024).", true, true, "General");
      return false;
    } else {
      this->max_stack = max_stack_value;
      adaptyst_print(module_id, ("Note that stacks with more than " + std::to_string(max_stack_value) +
                                 " entries/entry *WILL* be broken in your results! To avoid that, run "
                                 "\"sysctl kernel.perf_event_max_stack=<larger value>\".").c_str(), true, false, "General");
      adaptyst_print(module_id, "Remember that max stack values larger than 1024 are currently *NOT* "
                     "supported for off-CPU stacks (they will be capped at 1024 entries).",
                     true, false, "General");
    }

    // Done, everything's good!
    return true;
  };

  std::string NUMAMitigationReq::get_name() {
    return "NUMA balancing not interfering with profiling";
  }

  bool NUMAMitigationReq::check_internal() {
    fs::path numa_balancing_path("/proc/sys/kernel/numa_balancing");

    if (!fs::exists(numa_balancing_path)) {
      adaptyst_print(module_id, "kernel.numa_balancing does not seem to exist, so assuming "
                     "no NUMA on this machine. Note that if you actually have "
                     "NUMA, you may get broken stacks!", true, false, "General");
      return true;
    }

    std::ifstream numa_balancing(numa_balancing_path);

    if (!numa_balancing) {
      adaptyst_print(module_id, "Could not check the value of kernel.numa_balancing!",
                     true, true, "General");
      return false;
    }

    int numa_balancing_value;
    numa_balancing >> numa_balancing_value;

    numa_balancing.close();

    if (numa_balancing_value == 1) {
#ifdef LIBNUMA_AVAILABLE
      unsigned long mask = *numa_get_membind()->maskp;
      int count = 0;

      while (mask > 0 && count <= 1) {
        if (mask & 0x1) {
          count++;
        }

        mask >>= 1;
      }

      if (count > 1) {
        adaptyst_print(module_id, "NUMA balancing is enabled and Adaptyst is running on more "
                       "than 1 NUMA node!", true,
                       true, "General");
        adaptyst_print(module_id, "As this will result in broken stacks, Adaptyst will not run.",
                       true, true, "General");
        adaptyst_print(module_id, "Please disable balancing by running \"sysctl "
                       "kernel.numa_balancing=0\" or "
                       "bind Adaptyst at least memory-wise "
                       "to a single NUMA node, e.g. through numactl.", true,
                       true, "General");
        return false;
      }
#else
      adaptyst_print(module_id, "NUMA balancing is enabled, but Adaptyst is compiled without "
                     "libnuma support, so it cannot determine on how many NUMA nodes "
                     "it is running!", true, true, "General");
      adaptyst_print(module_id, "As this may result in broken stacks, Adaptyst will not run.",
                     true, true, "General");
      adaptyst_print(module_id, "Please disable balancing by running \"sysctl "
                     "kernel.numa_balancing=0\" or "
                     "recompile Adaptyst with libnuma support, followed by "
                     "binding the tool at least memory-wise "
                     "to a single NUMA node (e.g. through numactl).", true, true, "General");
      return false;
#endif
    }

    return true;
  }

  /**
     Constructs a PerfEvent object corresponding to thread tree
     profiling.

     Thread tree profiling traces all system calls relevant to
     spawning new threads/processes and exiting from them so that
     a thread/process tree can be created for later analysis.
  */
  PerfEvent::PerfEvent() {
    this->name = "<thread_tree>";
  }

  /**
     Constructs a PerfEvent object corresponding to on-CPU/off-CPU
     profiling.

     @param freq                  An on-CPU sampling frequency in Hz.
     @param off_cpu_freq          An off-CPU sampling frequency in Hz.
                                  0 disables off-CPU profiling.
     @param buffer_events         A number of on-CPU events that
                                  should be buffered before sending
                                  them for processing. 1
                                  effectively disables buffering.
     @param buffer_off_cpu_events A number of off-CPU events that
                                  should be buffered before sending
                                  them for processing. 0 leaves
                                  the default adaptive buffering, 1
                                  effectively disables buffering.
  */
  PerfEvent::PerfEvent(int freq,
                       int off_cpu_freq,
                       int buffer_events,
                       int buffer_off_cpu_events) {
    this->name = "<main>";
    this->options.push_back(std::to_string(freq));
    this->options.push_back(std::to_string(off_cpu_freq));
    this->options.push_back(std::to_string(buffer_events));
    this->options.push_back(std::to_string(buffer_off_cpu_events));
  }

  /**
     Constructs a PerfEvent object corresponding to a custom
     Linux "perf" event.

     @param name          The name of a "perf" event as displayed by
                          "perf list".
     @param period        A sampling period. The value of X means
                          "do a sample on every X occurrences of the
                          event".
     @param buffer_events A number of events that should be buffered
                          before sending them for processing. 1
                          effectively disables buffering.
     @param website_title The human-friendly title of an event.
     @param unit          The unit of an event.
  */
  PerfEvent::PerfEvent(std::string name,
                       int period,
                       int buffer_events,
                       std::string human_title,
                       std::string unit) {
    this->name = name;
    this->options.push_back(std::to_string(period));
    this->options.push_back(std::to_string(buffer_events));
    this->human_title = human_title;
    this->unit = unit;
  }

  /**
     Gets the name of a "perf" event as displayed by "perf list".
  */
  std::string PerfEvent::get_name() {
    return this->name;
  }

  /**
     Gets the human-friendly title of a "perf" event.
  */
  std::string PerfEvent::get_human_title() {
    return this->human_title;
  }

  /**
     Gets the unit of a "perf" event.
  */
  std::string PerfEvent::get_unit() {
    return this->unit;
  }

  /**
     Constructs a Perf object.

     @param acceptor_factory The factory to use for instantiating acceptors establishing
                             a connection for exchanging messages with the profiler.
     @param buf_size         The buffer size for a connection that the acceptor
                             will accept.
     @param perf_bin_path    The full path to the "perf" executable.
     @param perf_python_path The full path to the directory with "perf" Python scripts
                             (usually ending with "libexec/perf-core/scripts/python/Perf-Trace-Util/lib/Perf/Trace")
     @param perf_event       The PerfEvent object corresponding to a "perf" event
                             to be used in this "perf" instance.
     @param cpu_config       A CPUConfig object describing how CPU cores should
                             be used for profiling.
     @param name             The name of this "perf" instance.
  */
  Perf::Perf(Acceptor::Factory &acceptor_factory,
             unsigned int buf_size,
             fs::path perf_bin_path,
             fs::path perf_python_path,
             fs::path perf_script_path,
             PerfEvent &perf_event,
             CPUConfig &cpu_config,
             std::string name,
             CaptureMode capture_mode,
             Filter filter) : Profiler(acceptor_factory, buf_size),
                                          cpu_config(cpu_config) {
    this->perf_bin_path = perf_bin_path;
    this->perf_python_path = perf_python_path;
    this->perf_script_path = perf_script_path;
    this->perf_event = perf_event;
    this->name = name;
    this->max_stack = 1024;
    this->capture_mode = capture_mode;
    this->filter = filter;

    this->requirements.push_back(std::make_unique<PerfEventKernelSettingsReq>(this->max_stack));
    this->requirements.push_back(std::make_unique<NUMAMitigationReq>());
  }

  std::string Perf::get_name() {
    return this->name;
  }

  void Perf::start(pid_t pid,
                   bool capture_immediately) {
    const char *log_dir = adaptyst_get_log_dir(module_id);
    std::string node_id(adaptyst_get_node_id(module_id));

    fs::path stdout(log_dir);
    fs::path stderr_record(log_dir);
    fs::path stderr_script(log_dir);

    std::vector<std::string> argv_record;
    std::vector<std::string> argv_script;

    if (this->perf_event.name == "<thread_tree>") {
      stdout /= node_id + "_perf_script_syscall_stdout.log";
      stderr_record /= node_id + "_perf_record_syscall_stderr.log";
      stderr_script /= node_id + "_perf_script_syscall_stderr.log";

      argv_record = {this->perf_bin_path.string(), "record", "-o", "-",
        "--call-graph", "fp", "-k",
        "CLOCK_MONOTONIC", "--buffer-events", "1", "-e",
        "syscalls:sys_exit_execve,syscalls:sys_exit_execveat,"
        "sched:sched_process_fork,sched:sched_process_exit",
        "--sorted-stream", "--pid=" + std::to_string(pid)};
      argv_script = {this->perf_bin_path.string(), "script", "-i", "-", "-s",
        this->perf_script_path.string() + "/event-handler.py",
        "--demangle", "--demangle-kernel",
        "--max-stack=" + std::to_string(this->max_stack)};
    } else if (this->perf_event.name == "<main>") {
      stdout /= node_id + "_perf_script_main_stdout.log";
      stderr_record /= node_id + "_perf_record_main_stderr.log";
      stderr_script /= node_id + "_perf_script_main_stderr.log";

      argv_record = {this->perf_bin_path.string(), "record", "-o", "-",
        "--call-graph", "fp", "-k",
        "CLOCK_MONOTONIC", "--sorted-stream", "-e",
        "task-clock", "-F", this->perf_event.options[0],
        "--off-cpu", this->perf_event.options[1],
        "--buffer-events", this->perf_event.options[2],
        "--buffer-off-cpu-events", this->perf_event.options[3],
        "--pid=" + std::to_string(pid)};
      argv_script = {this->perf_bin_path.string(), "script", "-i", "-", "-s",
        this->perf_script_path.string() + "/event-handler.py",
        "--demangle", "--demangle-kernel",
        "--max-stack=" + std::to_string(this->max_stack)};
    } else {
      stdout /= node_id + "_perf_script_" + this->perf_event.name + "_stdout.log";
      stderr_record /= node_id + "_perf_record_" + this->perf_event.name + "_stderr.log";
      stderr_script /= node_id + "_perf_script_" + this->perf_event.name + "_stderr.log";

      argv_record = {this->perf_bin_path.string(), "record", "-o", "-",
        "--call-graph", "fp", "-k",
        "CLOCK_MONOTONIC", "--sorted-stream", "-e",
        this->perf_event.name + "/period=" + this->perf_event.options[0] + "/",
        "--buffer-events", this->perf_event.options[1],
        "--pid=" + std::to_string(pid)};
      argv_script = {this->perf_bin_path.string(), "script", "-i", "-", "-s",
        this->perf_script_path.string() + "/event-handler.py",
        "--demangle", "--demangle-kernel",
        "--max-stack=" + std::to_string(this->max_stack)};
    }

    if (this->capture_mode == KERNEL) {
      argv_record.push_back("--kernel-callchains");
    } else if (this->capture_mode == USER) {
      argv_record.push_back("--user-callchains");
    } else if (this->capture_mode == BOTH) {
      argv_record.push_back("--kernel-callchains");
      argv_record.push_back("--user-callchains");
    }

    this->record_proc = std::make_unique<Process>(argv_record);
    this->record_proc->set_redirect_stderr(stderr_record);

    this->script_proc = std::make_unique<Process>(argv_script);

    char *cur_pythonpath = getenv("PYTHONPATH");

    if (cur_pythonpath) {
      this->script_proc->add_env("PYTHONPATH",
                                 this->perf_python_path.string() + ":" +
                                 std::string(cur_pythonpath));
    } else {
      this->script_proc->add_env("PYTHONPATH",
                                 this->perf_python_path.string());
    }

    unsigned int threads = this->get_thread_count();
    std::vector<std::unique_ptr<Acceptor> > acceptors;
    std::stringstream instrs_stream;

    for (int i = 0; i < threads; i++) {
      acceptors.push_back(this->acceptor_factory.make_acceptor(1));
      instrs_stream << " " << acceptors[i]->get_connection_instructions();
    }

    this->script_proc->add_env("ADAPTYST_CONNECT",
                               acceptors[0]->get_type() +
                               instrs_stream.str());

    this->script_proc->set_redirect_stdout(stdout);
    this->script_proc->set_redirect_stderr(stderr_script);

    this->record_proc->set_redirect_stdout(*(this->script_proc));

    this->script_proc->start(false, this->cpu_config, true);
    this->record_proc->start(false, this->cpu_config, true);

    this->running = true;

    this->process = std::async([&, this]() {
      this->record_proc->close_stdin();
      int code = this->record_proc->join();

      if (code != 0) {
        int status = waitpid(pid, nullptr, WNOHANG);

        if (status == 0) {
          adaptyst_print(module_id, ("Profiler \"" + this->get_name() + "\" (perf-record) has "
                                     "returned non-zero exit code " + std::to_string(code) + ". "
                                     "Terminating the profiled command wrapper.").c_str(), true, true, "General");
          kill(pid, SIGTERM);
        } else {
          adaptyst_print(module_id, ("Profiler \"" + this->get_name() + "\" (perf-record) "
                                     "has returned non-zero exit code " + std::to_string(code) + " "
                                     "and the profiled command "
                                     "wrapper is no longer running.").c_str(), true, true, "General");
        }

        std::string hint = "Hint: perf-record wrapper has returned exit "
          "code " + std::to_string(code) + ", suggesting something bad "
          "happened when ";

        switch (code) {
        case Process::ERROR_STDOUT:
          adaptyst_print(module_id, (hint + "creating stdout log file.").c_str(), true, true, "General");
          break;

        case Process::ERROR_STDERR:
          adaptyst_print(module_id, (hint + "creating stderr log file.").c_str(), true, true, "General");
          break;

        case Process::ERROR_STDOUT_DUP2:
          adaptyst_print(module_id, (hint + "redirecting stdout to perf-script.").c_str(), true, true, "General");
          break;

        case Process::ERROR_STDERR_DUP2:
          adaptyst_print(module_id, (hint + "redirecting stderr to file.").c_str(), true, true, "General");
          break;
        }

        this->running = false;
        return code;
      }

      code = this->script_proc->join();

      if (code != 0) {
        int status = waitpid(pid, nullptr, WNOHANG);

        if (status == 0) {
          adaptyst_print(module_id, ("Profiler \"" + this->get_name() + "\" (perf-script) "
                                     "has returned non-zero exit code " + std::to_string(code) + ". "
                                     "Terminating the profiled command wrapper.").c_str(), true, true, "General");
          kill(pid, SIGTERM);
        } else {
          adaptyst_print(module_id, ("Profiler \"" + this->get_name() + "\" (perf-script) "
                                     "has returned non-zero exit code " + std::to_string(code) + " "
                                     "and the profiled command "
                                     "wrapper is no longer running.").c_str(), true, true, "General");
        }

        std::string hint = "Hint: perf-script wrapper has returned exit "
          "code " + std::to_string(code) + ", suggesting something bad "
          "happened when ";

        switch (code) {
        case Process::ERROR_STDOUT:
          adaptyst_print(module_id, (hint + "creating stdout log file.").c_str(), true, true, "General");
          break;

        case Process::ERROR_STDERR:
          adaptyst_print(module_id, (hint + "creating stderr log file.").c_str(), true, true, "General");
          break;

        case Process::ERROR_STDOUT_DUP2:
          adaptyst_print(module_id, (hint + "redirecting stdout to file.").c_str(), true, true, "General");
          break;

        case Process::ERROR_STDERR_DUP2:
          adaptyst_print(module_id, (hint + "redirecting stderr to file.").c_str(), true, true, "General");
          break;

        case Process::ERROR_STDIN_DUP2:
          adaptyst_print(module_id, (hint + "replacing stdin with perf-record pipe output.").c_str(),
                         true, true, "General");
          break;
        }
      }

      this->running = false;
      return code;
    });

    for (int i = 0; i < threads; i++) {
      while (true) {
        try {
          this->connections.push_back(acceptors[i]->accept(this->buf_size, ACCEPT_TIMEOUT));
          break;
        } catch (TimeoutException) {
          if (!this->running) {
            return;
          }
        }
      }
    }

    if (this->filter.mode != NONE) {
      nlohmann::json allowdenylist_json = nlohmann::json::object();

      if (this->filter.mode == ALLOW) {
        allowdenylist_json["type"] = "filter_settings";
        allowdenylist_json["data"] = nlohmann::json::object();
        allowdenylist_json["data"]["type"] = "allow";
        allowdenylist_json["data"]["mark"] = this->filter.mark;
        allowdenylist_json["data"]["conditions"] = this->filter.data;
      } else if (this->filter.mode == DENY) {
        allowdenylist_json["type"] = "filter_settings";
        allowdenylist_json["data"] = nlohmann::json::object();
        allowdenylist_json["data"]["type"] = "deny";
        allowdenylist_json["data"]["mark"] = this->filter.mark;
        allowdenylist_json["data"]["conditions"] = this->filter.data;
      } else if (this->filter.mode == PYTHON) {
        allowdenylist_json["type"] = "filter_settings";
        allowdenylist_json["data"] = nlohmann::json::object();
        allowdenylist_json["data"]["type"] = "python";
        allowdenylist_json["data"]["mark"] = this->filter.mark;
        allowdenylist_json["data"]["script"] = this->filter.script_path;
      }

      this->connections[0]->write(allowdenylist_json.dump());
    }

    this->connections[0]->write("<STOP>", true);
  }

  unsigned int Perf::get_thread_count() {
    if (this->perf_event.name == "<thread_tree>") {
      return 2;
    } else {
      return this->cpu_config.get_profiler_thread_count() + 1;
    }
  }

  void Perf::resume() {
    // TODO
  }

  void Perf::pause() {
    // TODO
  }

  int Perf::wait() {
    return this->process.get();
  }

  std::vector<std::unique_ptr<Requirement> > &Perf::get_requirements() {
    return this->requirements;
  }
};
