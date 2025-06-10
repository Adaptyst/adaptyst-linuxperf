#ifndef LINUXPERF_PROFILING_HPP_
#define LINUXPERF_PROFILING_HPP_

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <typeindex>
#include <typeinfo>
#include <filesystem>
#include <queue>
#include <sched.h>
#include <thread>
#include <future>
#include <adaptyst/socket.hpp>
#include <adaptyst/process.hpp>

namespace adaptyst {
  namespace fs = std::filesystem;

  /**
     A class describing a requirement of a profiler that needs to be
     satisfied before the profiler is used.
  */
  class Requirement {
    inline static std::unordered_map<std::type_index, bool> already_checked;

  protected:
    /**
       Determines whether the requirement is satisfied (internal method
       called by check()).

       This is an internal method which should *always* perform the check
       and return its result.
    */
    virtual bool check_internal() = 0;

  public:
    /**
       Gets the name of the requirement (e.g. for diagnostic purposes).
    */
    virtual std::string get_name() = 0;

    /**
       Determines whether the requirement is satisfied.

       On the first call, the check is performed and its result is
       cached. On all subsequent calls, the cached result
       is returned immediately, regardless of how many objects of a
       given Requirement-derived class are constructed.
    */
    bool check() {
      std::type_index index(typeid(*this));
      if (Requirement::already_checked.find(index) ==
          Requirement::already_checked.end()) {
        Requirement::already_checked[index] = this->check_internal();
      }

      return Requirement::already_checked[index];
    }
  };

  /**
     A class describing the requirement of the correct
     "perf"-specific kernel settings.

     At the moment, this is only kernel.perf_event_max_stack.
  */
  class PerfEventKernelSettingsReq : public Requirement {
  private:
    int &max_stack;

  protected:
    bool check_internal();

  public:
    PerfEventKernelSettingsReq(int &max_stack);
    std::string get_name();
  };

  /**
     A class describing the requirement of having proper
     NUMA-specific mitigations.

     The behaviour of this class depends on whether
     Adaptyst is compiled with libnuma support.
  */
  class NUMAMitigationReq : public Requirement {
  protected:
    bool check_internal();

  public:
    std::string get_name();
  };

  /**
     A class describing adaptyst-server connection instructions
     for profilers, sent by adaptyst-server during the initial
     setup phase.
  */
  class ServerConnInstrs {
  private:
    std::string type;
    std::queue<std::string> methods;

  public:
    ServerConnInstrs(std::string all_connection_instrs);
    std::string get_instructions(int thread_count);
  };

  /**
     A class describing a profiler.
  */
  class Profiler {
  protected:
    Acceptor::Factory &acceptor_factory;
    std::vector<std::unique_ptr<Connection> > connections;
    unsigned int buf_size;

  public:
    /**
       Constructs a Profiler object with
       the acceptor factory used for instantiating acceptors
       establishing a connection for exchanging messages with the profiler.

       @param acceptor   The acceptor factory to use.
       @param buf_size   The buffer size for a connection that the
                         acceptor will accept.
    */
    Profiler(Acceptor::Factory &acceptor_factory,
             unsigned int buf_size) : acceptor_factory(acceptor_factory) {
      this->buf_size = buf_size;
    }

    virtual ~Profiler() { }

    /**
       Gets the name of this profiler instance.
    */
    virtual std::string get_name() = 0;

    /**
       Starts the profiler and establishes the message connection(s).

       @param pid                 The PID of a process the profiler should
                                  be attached to. This may be left unused by
                                  classes deriving from Profiler.
       @param capture_immediately Indicates whether event capturing should
                                  become immediately after starting the profiler.
                                  If set to false, the call to start() must be
                                  followed by the call to resume() at some point.
    */
    virtual void start(pid_t pid,
                       bool capture_immediately) = 0;

    /**
       Resumes event capturing by the profiler.

       This is used for implementing partial profiling of the command.
    */
    virtual void resume() = 0;

    /**
       Pauses event capturing by the profiler.

       This is used for implementing partial profiling of the command.
    */
    virtual void pause() = 0;

    /**
       Waits for the profiler to finish executing and returns its exit code.
    */
    virtual int wait() = 0;

    /**
       Gets the number of threads the profiler is expected to use.
    */
    virtual unsigned int get_thread_count() = 0;

    /**
       Gets the list of requirements that must be satisfied for the profiler
       to run.
    */
    virtual std::vector<std::unique_ptr<Requirement> > &get_requirements() = 0;

    /**
       Gets the connections used for exchanging messages with
       the profiler. The first connection in the vector is used for
       exchanging *generic* messages.

       WARNING: An empty vector will be returned if start() hasn't been
       called before.
    */
    std::vector<std::unique_ptr<Connection> > &get_connections() {
      return this->connections;
    }
  };

  /**
     A class describing a Linux "perf" event, used
     by the Perf class.
  */
  class PerfEvent {
  private:
    std::string name;
    std::vector<std::string> options;
    std::string human_title;
    std::string unit;

  public:
    friend class Perf;

    // For thread tree profiling
    PerfEvent();

    // For main profiling
    PerfEvent(int freq,
              int off_cpu_freq,
              int buffer_events,
              int buffer_off_cpu_events);

    // For custom event profiling
    PerfEvent(std::string name,
              int period,
              int buffer_events,
              std::string human_title,
              std::string unit);

    std::string get_name();
    std::string get_human_title();
    std::string get_unit();
  };

  /**
     A class describing a Linux "perf" profiler.
  */
  class Perf : public Profiler {
  public:
    enum CaptureMode {
      KERNEL,
      USER,
      BOTH
    };

    enum FilterMode {
      ALLOW,
      DENY,
      PYTHON,
      NONE
    };

    struct Filter {
      FilterMode mode;
      bool mark;
      std::variant<fs::path,
                   std::vector<std::vector<std::string> > > data;
    };

  private:
    fs::path perf_bin_path;
    fs::path perf_python_path;
    std::future<int> process;
    PerfEvent perf_event;
    CPUConfig &cpu_config;
    std::string name;
    std::vector<std::unique_ptr<Requirement> > requirements;
    int max_stack;
    std::unique_ptr<Process> record_proc;
    std::unique_ptr<Process> script_proc;
    CaptureMode capture_mode;
    Filter filter;
    bool running;

  public:
    Perf(Acceptor::Factory &acceptor_factory,
         unsigned int buf_size,
         fs::path perf_bin_path,
         fs::path perf_python_path,
         PerfEvent &perf_event,
         CPUConfig &cpu_config,
         std::string name,
         CaptureMode capture_mode,
         Filter filter);
    ~Perf() {}
    std::string get_name();
    void start(pid_t pid,
               bool capture_immediately);
    unsigned int get_thread_count();
    void resume();
    void pause();
    int wait();
    std::vector<std::unique_ptr<Requirement> > &get_requirements();
  };
};

#endif
