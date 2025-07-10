# This script uses the perf-script Python API.
# See the man page for perf-script-python for learning how the API works.

import os
import sys
import subprocess
import json
import re
import socket
import importlib.util
import select
from cxxfilt import demangle
from bisect import bisect_left
from pathlib import Path
from collections import defaultdict

sys.path.append(os.environ['PERF_EXEC_PATH'] +
                '/scripts/python/Perf-Trace-Util/lib/Perf/Trace')

from perf_trace_context import *
from Core import *

allowed_chars = 'abcdefghijklmnopqrstuvwxyz' + \
    'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'
cur_code_sym = [0]


def next_code(cur_code):
    res = ''.join(map(lambda x: allowed_chars[x], cur_code))

    for i in range(len(cur_code)):
        cur_code[i] += 1

        if cur_code[i] < len(allowed_chars):
            break
        else:
            cur_code[i] = len(allowed_chars) - 1

            if i == len(cur_code) - 1:
                cur_code.append(0)

    return res


event_streams = []
next_index = 0
symbol_dict = defaultdict(lambda: next_code(cur_code_sym))
dso_dict = defaultdict(set)
overall_event_type = None
perf_maps = {}
filter_settings = None


def get_next_event_stream():
    global event_streams, next_index
    stream = event_streams[next_index]
    next_index = (next_index + 1) % len(event_streams)
    return stream


event_stream_dict = defaultdict(lambda: defaultdict(get_next_event_stream))
frontend_stream = None


# import_from_path is from
# https://docs.python.org/3/library/importlib.html#importing-a-source-file-directly
def import_from_path(module_name, file_path):
    spec = importlib.util.spec_from_file_location(module_name, file_path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def write(stream, msg):
    if isinstance(stream, socket.socket):
        stream.sendall((msg + '\n').encode('utf-8'))
    else:
        if 'b' in stream.mode:
            stream.write((msg + '\n').encode('utf-8'))
        else:
            stream.write(msg + '\n')

        stream.flush()


def find_in_map(map_path, map_id, ip):
    global perf_maps

    if map_id not in perf_maps:
        if map_path.exists():
            perf_maps[map_id] = (map_path,
                                 map_path.open(mode='r'), [0], [])
        else:
            perf_maps[map_id] = (map_path, None, None, None)
            return None

    _, f, i, groups = perf_maps[map_id]

    if f is None:
        return None

    for group in groups:
        index = bisect_left(group, ip,
                            key=lambda x: x[0])

        if index < len(group) and group[index][0] <= ip and \
           group[index][0] + group[index][1] > ip:
            return group[index][2]

    ready, _, _ = select.select([f], [], [], 0)
    to_add = []

    while f in ready:
        line = next(f).strip()
        i[0] += 1

        match = re.search(
            r'^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(.+)$', line)

        if match is None:
            print(f'Line {i[0]}, {map_path}: '
                  'incorrect syntax, ignoring.',
                  file=sys.stderr)
            ready, _, _ = select.select([f], [], [], 0)
            continue

        data = (int(match.group(1), 16),
                int(match.group(2), 16),
                demangle(match.group(3), False))

        to_add.append(data)

        if data[0] <= ip and data[0] + data[1] > ip:
            if len(to_add) > 0:
                to_add.sort(key=lambda x: x[0])
                perf_maps[map_id][-1].append(to_add)

            return data[2]

        ready, _, _ = select.select([f], [], [], 0)

    if len(to_add) > 0:
        to_add.sort(key=lambda x: x[0])
        perf_maps[map_id][-1].append(to_add)

    return None


def trace_begin():
    global event_streams, frontend_stream, filter_settings

    connect = os.environ['ADAPTYST_CONNECT'].split(' ')
    frontend_parts = connect[1].split('_')

    if connect[0] == 'pipe':
        stream = os.fdopen(int(frontend_parts[1]), 'w')
        stream.write('connect')
        stream.flush()
        frontend_stream = stream

    instrs = connect[2:]

    for i in instrs:
        parts = i.split('_')
        if connect[0] == 'tcp':
            stream = socket.socket()
            stream.connect((parts[0], int(parts[1])))
            event_streams.append(stream)
        elif connect[0] == 'pipe':
            stream = os.fdopen(int(parts[1]), 'wb')
            stream.write('connect'.encode('ascii'))
            stream.flush()
            event_streams.append(stream)

    frontend_stream_read = os.fdopen(int(frontend_parts[0]), 'r')
    for line in frontend_stream_read:
        line = line.strip()

        if line == '<STOP>':
            break

        command = json.loads(line)

        if command['type'] == 'filter_settings':
            filter_settings = command['data']

            if filter_settings['type'] == 'python':
                filter_settings['module'] = \
                    import_from_path('module',
                                     filter_settings['script'])
                filter_settings['module'].setup()

    frontend_stream_read.close()


# Callchain symbol names are attempted to be obtained here. In case of
# failure, an instruction address is put instead, along with
# the name of an executable/library if available.
#
# If obtained, symbol names are compressed to save memory.
# The dictionary mapping compressed names to full ones
# is saved at the end of profiling (see reverse_callchain_dict in
# trace_end()).
def process_callchain_elem(elem):
    sym_result = [f'[{elem["ip"]:#x}]', '']
    sym_result_set = False
    off_result = hex(elem['ip'])

    if 'dso' in elem:
        p = Path(elem['dso'])
        perf_map_match = re.search(r'^perf\-(\d+)\.map$', p.name)
        if perf_map_match is not None:
            if 'sym' in elem and 'name' in elem['sym']:
                sym_result[0] = demangle(elem['sym']['name'], False)
                sym_result_set = True
            else:
                result = find_in_map(p, perf_map_match.group(1), elem['ip'])
                if result is None:
                    sym_result[0] = f'[{elem["dso"]}]'
                else:
                    sym_result[0] = result
                    sym_result_set = True
        else:
            dso_dict[elem['dso']].add(hex(elem['dso_off']))
            sym_result[0] = f'[{elem["dso"]}]'
            off_result = hex(elem['dso_off'])

        sym_result[1] = elem['dso']

    if not sym_result_set and \
       'sym' in elem and 'name' in elem['sym']:
        sym_result[0] = elem['sym']['name']

    return tuple(sym_result), off_result


def process_event(param_dict):
    global event_stream_dict, overall_event_type, perf_map_paths

    event_type = param_dict['ev_name']
    comm = param_dict['comm']
    pid = param_dict['sample']['pid']
    tid = param_dict['sample']['tid']
    timestamp = param_dict['sample']['time']
    period = param_dict['sample']['period']
    raw_callchain = param_dict['callchain']

    parsed_event_type = re.search(r'^([^/]+)', event_type).group(1)

    if overall_event_type is None:
        if parsed_event_type in ['task-clock', 'offcpu-time']:
            overall_event_type = 'walltime'
        else:
            overall_event_type = parsed_event_type

    callchain_tmp = tuple(map(process_callchain_elem, raw_callchain))

    if filter_settings is None:
        callchain = [(symbol_dict[s], o) for s, o
                     in reversed(callchain_tmp)]
    else:
        callchain = []

        def satisfy_conditions(sym_result, conditions):
            for group in conditions:
                matched = True
                for cond in group:
                    match = re.search(r'^(SYM|EXEC|ANY) (.+)$',
                                      cond)

                    cond_type = match.group(1)
                    regex = match.group(2)

                    if cond_type == 'SYM':
                        if re.search(regex, sym_result[0]) is None:
                            matched = False
                            break
                    elif cond_type == 'EXEC':
                        if re.search(regex, sym_result[1]) is None:
                            matched = False
                            break
                    elif cond_type == 'ANY':
                        if re.search(regex, sym_result[0]) is None and \
                           re.search(regex, sym_result[1]) is None:
                            matched = False
                            break

                if matched:
                    return True

            return False

        if filter_settings['type'] == 'python':
            accepted = filter_settings['module'].process(callchain_tmp)

            if accepted is None or not isinstance(accepted, list) or \
               len(accepted) != len(callchain_tmp):
                raise RuntimeError('Invalid value of process() from the ' +
                                   'provided Python script: it is not ' +
                                   f'a list of size {len(callchain_tmp)}')
        else:
            accepted = None

        last_cut = False
        for i, (sym_result, off_result) in enumerate(callchain_tmp):
            if accepted is None:
                satisfied = satisfy_conditions(sym_result,
                                               filter_settings['conditions'])
            else:
                if not isinstance(accepted[i], bool):
                    raise RuntimeError('Invalid value of process() from the ' +
                                       'provided Python script: a non-boolean ' +
                                       f'element at index {i}')

                satisfied = accepted[i]

            if (filter_settings['type'] in ['python', 'allow'] and satisfied) or \
               (filter_settings['type'] == 'deny' and not satisfied):
                callchain.append((symbol_dict[sym_result], off_result))
                last_cut = False
            elif filter_settings['mark'] and not last_cut:
                callchain.append((symbol_dict[('(cut)', '')], ''))
                last_cut = True

        callchain = callchain[::-1]

    write(event_stream_dict[pid][tid], json.dumps({
        'type': 'sample',
        'data': {
            'event_type': parsed_event_type,
            'pid': str(pid),
            'tid': str(tid),
            'time': timestamp,
            'period': period,
            'callchain': callchain
        }
    }))


def trace_end():
    global event_streams, callchain_dict, overall_event_type, perf_map_paths, \
        perf_maps

    for stream in event_streams:
        write(stream, '<STOP>')
        stream.close()

    reverse_symbol_dict = {v: k for k, v in symbol_dict.items()}

    write(frontend_stream, json.dumps({
        'type': 'callchains',
        'data': reverse_symbol_dict
    }))

    write(frontend_stream, json.dumps({
        'type': 'sources',
        'data': {k: list(v) for k, v in dso_dict.items()}
    }))

    missing_maps = []

    for map_path, f, _, _ in perf_maps.values():
        if f is None:
            missing_maps.append(str(map_path))
        else:
            f.close()

    write(frontend_stream, json.dumps({
        'type': 'missing_symbol_maps',
        'data': missing_maps
    }))

    write(frontend_stream, '<STOP>')
    frontend_stream.close()


def syscall_callback(stack, ret_value):
    global perf_map_paths, dso_dict, event_stream_dict

    if int(ret_value) == 0:
        return

    callchain_tmp = tuple(map(process_callchain_elem, stack))

    if filter_settings is None:
        callchain = [(symbol_dict[s], o) for s, o in callchain_tmp]
    else:
        callchain = []

        def satisfy_conditions(sym_result, conditions):
            for group in conditions:
                matched = True
                for cond in group:
                    match = re.search(r'^(SYM|EXEC|ANY) (.+)$',
                                      cond)

                    cond_type = match.group(1)
                    regex = match.group(2)

                    if cond_type == 'SYM':
                        if re.search(regex, sym_result[0]) is None:
                            matched = False
                            break
                    elif cond_type == 'EXEC':
                        if re.search(regex, sym_result[1]) is None:
                            matched = False
                            break
                    elif cond_type == 'ANY':
                        if re.search(regex, sym_result[0]) is None and \
                           re.search(regex, sym_result[1]) is None:
                            matched = False
                            break

                if matched:
                    return True

            return False

        if filter_settings['type'] == 'python':
            accepted = filter_settings['module'].process(callchain_tmp)

            if accepted is None or not isinstance(accepted, list) or \
               len(accepted) != len(callchain_tmp):
                raise RuntimeError('Invalid value of process() from the ' +
                                   'provided Python script: it is not ' +
                                   f'a list of size {len(callchain_tmp)}')
        else:
            accepted = None

        last_cut = False
        for i, (sym_result, off_result) in enumerate(callchain_tmp):
            if accepted is None:
                satisfied = satisfy_conditions(sym_result,
                                               filter_settings['conditions'])
            else:
                if not isinstance(accepted[i], bool):
                    raise RuntimeError('Invalid value of process() from the ' +
                                       'provided Python script: a non-boolean ' +
                                       f'element at index {i}')

                satisfied = accepted[i]

            if (filter_settings['type'] in ['python', 'allow'] and satisfied) or \
               (filter_settings['type'] == 'deny' and not satisfied):
                callchain.append((symbol_dict[sym_result], off_result))
                last_cut = False
            elif filter_settings['mark'] and not last_cut:
                callchain.append((symbol_dict[('(cut)', '')], ''))
                last_cut = True

    write(event_stream_dict[0][0], json.dumps({
        'type': 'syscall',
        'data': {
            'ret_value': str(ret_value),
            'callchain': callchain
        }
    }))


def syscall_tree_callback(syscall_type, comm_name, pid, tid, time,
                          ret_value):
    write(event_stream_dict[0][0], json.dumps({
        'type': 'syscall_meta',
        'data': {
            'subtype': syscall_type,
            'comm': comm_name,
            'pid': str(pid),
            'tid': str(tid),
            'time': time,
            'ret_value': str(ret_value)
        }
    }))


def sched__sched_process_fork(event_name, context, common_cpu,
	                          common_secs, common_nsecs, common_pid,
                              common_comm, common_callchain, parent_comm,
                              parent_pid, child_comm, child_pid,
		                      perf_sample_dict):
    syscall_callback(perf_sample_dict['callchain'], child_pid)
    syscall_tree_callback('new_proc', common_comm, perf_sample_dict['sample']['pid'],
                          perf_sample_dict['sample']['tid'], perf_sample_dict['sample']['time'],
                          child_pid)


def sched__sched_process_exit(event_name, context, common_cpu,
	                          common_secs, common_nsecs, common_pid,
                              common_comm, common_callchain, comm, pid,
                              prio, perf_sample_dict):
    syscall_tree_callback('exit', common_comm, perf_sample_dict['sample']['pid'],
                          perf_sample_dict['sample']['tid'],
                          perf_sample_dict['sample']['time'], 0)


def syscalls__sys_exit_execve(event_name, context, common_cpu, common_secs,
                              common_nsecs, common_pid, common_comm,
                              common_callchain, __syscall_nr, ret,
                              perf_sample_dict):
    if ret == 0:
        syscall_tree_callback('execve', common_comm, perf_sample_dict['sample']['pid'],
                              perf_sample_dict['sample']['tid'],
                              perf_sample_dict['sample']['time'], ret)


def syscalls__sys_exit_execveat(event_name, context, common_cpu, common_secs,
                                common_nsecs, common_pid, common_comm,
                                common_callchain, __syscall_nr, ret,
                                perf_sample_dict):
    if ret == 0:
        syscall_tree_callback('execve', common_comm, perf_sample_dict['sample']['pid'],
                              perf_sample_dict['sample']['tid'],
                              perf_sample_dict['sample']['time'], ret)
